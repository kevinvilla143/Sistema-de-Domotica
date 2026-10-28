using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using Microsoft.AspNetCore.HttpOverrides;

var builder = WebApplication.CreateBuilder(args);

// Puerto por defecto 5036 en todas las interfaces (así funciona en el servidor Ubuntu sin launchSettings).
// Se puede cambiar con ASPNETCORE_URLS (p. ej. http://127.0.0.1:5036 para aceptar solo a ngrok) o --urls.
if (string.IsNullOrEmpty(builder.Configuration["urls"]) &&
    string.IsNullOrEmpty(builder.Configuration["http_ports"]) &&
    string.IsNullOrEmpty(builder.Configuration["https_ports"]))
{
    builder.WebHost.UseUrls("http://0.0.0.0:5036");
}

var esp32BaseUrl = builder.Configuration["Esp32:BaseUrl"] ?? "http://192.168.1.112/";
if (!Uri.TryCreate(esp32BaseUrl, UriKind.Absolute, out var esp32Uri) ||
    (esp32Uri.Scheme != Uri.UriSchemeHttp && esp32Uri.Scheme != Uri.UriSchemeHttps))
{
    throw new InvalidOperationException("Esp32:BaseUrl debe ser una URL HTTP/HTTPS válida.");
}
esp32BaseUrl = esp32Uri.ToString().TrimEnd('/') + "/";
esp32Uri = new Uri(esp32BaseUrl);

var esp32Timeout = TimeSpan.FromSeconds(Math.Clamp(builder.Configuration.GetValue("Esp32:TimeoutSeconds", 6), 2, 30));

// Lectura compartida: varios navegadores reutilizan la misma respuesta del ESP32 durante este tiempo,
// así 1 o 5 dispositivos abiertos generan prácticamente la misma carga sobre el microcontrolador.
var statusCacheTtl = TimeSpan.FromMilliseconds(Math.Clamp(builder.Configuration.GetValue("Esp32:CacheMilliseconds", 2500), 0, 30000));
var maxSessions = Math.Clamp(builder.Configuration.GetValue("Esp32:MaxSessions", 20), 1, 500);

// Esperas por turno para hablar con el ESP32. Son menores que el tiempo máximo del navegador (ver app.js),
// de modo que el servidor siempre responde con un error claro antes de que el navegador se rinda.
var statusGateWait = TimeSpan.FromSeconds(4);
var commandGateWait = TimeSpan.FromSeconds(7);   // login, luces y logout son acciones del usuario: esperan más

// Detrás de ngrok (agente en este mismo equipo) el servidor ve HTTP; con esto reconoce el HTTPS
// original y marca como Secure la cookie de sesión. Solo se confía en proxies de loopback.
builder.Services.Configure<ForwardedHeadersOptions>(o =>
    o.ForwardedHeaders = ForwardedHeaders.XForwardedFor | ForwardedHeaders.XForwardedProto | ForwardedHeaders.XForwardedHost);

// Cada navegador recibe su propio CookieContainer. Así, cada dispositivo mantiene
// su propia sesión sid del ESP32 y cerrar sesión en uno no afecta a los demás.
var browserSessions = new ConcurrentDictionary<string, Esp32ProxySession>(StringComparer.Ordinal);
var esp32RequestGate = new SemaphoreSlim(1, 1);
var statusCache = new StatusCache();
long lastCleanupTicks = DateTime.UtcNow.Ticks;

var app = builder.Build();

app.UseForwardedHeaders();

// Cabeceras de seguridad y sin caché en la API (el estado de las luces cambia a cada momento).
app.Use(async (ctx, next) =>
{
    var h = ctx.Response.Headers;
    h["X-Content-Type-Options"] = "nosniff";
    h["Referrer-Policy"] = "no-referrer";
    h["X-Frame-Options"] = "DENY";
    h["Content-Security-Policy"] = "default-src 'self'; img-src 'self' data:; frame-ancestors 'none'";
    if (ctx.Request.Path.StartsWithSegments("/api")) h.CacheControl = "no-store";
    await next();
});

