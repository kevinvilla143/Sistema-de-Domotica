// SISTEMA DOMOTICO ESP32 - v3.1 (interfaz y sesion corregidas; conexiones sin cambios)
//  - Reles ACTIVOS EN HIGH
//  - ACS712 conectado SIN divisor de tension (salida directa al ESP32)
//  - Requiere ArduinoJson v7
// ============================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Wire.h>

// -- Pines ------------------------------------------------------------------
const int LUZ1_PIN       = 14;   // OJO: GPIO14 emite un pulso breve al arrancar
const int LUZ2_PIN       = 26;
const int LUZ3_PIN       = 27;
const int PANEL_SOLAR_PIN = 34;  // Voltaje panel (divisor)
const int BATERIA_PIN    = 33;   // Voltaje bateria (divisor)
const int CORRIENTE_PIN  = 35;   // ACS712 (salida directa, sin divisor)
const int RESET_BTN_PIN  = 4;    // Boton fisico de reset WiFi (a GND)
const int DHT11_PIN      = 16;

// -- Reles ------------------------------------------------------------------
const bool RELE_ACTIVO_ALTO = true;   // true = el rele se activa con HIGH
inline int nivelRele(bool encendido) {
  return (encendido == RELE_ACTIVO_ALTO) ? HIGH : LOW;
}

// -- Guardar estado de las luces tras reinicio ------------------------------
const bool GUARDAR_ESTADO_LUCES = false;  // true = restaura luces al arrancar

// -- DHT11 / LCD ------------------------------------------------------------
#define DHTTYPE DHT11
DHT dht(DHT11_PIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);       // cambia a 0x3F si tu modulo lo usa

// -- WiFi -------------------------------------------------------------------
const int MAX_INTENTOS_WIFI = 3;
const unsigned long TIMEOUT_INTENTO_MS = 15000;
const unsigned long REINTENTO_WIFI_MS  = 15000;
const char* AP_SSID = "DomoticaESP32";
const char* AP_PASS = "domotica123";      // minimo 8 caracteres. CAMBIALA

// -- Credenciales admin (CAMBIALAS) -----------------------------------------
const char* ADMIN_USER = "admin";
const char* ADMIN_PASS = "admin1234";

// -- Divisores de tension (panel y bateria) ---------------------------------
const float FACTOR_PANEL   = 5.1;         // (100k + 20k) / 20k
const float FACTOR_BATERIA = 5.1;     // (52k + 23k) / 23k  factor 3.26087

// -- Bateria ----------------------------------------------------------------
const float BATERIA_VACIA = 7.2;
const float BATERIA_LLENA = 9.2;

// -- ACS712 (5A) sin divisor --------------------------------------------------
// Sensor en SERIE con la carga: corriente positiva = consumo.
// Si sale negativa, pon INVERTIR_CORRIENTE = true.
const float ACS_SENSIBILIDAD    = 0.185;  // V/A (modelo 5A)
const float ACS_FACTOR_DIVISOR  = 1.0;    // 1.0 = sin divisor. Si agregas uno 10k/10k: 2.0
const float ACS_MV_MAX          = 3100.0; // sobre esto el ADC (11 dB) satura
const bool  INVERTIR_CORRIENTE  = false;
const int   NUM_MUESTRAS_CALIBRACION = 200;
float corrienteOffset = 2.5;              // V, se calibra al arrancar

// -- Objetos globales ---------------------------------------------------------
WebServer server(80);
Preferences preferences;

String ssidGuardado = "";
String passGuardado = "";
bool portalActivo = false;                // true = AP de configuracion activo

bool estadoLuz1 = false, estadoLuz2 = false, estadoLuz3 = false;

// -- Sesion ---------------------------------------------------------------------
String sessionToken = "";
unsigned long sessionUltimoUso = 0;
const unsigned long SESSION_TIMEOUT_MS = 30UL * 60UL * 1000UL;
int loginFallos = 0;
unsigned long bloqueoHasta = 0;

