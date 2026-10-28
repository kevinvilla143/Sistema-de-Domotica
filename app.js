const $ = id => document.getElementById(id);
const lightStates = {1:false, 2:false, 3:false};
let loggedIn = false;
let pollTimer = null;
let statusInFlight = false;
let commandInFlight = false;
let lastStatusAt = 0;
const POLL_MS = 3000;

function setValue(id, value, suffix = "") {
  const el = $(id); if (!el) return;
  const n = Number(value);
  el.textContent = value === null || value === undefined || value === "" || !Number.isFinite(n) ? "--" : `${typeof value === "number" ? (Number.isInteger(value) ? value : Number(value.toFixed(1))) : value}${suffix}`;
}
function setMessage(id, message, kind = "") {
  const el = $(id); if (!el) return;
  el.textContent = message; el.className = `message ${kind}`.trim();
}
function setConnection(connected, message) {
  const el = $("connection"); if (el) { el.textContent = message; el.className = `status ${connected ? "online" : "offline"}`; }
  const dot = $("statusDot"); if (dot) dot.className = `status-dot ${connected ? "online" : "offline"}`;
  const diag = $("diagLed"); if (diag) diag.className = `diag-led ${connected ? "online" : "offline"}`;
  if ($("diagText")) $("diagText").textContent = connected ? "ESP32 conectado y respondiendo" : message;
}
function showLightControls(show) {
  loggedIn = show;
  if ($("lightControls")) $("lightControls").hidden = !show;
  if ($("loginFormWrap")) $("loginFormWrap").hidden = show;
  if (!show && pollTimer) { clearTimeout(pollTimer); pollTimer = null; }
}
function renderLight(id, on) {
  lightStates[id] = on === true;
  const label = $("luz" + id);
  const button = document.querySelector(`[data-light="${id}"]`);
  if (label) label.textContent = on === true ? "ENCENDIDA" : on === false ? "APAGADA" : "Sin datos";
  if (button) {
    button.classList.toggle("is-on", on === true);
    button.setAttribute("aria-pressed", on === true ? "true" : "false");
    const action = button.querySelector(".light-action");
    if (action) action.textContent = on === true ? "PULSAR PARA APAGAR" : "PULSAR PARA ENCENDER";
  }
}
function fmtUptime(value) { if (!Number.isFinite(Number(value))) return "--"; const s = Math.max(0, Math.floor(Number(value))); const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),r=s%60; return h ? `${h}h ${m}m` : m ? `${m}m ${r}s` : `${r}s`; }
async function fetchJson(url, options = {}, timeout = 12000) {
  const response = await fetch(url, {cache:"no-store", credentials:"same-origin", ...options, signal:AbortSignal.timeout(timeout)});
  const raw = await response.text();
  let data = {};
  try { data = raw ? JSON.parse(raw) : {}; } catch { data = {detail: raw}; }
  if (!response.ok) {
    const detail = data.mensaje || data.title || data.detail || data.error || data.detalle || `HTTP ${response.status}`;
    const esp = data.codigoHttpESP32 ? ` (respuesta ESP32: HTTP ${data.codigoHttpESP32})` : "";
    const err = new Error(`${detail}${esp}`); err.status = response.status; throw err;
  }
  return data;
}
function sessionExpired() {
  showLightControls(false);
  setConnection(false, "La sesión venció; inicia sesión nuevamente");
  setMessage("loginMessage", "La sesión del ESP32 venció. Inicia sesión nuevamente.", "error");
}
async function refreshStatus() {
  if (!loggedIn || statusInFlight || commandInFlight) return;
  statusInFlight = true;
  try {
    const data = await fetchJson("/api/status", {}, 12000);
    if (data.sesion !== true) { showLightControls(false); setConnection(false,"La sesión venció; inicia sesión nuevamente"); setMessage("loginMessage","La sesión del ESP32 venció. Inicia sesión nuevamente.","error"); return; }
    setConnection(true,"ESP32 conectado");
    setValue("temperature", data.dht_ok ? data.temperatura : null);
    setValue("humidity", data.dht_ok ? data.humedad : null);
    setValue("battery", data.bateria?.voltaje);
    setValue("solar", data.panel?.voltaje);
    setValue("batteryPercent", data.bateria?.porcentaje, "%");
    if ($("solarState")) $("solarState").textContent = data.panel?.cargando === true ? "Panel suministrando carga" : "Sin carga detectada";
    setValue("distance", data.tanque?.valido ? data.tanque?.distancia_cm : null);
    const tankValid = data.tanque?.valido === true;
    const level = Number(data.tanque?.porcentaje);
    if (tankValid && Number.isFinite(level)) {
      const safe = Math.max(0, Math.min(100, level));
      if ($("level")) $("level").textContent = `${safe.toFixed(0)}%`;
      if ($("water")) $("water").style.height = `${safe}%`;
      if ($("tankPercent")) $("tankPercent").textContent = `${safe.toFixed(0)}%`;
      if ($("tankBadge")) { $("tankBadge").textContent = "LECTURA NORMAL"; $("tankBadge").classList.add("good"); }
    } else {
      if ($("level")) $("level").textContent = "--%";
      if ($("water")) $("water").style.height = "0%";
      if ($("tankPercent")) $("tankPercent").textContent = "--%";
      if ($("tankBadge")) { $("tankBadge").textContent = "SIN LECTURA"; $("tankBadge").classList.remove("good"); }
    }
    if ($("pumpState")) { const pump=data.tanque?.bomba; $("pumpState").textContent = pump === true ? "ENCENDIDA" : pump === false ? "APAGADA" : "Sin datos"; $("pumpState").style.color = pump === true ? "#34d399" : ""; }
    if ($("pumpMode")) $("pumpMode").textContent = "Automatización gestionada por el ESP32";
    renderLight(1,data.luces?.luz1); renderLight(2,data.luces?.luz2); renderLight(3,data.luces?.luz3);
    setValue("current",data.carga?.corriente); setValue("power",data.carga?.watts); setValue("rssi",data.rssi);
    if ($("uptime")) $("uptime").textContent = fmtUptime(data.uptime);
    if ($("updated")) $("updated").textContent = "Última actualización: " + new Date().toLocaleTimeString();
    lastStatusAt = Date.now();
  } catch (error) {
    if (error.status === 401) { sessionExpired(); return; }
    setConnection(false,"Error de comunicación con el ESP32");
    if ($("updated")) $("updated").textContent = error.name === "TimeoutError" || error.name === "AbortError" ? "El ESP32 tardó en responder; se reintentará automáticamente." : error.message;
  } finally { statusInFlight = false; }
}
function schedulePolling(delay = POLL_MS) {
  if (pollTimer) clearTimeout(pollTimer);
  if (!loggedIn) return;
  pollTimer = setTimeout(async () => { await refreshStatus(); schedulePolling(); }, delay);
}
const loginForm = $("loginForm");
if (loginForm) loginForm.addEventListener("submit", async event => {
  event.preventDefault(); const submit = $("loginButton"); if (submit) submit.disabled = true;
  setMessage("loginMessage","Validando acceso con el ESP32…");
  try {
    const data = await fetchJson("/api/login", {method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({usuario:$("usuario").value,clave:$("clave").value})},14000);
    if (data.ok !== true) throw new Error(data.mensaje || "El ESP32 no aceptó las credenciales.");
    loginForm.reset(); showLightControls(true); setMessage("loginMessage","Sesión iniciada correctamente.","success");
    await refreshStatus(); schedulePolling(300);
  } catch (error) { showLightControls(false); setMessage("loginMessage",error.message || "No se pudo iniciar sesión.","error"); }
  finally { if (submit) submit.disabled = false; }
});