// Contraseña OPCIONAL delante de toda la web (Auth__Password). Con ngrok público evita que cualquiera
// llegue al formulario de login del ESP32 (que se bloquea 1 min tras 5 intentos fallidos para TODOS).
var basicUser = builder.Configuration["Auth:User"] ?? "admin";
var basicPassword = builder.Configuration["Auth:Password"] ?? string.Empty;
if (!string.IsNullOrEmpty(basicPassword))
{
    var expected = SHA256.HashData(Encoding.UTF8.GetBytes($"{basicUser}:{basicPassword}"));
    app.Use(async (ctx, next) =>
    {
        if (ctx.Request.Path.StartsWithSegments("/healthz")) { await next(); return; }

        if (AuthenticationHeaderValue.TryParse(ctx.Request.Headers.Authorization, out var header) &&
            header.Scheme.Equals("Basic", StringComparison.OrdinalIgnoreCase) &&
            header.Parameter is { Length: > 0 } parameter)
        {
            try
            {
                var credentials = Encoding.UTF8.GetString(Convert.FromBase64String(parameter));
                var actual = SHA256.HashData(Encoding.UTF8.GetBytes(credentials));
                if (CryptographicOperations.FixedTimeEquals(actual, expected)) { await next(); return; }
            }
            catch (FormatException) { /* base64 inválido: credenciales incorrectas */ }

            app.Logger.LogWarning("Contraseña de ENTRADA incorrecta desde {Ip}", ClientIp(ctx));
            await Task.Delay(500);   // frena un poco la fuerza bruta
        }

        ctx.Response.StatusCode = StatusCodes.Status401Unauthorized;
        ctx.Response.Headers.WWWAuthenticate = "Basic realm=\"Domotica ESP32\", charset=\"UTF-8\"";
    });
}
else
{
    app.Logger.LogWarning("Auth:Password está vacío: la web NO tiene contraseña propia. Si la expones con ngrok, " +
                          "cualquiera podrá intentar iniciar sesión en el ESP32. Define Auth__Password.");
}

app.UseDefaultFiles();
app.UseStaticFiles();

app.MapGet("/healthz", () => Results.Text("ok"));   // comprobación del servicio (no consulta al ESP32)

// GET /api/status: lecturas del firmware. Solo responde a navegadores que iniciaron sesión con el ESP32
// y reutiliza una lectura reciente compartida, para no saturar al microcontrolador.
app.MapGet("/api/status", async (HttpContext context) =>
{
    var session = FindBrowserSession(context);
    if (session is null || !session.LoggedIn)
        return Results.Json(new { error = "No autorizado" }, statusCode: 401);

    if (statusCache.GetFresh(statusCacheTtl) is { } cached)
        return Results.Content(cached, "application/json; charset=utf-8");

    if (!await esp32RequestGate.WaitAsync(statusGateWait, context.RequestAborted))
        return Results.Problem(title: "El ESP32 está ocupado; vuelve a intentarlo.", statusCode: 503);

    try
    {
        if (statusCache.GetFresh(statusCacheTtl) is { } fresh)   // otro navegador la obtuvo mientras esperábamos
            return Results.Content(fresh, "application/json; charset=utf-8");

        using var response = await session.Client.GetAsync("api/status", context.RequestAborted);
        var body = await response.Content.ReadAsStringAsync(context.RequestAborted);

        if (response.IsSuccessStatusCode) statusCache.Set(body);
        else if (response.StatusCode == HttpStatusCode.Unauthorized) ExpireSession(context, session);

        return Results.Content(body, "application/json; charset=utf-8", statusCode: (int)response.StatusCode);
    }
    catch (OperationCanceledException) when (!context.RequestAborted.IsCancellationRequested)
    {
        return Results.Problem(title: "Tiempo de espera agotado al consultar el ESP32.", statusCode: 504);
    }
    catch (HttpRequestException ex)
    {
        return Results.Problem(title: "No se pudo conectar con el ESP32.", detail: ex.Message, statusCode: 502);
    }
    finally { esp32RequestGate.Release(); }
});