// -- Filtro de mediana con buffer circular (uno por sensor) -------------------
const int NUM_MUESTRAS = 9;
struct FiltroMediana {
  float datos[NUM_MUESTRAS];
  int n = 0;
  int idx = 0;
  void agregar(float v) {
    datos[idx] = v;
    idx = (idx + 1) % NUM_MUESTRAS;
    if (n < NUM_MUESTRAS) n++;
  }
  float mediana() {
    if (n == 0) return 0;
    float t[NUM_MUESTRAS];
    for (int i = 0; i < n; i++) t[i] = datos[i];
    for (int i = 1; i < n; i++) {          // insercion
      float k = t[i];
      int j = i - 1;
      while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
      t[j + 1] = k;
    }
    return t[n / 2];
  }
};
FiltroMediana fPanel, fBat, fCorr;

// -- Valores medidos (cache: la API solo los lee) -----------------------------
float vPanel = 0, vBat = 0, pctBat = 0, corriente = 0, potenciaCarga = 0;
float temperatura = 0, humedad = 0;
bool dhtOk = false;
int dhtFallos = 0;
bool sensorSaturado = false;
bool offsetOk = true;

// -- Temporizadores ------------------------------------------------------------
unsigned long lastSensor = 0, lastDHT = 0, lastLCD = 0, lastDiag = 0;
const unsigned long SENSOR_INTERVAL = 250;
const unsigned long DHT_INTERVAL    = 2000;
const unsigned long LCD_INTERVAL    = 3000;

// -- LCD (cache para no parpadear) --------------------------------------------
String lcdL1 = "", lcdL2 = "";

// -- Prototipos -----------------------------------------------------------------
void configurarRutas();
void iniciarPortal(bool conSTA);
bool conectarWiFi();
void mantenerWiFi();
void verificarBotonReset();
void borrarCredenciales();
void handleRoot();
void handleLogin();
void handleLogout();
void handleConfig();
void handleGuardar();
void handleResetWiFi();
void handleApiStatus();
void handleApiLuz();
void handleCalibrar();
void handleNotFound();

// ============================================================================
// LCD
// ============================================================================
String ajustar16(String s) {
  if (s.length() > 16) s = s.substring(0, 16);
  while (s.length() < 16) s += ' ';
  return s;
}

void actualizarLCD(String l1, String l2) {
  l1 = ajustar16(l1);
  l2 = ajustar16(l2);
  if (l1 == lcdL1 && l2 == lcdL2) return;      // sin cambios, sin parpadeo
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
  lcdL1 = l1; lcdL2 = l2;
  Serial.println("LCD: " + l1 + " | " + l2);
}

// ============================================================================
// LUCES
// ============================================================================
void guardarLuces() {
  if (!GUARDAR_ESTADO_LUCES) return;
  preferences.begin("luces", false);
  preferences.putUChar("mask", (estadoLuz1 ? 1 : 0) | (estadoLuz2 ? 2 : 0) | (estadoLuz3 ? 4 : 0));
  preferences.end();
}

void controlarLuz(int n, bool e) {
  switch (n) {
    case 1: digitalWrite(LUZ1_PIN, nivelRele(e)); estadoLuz1 = e; break;
    case 2: digitalWrite(LUZ2_PIN, nivelRele(e)); estadoLuz2 = e; break;
    case 3: digitalWrite(LUZ3_PIN, nivelRele(e)); estadoLuz3 = e; break;
    default: return;
  }
  Serial.printf("Luz %d: %s\n", n, e ? "ON" : "OFF");
  guardarLuces();
}

// ============================================================================
// SENSORES
// ============================================================================
float leerMilivoltios(int pin, int muestras = 16) {
  uint32_t suma = 0;
  for (int i = 0; i < muestras; i++) suma += analogReadMilliVolts(pin);
  return suma / (float)muestras;
}

void calibrarSensorCorriente() {
  Serial.println("Calibrando sensor de corriente (sin carga)...");
  actualizarLCD("Calibrando", "Sensor corriente");
  float mv = leerMilivoltios(CORRIENTE_PIN, NUM_MUESTRAS_CALIBRACION);
  corrienteOffset = (mv / 1000.0) * ACS_FACTOR_DIVISOR;
  offsetOk = (corrienteOffset > 2.3 && corrienteOffset < 2.7);
  Serial.printf("Offset ACS712: %.3f V %s\n", corrienteOffset,
                offsetOk ? "" : "(FUERA DE RANGO: revisa cableado/alimentacion 5V)");
  actualizarLCD("Offset ACS712", String(corrienteOffset, 3) + "V");
  delay(1000);
  fCorr.n = 0; fCorr.idx = 0;
}