document.querySelectorAll("[data-light]").forEach(button => button.addEventListener("click", async () => {
  if (!loggedIn || commandInFlight) return;
  const id = Number(button.dataset.light); const state = !lightStates[id]; commandInFlight = true;
  document.querySelectorAll("[data-light]").forEach(b => b.disabled = true);
  setMessage("lightMessage",`Enviando orden para ${state ? "encender" : "apagar"} la luz ${id}…`);
  try {
    const data = await fetchJson("/api/light",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({luz:id,estado:state})},14000);
    if (data.ok !== true) throw new Error(data.mensaje || "El ESP32 no confirmó el comando.");
    // No mostramos el cambio como definitivo hasta leer el estado real de los GPIO.
    setMessage("lightMessage","Orden aceptada; verificando el estado real de la salida…","success");
  } catch (error) {
    if (error.status === 401) { sessionExpired(); return; }
    setMessage("lightMessage",`No se pudo controlar la luz ${id}: ${error.message}`,"error");
  } finally {
    commandInFlight = false;
    document.querySelectorAll("[data-light]").forEach(b => b.disabled = false);
    await refreshStatus(); schedulePolling(POLL_MS);
  }
}));
const logoutButton = $("logoutButton");
if (logoutButton) logoutButton.addEventListener("click", async () => {
  logoutButton.disabled = true;
  try { await fetchJson("/api/logout",{method:"POST"},14000); showLightControls(false); setConnection(false,"Sesión cerrada"); setMessage("loginMessage","Sesión cerrada."); if ($("updated")) $("updated").textContent="Sesión cerrada."; }
  catch(error) { setMessage("lightMessage",error.message,"error"); }
  finally { logoutButton.disabled=false; }
});
async function checkSession() {
  try {
    const data = await fetchJson("/api/status",{},12000);
    if (data.sesion === true) { showLightControls(true); await refreshStatus(); schedulePolling(250); }
    else { showLightControls(false); setConnection(false,"Inicia sesión para consultar el ESP32"); if ($("updated")) $("updated").textContent="Inicia sesión para habilitar la supervisión."; }
  } catch { showLightControls(false); setConnection(false,"Inicia sesión para consultar el ESP32"); if ($("updated")) $("updated").textContent="Inicia sesión; si el ESP32 no responde, revisa su conexión de red."; }
}
showLightControls(false); checkSession();

// ---- Mostrar / ocultar contraseña ----
const passInput = $("clave");
const passToggle = $("togglePassword");
function setPasswordVisible(visible) {
  if (!passInput || !passToggle) return;
  passInput.type = visible ? "text" : "password";
  passToggle.classList.toggle("is-visible", visible);
  passToggle.setAttribute("aria-pressed", String(visible));
  const label = visible ? "Ocultar contraseña" : "Mostrar contraseña";
  passToggle.setAttribute("aria-label", label);
  passToggle.title = label;
}
if (passInput && passToggle) {
  passToggle.addEventListener("mousedown", event => event.preventDefault());   // el campo conserva el foco
  passToggle.addEventListener("click", () => { setPasswordVisible(passInput.type === "password"); passInput.focus(); });
}
// Tras iniciar sesión el formulario se limpia (reset): la contraseña vuelve a quedar oculta.
if (loginForm) loginForm.addEventListener("reset", () => setTimeout(() => setPasswordVisible(false), 0));