// POST /api/login: el firmware confirma el acceso con HTTP 302 y la cookie sid.
// La sesión del servidor SOLO se conserva si el ESP32 acepta las credenciales: quien no se autentica no ocupa memoria.
app.MapPost("/api/login", async (HttpContext context, LoginRequest login) =>
{
    var ip = ClientIp(context);
    var (key, session, isNew) = GetOrCreateBrowserSession(context);

    if (!await esp32RequestGate.WaitAsync(commandGateWait, context.RequestAborted))
    {
        if (isNew) browserSessions.TryRemove(key, out _);
        return Results.Problem(title: "El ESP32 está ocupado; vuelve a intentarlo.", statusCode: 503);
    }

    try
    {
        using var content = new FormUrlEncodedContent(new[]
        {
            new KeyValuePair<string, string>("usuario", login.Usuario ?? string.Empty),
            new KeyValuePair<string, string>("clave", login.Clave ?? string.Empty),
            new KeyValuePair<string, string>("next", "/")
        });
        using var response = await session.Client.PostAsync("login", content, context.RequestAborted);
        var body = await response.Content.ReadAsStringAsync(context.RequestAborted);
        var sid = session.Cookies.GetCookies(esp32Uri)["sid"];

        if ((int)response.StatusCode == 302 && sid is not null && !string.IsNullOrWhiteSpace(sid.Value))
        {
            session.LoggedIn = true;
            statusCache.Clear();
            app.Logger.LogInformation("Inicio de sesión en el ESP32 CORRECTO desde {Ip} (sesiones activas: {Count})",
                ip, browserSessions.Values.Count(x => x.LoggedIn));
            return Results.Ok(new { ok = true, mensaje = "Inicio de sesión aceptado por el ESP32." });
        }

        app.Logger.LogWarning("Inicio de sesión en el ESP32 FALLIDO desde {Ip}", ip);
        if (isNew) browserSessions.TryRemove(key, out _);

        return Results.Json(new
        {
            ok = false,
            mensaje = "El ESP32 no aceptó el inicio de sesión. Verifica usuario y contraseña.",
            codigoHttpESP32 = (int)response.StatusCode,
            detalle = body.Length > 500 ? body[..500] : body
        }, statusCode: 401);
    }
    catch (OperationCanceledException) when (!context.RequestAborted.IsCancellationRequested)
    {
        if (isNew) browserSessions.TryRemove(key, out _);
        return Results.Problem(title: "Tiempo de espera agotado al iniciar sesión.", statusCode: 504);
    }
    catch (HttpRequestException ex)
    {
        if (isNew) browserSessions.TryRemove(key, out _);
        return Results.Problem(title: "No se pudo conectar con el ESP32.", detail: ex.Message, statusCode: 502);
    }
    finally { esp32RequestGate.Release(); }
});

// POST /api/light: coincide con handleApiLuz() del Arduino (luz 1..3 y estado booleano).
app.MapPost("/api/light", async (HttpContext context, LightRequest command) =>
{
    if (command.Luz is < 1 or > 3)
        return Results.BadRequest(new { ok = false, mensaje = "El número de luz debe ser 1, 2 o 3." });

    var session = FindBrowserSession(context);
    if (session is null || !session.LoggedIn)
        return Results.Json(new { ok = false, mensaje = "Inicia sesión para controlar las luces." }, statusCode: 401);

    if (!await esp32RequestGate.WaitAsync(commandGateWait, context.RequestAborted))
        return Results.Problem(title: "El ESP32 está ocupado; vuelve a intentarlo.", statusCode: 503);

    try
    {
        // Enviar los parámetros en la URL, no en el cuerpo. Esto evita el problema
        // del WebServer del ESP32 que estaba recibiendo el POST sin el argumento "plain".
        var estadoTexto = command.Estado ? "true" : "false";
        var endpoint = $"api/light?luz={command.Luz}&estado={estadoTexto}";
        using var request = new HttpRequestMessage(HttpMethod.Post, endpoint);
        request.Headers.ExpectContinue = false;
        using var response = await session.Client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, context.RequestAborted);
        var body = await response.Content.ReadAsStringAsync(context.RequestAborted);

        statusCache.Clear();   // la próxima lectura debe ser el estado real, no una copia guardada

        if (response.StatusCode == HttpStatusCode.Unauthorized)
        {
            ExpireSession(context, session);
            return Results.Json(new { ok = false, mensaje = "La sesión del ESP32 venció. Inicia sesión nuevamente.", detalle = body }, statusCode: 401);
        }

        if (!response.IsSuccessStatusCode)
            return Results.Json(new { ok = false, mensaje = "El ESP32 rechazó el comando.", codigoHttpESP32 = (int)response.StatusCode, detalle = body }, statusCode: 502);

        app.Logger.LogInformation("Luz {Luz} -> {Estado} desde {Ip}", command.Luz, command.Estado ? "ENCENDIDA" : "APAGADA", ClientIp(context));
        return Results.Content(body, "application/json; charset=utf-8", statusCode: (int)response.StatusCode);
    }
    catch (OperationCanceledException) when (!context.RequestAborted.IsCancellationRequested)
    {
        return Results.Problem(title: "Tiempo de espera agotado al controlar la luz.", statusCode: 504);
    }
    catch (HttpRequestException ex)
    {
        return Results.Problem(title: "No se pudo conectar con el ESP32.", detail: ex.Message, statusCode: 502);
    }
    finally { esp32RequestGate.Release(); }
});