void leerSensores() {
  // Panel y bateria
  float vp = (leerMilivoltios(PANEL_SOLAR_PIN) / 1000.0) * FACTOR_PANEL;
  float vb = (leerMilivoltios(BATERIA_PIN) / 1000.0) * FACTOR_BATERIA;
  fPanel.agregar(vp);
  fBat.agregar(vb);

  // Corriente
  float mvAcs = leerMilivoltios(CORRIENTE_PIN);
  sensorSaturado = (mvAcs >= ACS_MV_MAX);
  float vAcs = (mvAcs / 1000.0) * ACS_FACTOR_DIVISOR;
  float i = (vAcs - corrienteOffset) / ACS_SENSIBILIDAD;
  if (INVERTIR_CORRIENTE) i = -i;
  fCorr.agregar(i);

  vPanel = fPanel.mediana();
  vBat = fBat.mediana();
  corriente = fCorr.mediana();

  if (vPanel < 0.9) vPanel = 0; //original era 0.2
  if (vBat < 0.9) vBat = 0;
  if (fabsf(corriente) < 0.03) corriente = 0;   // zona muerta de ruido

  pctBat = ((vBat - BATERIA_VACIA) / (BATERIA_LLENA - BATERIA_VACIA)) * 100.0;
  if (pctBat < 0) pctBat = 0;
  if (pctBat > 100) pctBat = 100;

  float consumo = (corriente > 0) ? corriente : 0;
  potenciaCarga = vBat * consumo;
  if (potenciaCarga < 0.01) potenciaCarga = 0;
}

void leerDHT11() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    temperatura = t;
    humedad = h;
    dhtOk = true;
    dhtFallos = 0;
  } else {
    if (++dhtFallos >= 3) dhtOk = false;
    Serial.println("Error al leer DHT11");
  }
}

// ============================================================================
// SESION / AUTENTICACION
// ============================================================================
String generarToken() {
  String t = "";
  for (int i = 0; i < 4; i++) {
    char b[9];
    snprintf(b, sizeof(b), "%08lx", (unsigned long)esp_random());
    t += b;
  }
  return t;
}

bool autenticado() {
  if (sessionToken == "" || !server.hasHeader("Cookie")) return false;
  if (millis() - sessionUltimoUso > SESSION_TIMEOUT_MS) {
    sessionToken = "";
    return false;
  }
  String c = server.header("Cookie");
  if (c.indexOf("sid=" + sessionToken) >= 0) {
    sessionUltimoUso = millis();
    return true;
  }
  return false;
}