// POST /api/logout: llama al GET /logout del firmware, que elimina solo la sid de este navegador.
app.MapPost("/api/logout", async (HttpContext context) =>
{
    var session = FindBrowserSession(context);
    if (session is null) return Results.Ok(new { ok = true });   // no había sesión que cerrar

    if (!await esp32RequestGate.WaitAsync(commandGateWait, context.RequestAborted))
        return Results.Problem(title: "El ESP32 está ocupado; vuelve a intentarlo.", statusCode: 503);

    try
    {
        using var response = await session.Client.GetAsync("logout", context.RequestAborted);
        var body = await response.Content.ReadAsStringAsync(context.RequestAborted);
        if (response.IsSuccessStatusCode || (int)response.StatusCode == 302)
        {
            session.LoggedIn = false;
            RemoveSession(context);
            app.Logger.LogInformation("Sesión cerrada desde {Ip} (sesiones activas: {Count})",
                ClientIp(context), browserSessions.Values.Count(x => x.LoggedIn));
            return Results.Ok(new { ok = true });
        }
        return Results.Json(new { ok = false, mensaje = "No se pudo cerrar la sesión del ESP32.", codigoHttpESP32 = (int)response.StatusCode, detalle = body }, statusCode: 502);
    }
    catch (OperationCanceledException) when (!context.RequestAborted.IsCancellationRequested)
    {
        return Results.Problem(title: "Tiempo de espera agotado al cerrar sesión.", statusCode: 504);
    }
    catch (HttpRequestException ex)
    {
        return Results.Problem(title: "No se pudo conectar con el ESP32.", detail: ex.Message, statusCode: 502);
    }
    finally { esp32RequestGate.Release(); }
});

const string SessionCookie = "domotica_proxy_sid";

// Dirección real del visitante (ngrok la envía en X-Forwarded-For; UseForwardedHeaders la aplica).
string ClientIp(HttpContext context)
{
    var ip = context.Connection.RemoteIpAddress;
    if (ip is null) return "desconocida";
    if (ip.IsIPv4MappedToIPv6) ip = ip.MapToIPv4();
    return ip.ToString();
}

// Devuelve la sesión existente de este navegador, o null (no crea nada).
Esp32ProxySession? FindBrowserSession(HttpContext context)
{
    CleanupExpiredSessions();
    var key = context.Request.Cookies[SessionCookie];
    if (string.IsNullOrWhiteSpace(key) || key.Length > 100 || !browserSessions.TryGetValue(key, out var session))
        return null;
    session.Touch();
    return session;
}

// Solo se usa en el login. Con el tope de sesiones, expulsa primero las que no están autenticadas y las más antiguas.
(string Key, Esp32ProxySession Session, bool IsNew) GetOrCreateBrowserSession(HttpContext context)
{
    CleanupExpiredSessions();
    var key = context.Request.Cookies[SessionCookie];
    if (!string.IsNullOrWhiteSpace(key) && key.Length <= 100 && browserSessions.TryGetValue(key, out var existing))
    {
        existing.Touch();
        return (key, existing, false);
    }

    while (browserSessions.Count >= maxSessions)
    {
        var victim = browserSessions.OrderBy(p => p.Value.LoggedIn).ThenBy(p => p.Value.LastAccessUtc).First();
        if (browserSessions.TryRemove(victim.Key, out _))
            app.Logger.LogWarning("Tope de {Max} sesiones alcanzado: se descartó la más antigua.", maxSessions);
    }

    key = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
    var session = new Esp32ProxySession(esp32BaseUrl, esp32Timeout);
    browserSessions[key] = session;
    context.Response.Cookies.Append(SessionCookie, key, new CookieOptions
    {
        HttpOnly = true,
        IsEssential = true,
        Secure = context.Request.IsHttps,
        SameSite = SameSiteMode.Lax,
        Path = "/",
        MaxAge = TimeSpan.FromHours(8)
    });
    return (key, session, true);
}

void RemoveSession(HttpContext context)
{
    var key = context.Request.Cookies[SessionCookie];
    if (!string.IsNullOrWhiteSpace(key)) browserSessions.TryRemove(key, out _);
}

// El ESP32 ya no reconoce la sesión (30 min de inactividad o reinicio del equipo).
void ExpireSession(HttpContext context, Esp32ProxySession session)
{
    if (!session.LoggedIn) return;
    session.LoggedIn = false;
    app.Logger.LogWarning("El ESP32 cerró la sesión del visitante {Ip} (venció o el ESP32 se reinició)", ClientIp(context));
}

void CleanupExpiredSessions()
{
    var now = DateTime.UtcNow;
    var previous = new DateTime(Interlocked.Read(ref lastCleanupTicks), DateTimeKind.Utc);
    if (now - previous < TimeSpan.FromMinutes(5)) return;
    if (Interlocked.CompareExchange(ref lastCleanupTicks, now.Ticks, previous.Ticks) != previous.Ticks) return;

    foreach (var pair in browserSessions)
    {
        if (now - pair.Value.LastAccessUtc > TimeSpan.FromHours(8) &&
            browserSessions.TryRemove(pair.Key, out var removed))
            removed.Dispose();
    }
}

app.Logger.LogInformation("ESP32 configurado en {Url} (timeout {Timeout} s)", esp32BaseUrl, esp32Timeout.TotalSeconds);

app.Run();

record LoginRequest(string? Usuario, string? Clave);
record LightRequest(int Luz, bool Estado);

sealed class Esp32ProxySession : IDisposable
{
    public CookieContainer Cookies { get; } = new();
    public HttpClient Client { get; }
    /// <summary>true cuando el ESP32 aceptó las credenciales de este navegador.</summary>
    public volatile bool LoggedIn;
    private long _lastAccessTicks = DateTime.UtcNow.Ticks;
    public DateTime LastAccessUtc => new(Interlocked.Read(ref _lastAccessTicks), DateTimeKind.Utc);

    public Esp32ProxySession(string baseUrl, TimeSpan timeout)
    {
        var handler = new HttpClientHandler
        {
            CookieContainer = Cookies,
            UseCookies = true,
            AllowAutoRedirect = false
        };
        Client = new HttpClient(handler) { BaseAddress = new Uri(baseUrl), Timeout = timeout };
        Client.DefaultRequestHeaders.ConnectionClose = true;   // el WebServer del ESP32 atiende una conexión a la vez
    }

    public void Touch() => Interlocked.Exchange(ref _lastAccessTicks, DateTime.UtcNow.Ticks);
    public void Dispose() => Client.Dispose();
}

/// <summary>Última lectura buena del ESP32, compartida entre navegadores autenticados.</summary>
sealed class StatusCache
{
    private sealed record Snapshot(string Body, long Timestamp);
    private Snapshot? _snapshot;

    public string? GetFresh(TimeSpan ttl)
    {
        var s = Volatile.Read(ref _snapshot);
        return s is not null && Stopwatch.GetElapsedTime(s.Timestamp) < ttl ? s.Body : null;
    }

    public void Set(string body) => Volatile.Write(ref _snapshot, new Snapshot(body, Stopwatch.GetTimestamp()));
    public void Clear() => Volatile.Write(ref _snapshot, null);
}