// ============================================================================
// PAGINA PRINCIPAL (PROGMEM)
// ============================================================================
const char PAGINA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Domótica ESP32</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:linear-gradient(135deg,#0a0f0e,#0c1412);color:#e8f0ec;padding:20px;min-height:100vh;-webkit-user-select:none;user-select:none;-webkit-touch-callout:none;-webkit-tap-highlight-color:transparent;caret-color:transparent}
.container{max-width:600px;margin:0 auto}
.header{text-align:center;margin-bottom:30px}
.header h1{font-size:28px;color:#39d98a;margin-bottom:5px}
.header p{color:#7a9186;font-size:14px}
.card{background:rgba(20,25,24,.95);border-radius:20px;padding:20px;margin-bottom:20px;border:1px solid rgba(57,217,138,.2);box-shadow:0 8px 32px rgba(0,0,0,.3)}
.card h3{color:#39d98a;font-size:16px;margin-bottom:15px;display:flex;align-items:center;gap:8px}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:15px}
.metric{background:rgba(10,15,14,.8);border-radius:12px;padding:15px;text-align:center}
.label{font-size:12px;color:#7a9186;margin-bottom:8px}
.value{font-size:26px;font-weight:bold;color:#39d98a}
.unit{font-size:12px;color:#7a9186;margin-left:2px}
.emoji{font-size:30px;margin-bottom:5px}
.buttons{display:flex;gap:15px}
.light-btn{flex:1;background:rgba(30,40,38,.8);border:1px solid rgba(57,217,138,.3);border-radius:50px;padding:15px;text-align:center;cursor:pointer;transition:all .3s}
.light-btn.active{background:#39d98a;color:#0a0f0e;border-color:#39d98a;box-shadow:0 0 20px rgba(57,217,138,.5)}
.light-btn span{display:block;font-size:24px;margin-bottom:5px}
.light-btn small{font-size:11px;opacity:.8}
.links{display:flex;gap:10px;justify-content:center;flex-wrap:wrap;margin-top:10px}
.links a,.links button{font-family:inherit;background:rgba(30,40,38,.8);border:1px solid rgba(57,217,138,.3);color:#e8f0ec;border-radius:50px;padding:10px 16px;font-size:13px;text-decoration:none;cursor:pointer}
.links .danger{background:rgba(239,68,68,.2);border-color:rgba(239,68,68,.5);color:#fca5a5}
.dot{width:10px;height:10px;border-radius:50%;background:#39d98a;display:inline-block;animation:pulse 2s infinite}
.dot.off{background:#ef4444;animation:none}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
.warn{color:#fbbf24;font-size:11px;margin-top:4px;min-height:14px}
.footer{text-align:center;font-size:11px;color:#4a5a54;margin-top:20px}
</style>
</head>
<body>
<div class="container">
  <div class="header"><h1>Domótica ESP32</h1><p>Sistema de monitoreo y control</p></div>

  <div class="card">
    <h3><span class="dot" id="dot"></span> Clima Interior</h3>
    <div class="grid">
      <div class="metric"><div class="emoji">🌡️</div><div><span class="value" id="temp">--</span><span class="unit">°C</span></div><div class="label">Temperatura</div></div>
      <div class="metric"><div class="emoji">💧</div><div><span class="value" id="hum">--</span><span class="unit">%</span></div><div class="label">Humedad</div></div>
    </div>
    <div class="warn" id="dhtWarn"></div>
  </div>

  <div class="card">
    <h3>Estado de energía</h3>
    <div class="grid">
      <div class="metric"><div class="label">Panel Solar</div><div><span class="value" id="pV">--</span><span class="unit">V</span></div><div class="label" id="pEstado">--</div></div>
      <div class="metric"><div class="label">Batería</div><div><span class="value" id="bV">--</span><span class="unit">V</span></div><div class="label" id="bPct">--%</div></div>
      <div class="metric"><div class="label">Consumo</div><div><span class="value" id="cW">--</span><span class="unit">W</span></div><div class="label">Potencia carga</div></div>
      <div class="metric"><div class="label">Corriente</div><div><span class="value" id="cA">--</span><span class="unit">A</span></div><div class="label">Sensor ACS712</div></div>
    </div>
    <div class="warn" id="acsWarn"></div>
  </div>

  <div class="card">
    <h3>Control de Luces</h3>
    <div class="buttons">
      <div class="light-btn" id="luz1Btn" onclick="toggleLight(1)"><span>💡</span><small>Luz 1</small></div>
      <div class="light-btn" id="luz2Btn" onclick="toggleLight(2)"><span>💡</span><small>Luz 2</small></div>
      <div class="light-btn" id="luz3Btn" onclick="toggleLight(3)"><span>💡</span><small>Luz 3</small></div>
    </div>
  </div>

  <div class="links">
    <a href="/login" id="authLink">Iniciar sesión</a>
    <a href="/config">Configurar WiFi</a>
    <button onclick="calibrar()">Recalibrar corriente</button>
    <button class="danger" onclick="resetWifi()">Resetear WiFi</button>
  </div>
  <div class="footer" id="footer">v3.0</div>
</div>

<script>
let luces = {luz1:false, luz2:false, luz3:false};
let enCurso = false;
const $ = id => document.getElementById(id);

async function actualizar() {
  if (enCurso) return;
  enCurso = true;
  try {
    const r = await fetch('/api/status', {cache:'no-store'});
    if (r.status === 401) { location.replace('/login?next=/'); return; }
    const d = await r.json();
    $('dot').classList.remove('off');
    $('temp').textContent = d.dht_ok ? d.temperatura.toFixed(1) : '--';
    $('hum').textContent  = d.dht_ok ? d.humedad.toFixed(1) : '--';
    $('dhtWarn').textContent = d.dht_ok ? '' : 'El sensor DHT11 no responde';
    $('pV').textContent = d.panel.voltaje.toFixed(1);
    $('pEstado').textContent = d.panel.cargando ? 'Cargando' : 'Sin carga';
    $('bV').textContent = d.bateria.voltaje.toFixed(1);
    $('bPct').textContent = d.bateria.porcentaje.toFixed(0) + '%';
    $('cW').textContent = d.carga.watts.toFixed(1);
    $('cA').textContent = d.carga.corriente.toFixed(2);
    let w = [];
    if (d.alertas.saturado) w.push('Sensor saturado: la corriente supera lo medible');
    if (!d.alertas.offset_ok) w.push('Offset del ACS712 fuera de rango');
    $('acsWarn').textContent = w.join(' | ');
    luces = d.luces;
    for (let i = 1; i <= 3; i++) $('luz'+i+'Btn').classList.toggle('active', !!luces['luz'+i]);
    $('authLink').textContent = d.sesion ? 'Cerrar sesión' : 'Iniciar sesión';
    $('authLink').href = d.sesion ? '/logout' : '/login';
    $('footer').textContent = 'v3.0 | WiFi ' + d.rssi + ' dBm | activo ' + Math.floor(d.uptime/60) + ' min';
  } catch (e) {
    $('dot').classList.add('off');
  }
  enCurso = false;
}

async function toggleLight(n) {
  const nuevo = !luces['luz'+n];
  const r = await fetch('/api/light', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({luz:n, estado:nuevo})});
  if (r.status === 401) { location.href = '/login?next=/'; return; }
  if (r.ok) { luces['luz'+n] = nuevo; $('luz'+n+'Btn').classList.toggle('active', nuevo); }
}

async function calibrar() {
  if (!confirm('Apaga todas las cargas antes de recalibrar. ¿Continuar?')) return;
  const r = await fetch('/api/calibrar', {method:'POST'});
  if (r.status === 401) { location.href = '/login?next=/'; return; }
  actualizar();
}

async function resetWifi() {
  if (!confirm('¿Restablecer la configuración WiFi? El ESP32 se reiniciará.')) return;
  const r = await fetch('/resetwifi', {method:'POST'});
  if (r.status === 401) { location.href = '/login?next=/'; return; }
  setTimeout(() => location.reload(), 4000);
}

window.addEventListener('pageshow', e => { if (e.persisted) location.reload(); });
setInterval(actualizar, 2000);
actualizar();
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== SISTEMA DOMOTICO ESP32 v3.0 ===");

  // Restaurar estado de luces (o todo apagado)
  if (GUARDAR_ESTADO_LUCES) {
    preferences.begin("luces", false);
    uint8_t m = preferences.getUChar("mask", 0);
    preferences.end();
    estadoLuz1 = m & 1; estadoLuz2 = m & 2; estadoLuz3 = m & 4;
  }
  // Precargar el nivel ANTES de configurar como salida para evitar glitches
  digitalWrite(LUZ1_PIN, nivelRele(estadoLuz1));
  digitalWrite(LUZ2_PIN, nivelRele(estadoLuz2));
  digitalWrite(LUZ3_PIN, nivelRele(estadoLuz3));
  pinMode(LUZ1_PIN, OUTPUT);
  pinMode(LUZ2_PIN, OUTPUT);
  pinMode(LUZ3_PIN, OUTPUT);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  actualizarLCD("Iniciando...", "ESP32 Domotica");
  delay(1500);

  dht.begin();

  analogSetPinAttenuation(PANEL_SOLAR_PIN, ADC_11db);
  analogSetPinAttenuation(BATERIA_PIN, ADC_11db);
  analogSetPinAttenuation(CORRIENTE_PIN, ADC_11db);
  analogReadResolution(12);

  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  calibrarSensorCorriente();                 // con cargas apagadas
  for (int i = 0; i < NUM_MUESTRAS; i++) leerSensores();   // llena los filtros
  leerDHT11();

  preferences.begin("wifi", false);
  ssidGuardado = preferences.getString("ssid", "");
  passGuardado = preferences.getString("pass", "");
  preferences.end();

  configurarRutas();

  if (ssidGuardado == "") {
    iniciarPortal(false);
  } else if (!conectarWiFi()) {
    Serial.println("No se pudo conectar. Portal de configuracion activo, se sigue reintentando.");
    iniciarPortal(true);                     // NO se borran las credenciales
  }

  server.begin();                            // WiFi ya inicializado -> seguro
  Serial.println("Servidor web listo en puerto 80");
}

// ============================================================================
// LOOP
// ============================================================================
void loop() {
  server.handleClient();
  verificarBotonReset();
  mantenerWiFi();

  unsigned long ahora = millis();

  if (ahora - lastSensor >= SENSOR_INTERVAL) {
    lastSensor = ahora;
    leerSensores();
  }

  if (ahora - lastDHT >= DHT_INTERVAL) {
    lastDHT = ahora;
    leerDHT11();
  }

  if (ahora - lastLCD >= LCD_INTERVAL) {
    lastLCD = ahora;
    static bool pagina = false;
    pagina = !pagina;
    if (!pagina) {
    
    String l1 = dhtOk ? ("Temp:" + String(temperatura, 0) + "C Hum:" + String(humedad, 0) + "%") : "DHT11 error";
    String l2 = "I:" + String(corriente, 2) + "A Bat:" + String(pctBat, 0) + "%";
    actualizarLCD(l1, l2);
    
    } else {
      String l2 = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString()
                  : (portalActivo ? "AP:192.168.4.1" : "Sin WiFi");
      actualizarLCD("BAT: " + String(vBat, 1) + "V", l2);
    }
  }

  if (ahora - lastDiag >= 30000) {
    lastDiag = ahora;
    Serial.printf("Panel %.2fV | Bat %.2fV (%.0f%%) | I %.3fA | P %.2fW | Sat:%d OffsetOK:%d | Heap %u\n",
                  vPanel, vBat, pctBat, corriente, potenciaCarga,
                  sensorSaturado, offsetOk, ESP.getFreeHeap());
  }
}

// ============================================================================
// WIFI
// ============================================================================
bool conectarWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  for (int intento = 1; intento <= MAX_INTENTOS_WIFI; intento++) {
    Serial.printf("Conectando a %s (intento %d/%d)\n", ssidGuardado.c_str(), intento, MAX_INTENTOS_WIFI);
    actualizarLCD("Conectando WiFi", ssidGuardado);
    WiFi.begin(ssidGuardado.c_str(), passGuardado.c_str());

    unsigned long inicio = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - inicio < TIMEOUT_INTENTO_MS) {
      delay(100);
      verificarBotonReset();
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
      actualizarLCD("Conectado!", WiFi.localIP().toString());
      delay(1500);
      return true;
    }

    WiFi.disconnect();
    actualizarLCD("Error WiFi", "Intento " + String(intento) + "/" + String(MAX_INTENTOS_WIFI));
    delay(1000);
  }
  return false;
}

void iniciarPortal(bool conSTA) {
  WiFi.mode(conSTA ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  portalActivo = true;
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("AP '%s' activo - http://%s\n", AP_SSID, apIP.toString().c_str());
  actualizarLCD("Conectate a:", AP_SSID);
  delay(2000);
  actualizarLCD("Web:", apIP.toString());
  if (conSTA && ssidGuardado != "") {
    WiFi.begin(ssidGuardado.c_str(), passGuardado.c_str());
  }
}

void mantenerWiFi() {
  static unsigned long ultimo = 0;
  if (millis() - ultimo < REINTENTO_WIFI_MS) return;
  ultimo = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (portalActivo) {                      // ya hay red: cerrar el AP
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      portalActivo = false;
      Serial.printf("WiFi restablecido. IP: %s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }

  if (ssidGuardado == "") return;            // solo modo AP, nada que reintentar
  Serial.println("WiFi caido, reintentando...");
  WiFi.disconnect();
  WiFi.begin(ssidGuardado.c_str(), passGuardado.c_str());
}

void borrarCredenciales() {
  preferences.begin("wifi", false);
  preferences.putString("ssid", "");
  preferences.putString("pass", "");
  preferences.end();
}

// ============================================================================
// BOTON FISICO DE RESET (mantener 3 s)
// ============================================================================
void verificarBotonReset() {
  static unsigned long desde = 0;
  static bool enProgreso = false;
  const unsigned long HOLD_MS = 3000;

  bool presionado = (digitalRead(RESET_BTN_PIN) == LOW);

  if (presionado && !enProgreso) {
    desde = millis();
    enProgreso = true;
    actualizarLCD("Mantenga 3 seg", "para reset WiFi");
  } else if (presionado && enProgreso) {
    if (millis() - desde >= HOLD_MS) {
      Serial.println("RESETEANDO WiFi...");
      actualizarLCD("Reset WiFi", "Borrando datos...");
      borrarCredenciales();
      delay(500);
      ESP.restart();
    }
  } else if (!presionado && enProgreso) {
    enProgreso = false;
    lcdL1 = ""; lcdL2 = "";                  // fuerza redibujar en el siguiente ciclo
  }
}

// ============================================================================
// SERVIDOR WEB
// ============================================================================
void configurarRutas() {
  const char* cabeceras[] = {"Cookie"};
  server.collectHeaders(cabeceras, 1);       // necesario para leer la cookie

  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/guardar", HTTP_POST, handleGuardar);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/light", HTTP_POST, handleApiLuz);
  server.on("/api/calibrar", HTTP_POST, handleCalibrar);
  server.on("/resetwifi", HTTP_POST, handleResetWiFi);
  server.onNotFound(handleNotFound);
  // OJO: server.begin() se llama DESPUES de iniciar el WiFi (ver setup)
}

void handleRoot() {
  if (portalActivo) {
    server.sendHeader("Location", "/login?next=/config");
    server.send(302, "text/plain", "");
    return;
  }
  if (!autenticado()) {                      // sin sesion: pedir credenciales
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Location", "/login?next=/");
    server.send(302, "text/plain", "");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", PAGINA_HTML);
}

void handleNotFound() {
  server.send(404, "text/plain", "No encontrado");
}

void handleApiStatus() {
  if (!autenticado()) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(401, "application/json", "{\"error\":\"No autorizado\"}");
    return;
  }
  JsonDocument doc;
  doc["temperatura"] = temperatura;
  doc["humedad"] = humedad;
  doc["dht_ok"] = dhtOk;

  doc["panel"]["voltaje"] = vPanel;
  doc["panel"]["cargando"] = (vPanel > vBat + 0.5);

  doc["bateria"]["voltaje"] = vBat;
  doc["bateria"]["porcentaje"] = pctBat;

  doc["carga"]["watts"] = potenciaCarga;
  doc["carga"]["corriente"] = corriente;

  doc["luces"]["luz1"] = estadoLuz1;
  doc["luces"]["luz2"] = estadoLuz2;
  doc["luces"]["luz3"] = estadoLuz3;

  doc["alertas"]["saturado"] = sensorSaturado;
  doc["alertas"]["offset_ok"] = offsetOk;

  doc["sesion"] = autenticado();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;

  String json;
  serializeJson(doc, json);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleApiLuz() {
  if (!autenticado()) {
    server.send(401, "application/json", "{\"error\":\"No autorizado\"}");
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Sin cuerpo\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"JSON invalido\"}");
    return;
  }
  int luz = doc["luz"] | 0;
  bool estado = doc["estado"] | false;
  if (luz < 1 || luz > 3) {
    server.send(400, "application/json", "{\"error\":\"Luz invalida (1-3)\"}");
    return;
  }
  controlarLuz(luz, estado);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleCalibrar() {
  if (!autenticado()) {
    server.send(401, "application/json", "{\"error\":\"No autorizado\"}");
    return;
  }
  calibrarSensorCorriente();                 // las cargas deben estar apagadas
  for (int i = 0; i < NUM_MUESTRAS; i++) leerSensores();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleResetWiFi() {
  if (!autenticado()) {
    server.send(401, "text/plain", "No autorizado");
    return;
  }
  borrarCredenciales();
  server.send(200, "text/plain", "OK");
  delay(1000);
  ESP.restart();
}

// ============================================================================
// LOGIN / CONFIGURACION
// ============================================================================
const char ESTILO_PAGINAS[] PROGMEM =
  "<style>body{-webkit-user-select:none;user-select:none;caret-color:transparent;font-family:sans-serif;background:#0a0a0a;color:#fff;display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
  ".card{background:#1a1a1a;padding:30px;border-radius:15px;width:320px;box-shadow:0 4px 15px rgba(0,0,0,.3)}"
  "h1{color:#39d98a;font-size:24px;margin-bottom:10px}"
  "input{-webkit-user-select:text;user-select:text;caret-color:auto;width:100%;padding:10px;margin:10px 0;background:#2a2a2a;border:1px solid #3a3a3a;color:#fff;border-radius:5px;box-sizing:border-box}"
  "button{font-family:inherit;width:100%;padding:10px;background:#39d98a;border:none;border-radius:5px;color:#000;font-weight:bold;cursor:pointer}"
  ".error{color:#ff6b6b;margin-bottom:10px}.info{font-size:12px;color:#aaa;margin-top:15px;text-align:center}a{color:#39d98a}</style>";

void handleLogin() {
  String next = server.hasArg("next") ? server.arg("next") : (portalActivo ? "/config" : "/");
  if (next != "/config") next = "/";        // solo destinos permitidos
  String error = "";

  if (server.method() == HTTP_POST) {
    if (millis() < bloqueoHasta) {
      error = "Demasiados intentos. Espera 1 minuto.";
    } else if (server.arg("usuario") == ADMIN_USER && server.arg("clave") == ADMIN_PASS) {
      loginFallos = 0;
      sessionToken = generarToken();
      sessionUltimoUso = millis();
      server.sendHeader("Set-Cookie", "sid=" + sessionToken + "; Path=/; HttpOnly; SameSite=Strict");
      server.sendHeader("Location", next);
      server.send(302, "text/plain", "");
      return;
    } else {
      error = "Usuario o contraseña incorrectos";
      if (++loginFallos >= 5) {
        bloqueoHasta = millis() + 60000UL;
        loginFallos = 0;
      }
      delay(500);
    }
  }

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width'><title>Ingresar · Domótica</title>";
  html += FPSTR(ESTILO_PAGINAS);
  html += "</head><body><div class='card'><h1>Ingresar</h1>";
  if (error != "") html += "<div class='error'>" + error + "</div>";
  html += "<form method='POST' action='/login'>"
          "<input type='hidden' name='next' value='" + next + "'>"
          "<input type='text' name='usuario' placeholder='Usuario' autocomplete='off' required>"
          "<input type='password' name='clave' placeholder='Contraseña' autocomplete='new-password' required>"
          "<button type='submit'>Ingresar</button></form></div></body></html>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleLogout() {
  sessionToken = "";
  server.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "");
}

void handleConfig() {
  if (!autenticado()) {
    server.sendHeader("Location", "/login?next=/config");
    server.send(302, "text/plain", "");
    return;
  }
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width'><title>Configurar WiFi</title>";
  html += FPSTR(ESTILO_PAGINAS);
  html += "</head><body><div class='card'><h1>Configurar WiFi</h1>"
          "<form method='POST' action='/guardar'>"
          "<input type='text' name='ssid' placeholder='Nombre WiFi (SSID)' required>"
          "<input type='password' name='pass' placeholder='Contraseña (vacía si es red abierta)'>"
          "<button type='submit'>Guardar y Conectar</button></form>"
          "<div class='info'>El dispositivo se reiniciará automáticamente<br><br><a href='/'>Volver</a> | <a href='/logout'>Cerrar sesión</a></div>"
          "</div></body></html>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleGuardar() {
  if (!autenticado()) {
    server.send(403, "text/plain", "No autorizado");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  if (ssid == "") {
    server.send(400, "text/plain", "SSID requerido");
    return;
  }
  if (pass.length() > 0 && pass.length() < 8) {
    server.send(400, "text/plain; charset=utf-8", "La contraseña WPA debe tener al menos 8 caracteres");
    return;
  }

  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();

  actualizarLCD("Guardando WiFi", "Reiniciando...");
  server.send(200, "text/html; charset=utf-8", "<html><head><meta charset='UTF-8'></head><body><h2>Datos guardados. Reiniciando...</h2></body></html>");
  delay(1500);
  ESP.restart();
}
