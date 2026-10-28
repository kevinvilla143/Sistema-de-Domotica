// SISTEMA DOMOTICO ESP32 - v3.1
//  - Reles ACTIVOS EN HIGH
//  - ACS712 conectado SIN divisor de tension (salida directa al ESP32)
//  - Requiere ArduinoJson v7
//
// CAMBIOS v3.1 (sobre v3.0):
//  - WiFi.setSleep(false): evita que el ESP32 "duerma" el WiFi y deje de responder
//  - mDNS: acceso por http://domotica.local ademas de la IP
//  - Ruta /ping para diagnostico + logs de cada peticion en el monitor serie
//  - Log de IP, gateway, mascara y RSSI al conectar
//  - Evento de desconexion WiFi con el motivo (para depurar)
//  - Watchdog de conectividad: si el WiFi parece conectado pero no hay gateway, reconecta
// ============================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <Wire.h>

// -- Pines ------------------------------------------------------------------
const int LUZ1_PIN        = 14;   // OJO: GPIO14 emite un pulso breve al arrancar
const int LUZ2_PIN        = 26;
const int LUZ3_PIN        = 27;
const int PANEL_SOLAR_PIN = 34;   // Voltaje panel (divisor)
const int BATERIA_PIN     = 33;   // Voltaje bateria (divisor)
const int CORRIENTE_PIN   = 35;   // ACS712 (salida directa, sin divisor)
const int RESET_BTN_PIN   = 4;    // Boton fisico de reset WiFi (a GND)
const int DHT11_PIN       = 16;
const int BOMBA_PIN       = 25;   // Rele de la mini bomba (activo HIGH)
const int ULTRASONICO_TRIG_PIN = 5;
const int ULTRASONICO_ECHO_PIN = 18; // ECHO requiere divisor de tension a 3.3 V

// Ajustes del tanque: distancia desde el sensor superior hasta el fondo = 20 cm.
// Distancia grande = nivel bajo; distancia pequena = tanque lleno.
// Se considera lleno a 3 cm del sensor para dejar margen y evitar salpicaduras.
// La bomba arranca cuando la distancia al agua llega a 16 cm (nivel bajo).
const float TANQUE_FONDO_CM        = 20.0;  // sensor al fondo del tanque, confirmado por el usuario
const float TANQUE_LLENO_CM        = 3.0;   // distancia sensor-agua al nivel maximo
const float BOMBA_ENCIENDE_CM      = 16.0;  // arranca cuando el agua esta a 16 cm o mas
const float BOMBA_APAGA_CM         = 3.0;   // se detiene cuando el agua esta a 3 cm o menos
const unsigned long ULTRASONICO_INTERVAL_MS = 1000;

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
const char* MDNS_NOMBRE = "domotica";     // acceso: http://domotica.local

// -- Credenciales admin (CAMBIALAS) -----------------------------------------
const char* ADMIN_USER = "admin";
const char* ADMIN_PASS = "admin1234";

// -- Divisores de tension (panel y bateria) ---------------------------------
const float FACTOR_PANEL   = 5.1;         // (100k + 20k) / 20k
const float FACTOR_BATERIA = 5.1;

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
String ipGuardada = "";
String gatewayGuardado = "";
String mascaraGuardada = "";
String dnsGuardado = "";
bool portalActivo = false;                // true = AP de configuracion activo
bool mdnsActivo = false;

bool estadoLuz1 = false, estadoLuz2 = false, estadoLuz3 = false;
bool estadoBomba = false;
bool lecturaTanqueValida = false;
float distanciaAguaCm = 0.0;
float nivelTanquePct = 0.0;
unsigned long ultimoUltrasonico = 0;
int fallosUltrasonico = 0;

// -- Sesiones independientes por dispositivo -----------------------------------
// Cada navegador conserva su propia cookie sid. Iniciar/cerrar sesion en uno
// de ellos no invalida las sesiones de los demas dispositivos.
const uint8_t MAX_SESIONES = 8;
const unsigned long SESSION_TIMEOUT_MS = 30UL * 60UL * 1000UL;
struct SesionWeb {
  String token;
  unsigned long ultimoUso;
  bool activa;
};
SesionWeb sesionesWeb[MAX_SESIONES];
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
void iniciarMDNS();
void configurarWiFiComun();
void aplicarIPFija();
void mostrarInfoRed();
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
void leerNivelTanque();
void controlarBomba(bool encendida);
void handleCalibrar();
void handlePing();
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

  if (vPanel < 0.9) vPanel = 0;
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
// CONTROL AUTOMATICO DEL TANQUE Y LA BOMBA
// ============================================================================
void controlarBomba(bool encendida) {
  estadoBomba = encendida;
  digitalWrite(BOMBA_PIN, encendida ? nivelRele(true) : nivelRele(false));
  Serial.printf("Bomba: %s\n", encendida ? "ENCENDIDA" : "APAGADA");
}

void leerNivelTanque() {
  digitalWrite(ULTRASONICO_TRIG_PIN, LOW);
  delayMicroseconds(3);
  digitalWrite(ULTRASONICO_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONICO_TRIG_PIN, LOW);

  // Timeout de 30 ms: evita bloquear el servidor web si no hay eco.
  unsigned long duracion = pulseIn(ULTRASONICO_ECHO_PIN, HIGH, 30000UL);
  if (duracion == 0) {
    lecturaTanqueValida = false;
    if (++fallosUltrasonico >= 3) {
      // Si se pierde repetidamente la medicion, detener la bomba por seguridad.
      controlarBomba(false);
    }
    Serial.println("HC-SR04: sin eco; lectura invalida");
    return;
  }

  float distancia = duracion * 0.0343f / 2.0f;
  // Rango practico aproximado del HC-SR04; ignora ecos imposibles.
  if (distancia < 2.0f || distancia > 400.0f) {
    lecturaTanqueValida = false;
    if (++fallosUltrasonico >= 3) controlarBomba(false);
    return;
  }

  fallosUltrasonico = 0;
  lecturaTanqueValida = true;
  distanciaAguaCm = distancia;

  // Porcentaje estimado sobre el recorrido util: de 20 cm (vacio) a 3 cm (lleno).
  // Si la distancia supera 20 cm, el porcentaje queda limitado a 0%.
  const float alturaUtilCm = TANQUE_FONDO_CM - TANQUE_LLENO_CM;
  nivelTanquePct = ((TANQUE_FONDO_CM - distanciaAguaCm) / alturaUtilCm) * 100.0f;
  if (nivelTanquePct < 0) nivelTanquePct = 0;
  if (nivelTanquePct > 100) nivelTanquePct = 100;

  // Histéresis: arranca con nivel bajo y se detiene cerca del lleno.
  if (!estadoBomba && distanciaAguaCm >= BOMBA_ENCIENDE_CM) {
    controlarBomba(true);
  } else if (estadoBomba && distanciaAguaCm <= BOMBA_APAGA_CM) {
    controlarBomba(false);
  }

  Serial.printf("Tanque: %.1f cm | %.0f%% | Bomba: %s\n",
                distanciaAguaCm, nivelTanquePct, estadoBomba ? "ON" : "OFF");
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

// Extrae el valor exacto de sid de la cookie de ESTE navegador.
String tokenCookieActual() {
  if (!server.hasHeader("Cookie")) return "";
  String cookie = server.header("Cookie");
  int pos = cookie.indexOf("sid=");
  if (pos < 0) return "";
  pos += 4;
  int fin = cookie.indexOf(';', pos);
  if (fin < 0) fin = cookie.length();
  String token = cookie.substring(pos, fin);
  token.trim();
  return token;
}

int buscarSesion(const String &token) {
  if (token.length() == 0) return -1;
  unsigned long ahora = millis();
  for (uint8_t i = 0; i < MAX_SESIONES; i++) {
    if (!sesionesWeb[i].activa) continue;
    if (ahora - sesionesWeb[i].ultimoUso > SESSION_TIMEOUT_MS) {
      sesionesWeb[i].token = "";
      sesionesWeb[i].activa = false;
      continue;
    }
    if (sesionesWeb[i].token == token) return (int)i;
  }
  return -1;
}

// Reserva una sesion para un nuevo inicio de sesion. Si las 8 plazas estan
// ocupadas, reemplaza la mas antigua; no toca las sesiones mientras haya plaza.
bool crearSesion(String &tokenNuevo) {
  int plaza = -1;
  unsigned long masAntigua = 0;
  int indiceAntiguo = -1;
  bool hayAntigua = false;
  unsigned long ahora = millis();

  for (uint8_t i = 0; i < MAX_SESIONES; i++) {
    if (sesionesWeb[i].activa &&
        ahora - sesionesWeb[i].ultimoUso > SESSION_TIMEOUT_MS) {
      sesionesWeb[i].token = "";
      sesionesWeb[i].activa = false;
    }
    if (!sesionesWeb[i].activa && plaza < 0) plaza = (int)i;
    if (sesionesWeb[i].activa &&
        (!hayAntigua || sesionesWeb[i].ultimoUso < masAntigua)) {
      masAntigua = sesionesWeb[i].ultimoUso;
      indiceAntiguo = (int)i;
      hayAntigua = true;
    }
  }

  if (plaza < 0) plaza = (indiceAntiguo >= 0) ? indiceAntiguo : 0;
  tokenNuevo = generarToken();
  sesionesWeb[plaza].token = tokenNuevo;
  sesionesWeb[plaza].ultimoUso = ahora;
  sesionesWeb[plaza].activa = true;
  return true;
}

bool autenticado() {
  String token = tokenCookieActual();
  int indice = buscarSesion(token);
  if (indice < 0) return false;
  sesionesWeb[indice].ultimoUso = millis();
  return true;
}

void eliminarSesionActual() {
  String token = tokenCookieActual();
  int indice = buscarSesion(token);
  if (indice >= 0) {
    sesionesWeb[indice].token = "";
    sesionesWeb[indice].activa = false;
    sesionesWeb[indice].ultimoUso = 0;
  }
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
<title>Domotica ESP32</title>
<style>
:root{color-scheme:dark;--bg:#080d18;--panel:#111a2b;--panel2:#0c1423;--line:rgba(148,163,184,.16);--text:#edf4ff;--muted:#91a2bb;--cyan:#38bdf8;--blue:#6366f1;--green:#34d399;--amber:#fbbf24;--red:#fb7185}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:Inter,-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:radial-gradient(ellipse at 12% 0%,rgba(99,102,241,.17),transparent 35%),radial-gradient(ellipse at 100% 20%,rgba(56,189,248,.10),transparent 30%),var(--bg);color:var(--text);padding:28px 20px;min-height:100vh}
.container{max-width:1180px;margin:0 auto;display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:20px;align-items:stretch}
.header{grid-column:1/-1;display:flex;align-items:center;justify-content:space-between;gap:20px;padding:8px 2px 18px;border-bottom:1px solid var(--line);margin-bottom:2px}
.header h1{font-size:clamp(24px,3vw,34px);letter-spacing:-.8px;font-weight:750;background:linear-gradient(100deg,#f8fafc 10%,#7dd3fc 70%,#a5b4fc);-webkit-background-clip:text;background-clip:text;color:transparent;margin-bottom:7px}
.header p{color:var(--muted);font-size:13px;letter-spacing:.2px}
.card{background:linear-gradient(145deg,rgba(20,31,51,.96),rgba(13,21,36,.96));border:1px solid var(--line);border-radius:22px;padding:22px;box-shadow:0 18px 45px rgba(0,0,0,.18);min-width:0;transition:border-color .2s,transform .2s}
.card:hover{border-color:rgba(125,211,252,.28)}
.card h3{color:#dbeafe;font-size:15px;font-weight:650;letter-spacing:.2px;margin-bottom:19px;display:flex;align-items:center;gap:10px}
.section-icon{width:30px;height:30px;display:inline-flex;align-items:center;justify-content:center;border-radius:10px;background:rgba(56,189,248,.12);color:var(--cyan);font-size:17px}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
.metric{background:linear-gradient(145deg,rgba(8,15,29,.78),rgba(14,23,39,.88));border:1px solid rgba(148,163,184,.10);border-radius:16px;padding:17px 12px;text-align:center;min-width:0}
.label{font-size:11px;color:var(--muted);margin-top:7px;letter-spacing:.35px}
.value{font-size:clamp(23px,2.5vw,31px);line-height:1.25;font-weight:750;letter-spacing:-.7px;color:#7dd3fc;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.unit{font-size:12px;color:#9fb2cc;margin-left:3px;font-weight:500}
.emoji{font-size:25px;margin-bottom:9px;filter:saturate(.85)}
.buttons{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}
.light-btn{background:rgba(8,15,29,.65);border:1px solid rgba(148,163,184,.18);border-radius:16px;padding:17px 8px;text-align:center;cursor:pointer;transition:all .2s;color:var(--muted);user-select:none}
.light-btn:hover{transform:translateY(-2px);border-color:rgba(125,211,252,.45);color:var(--text)}
.light-btn.active{background:linear-gradient(145deg,rgba(52,211,153,.18),rgba(16,185,129,.08));color:#a7f3d0;border-color:rgba(52,211,153,.55);box-shadow:0 0 24px rgba(52,211,153,.08)}
.light-btn span{display:block;font-size:25px;margin-bottom:8px}.light-btn small{font-size:12px;font-weight:600;letter-spacing:.3px}
.links{grid-column:1/-1;display:flex;gap:10px;justify-content:flex-start;flex-wrap:wrap;margin-top:0;padding-top:2px}
.links a,.links button{background:rgba(17,26,43,.86);border:1px solid rgba(148,163,184,.19);color:#dbeafe;border-radius:12px;padding:11px 15px;font-size:12px;font-weight:600;text-decoration:none;cursor:pointer;transition:all .2s}
.links a:hover,.links button:hover{border-color:var(--cyan);background:rgba(56,189,248,.08)}
.links .danger{background:rgba(251,113,133,.08);border-color:rgba(251,113,133,.25);color:#fda4af}
.dot{width:9px;height:9px;border-radius:50%;background:var(--green);display:inline-block;box-shadow:0 0 12px rgba(52,211,153,.65);animation:pulse 2s infinite}.dot.off{background:var(--red);box-shadow:none;animation:none}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.45}}
.warn{color:var(--amber);font-size:11px;margin-top:10px;min-height:14px;line-height:1.5}
.footer{grid-column:1/-1;text-align:right;font-size:11px;color:#64748b;margin-top:-7px;padding:0 3px}
#tankBar{background:linear-gradient(90deg,#6366f1,#38bdf8,#34d399)!important;border-radius:10px;box-shadow:0 0 18px rgba(56,189,248,.25)}
.tank-card .metric:last-of-type{border-color:rgba(56,189,248,.16)}
#pumpState{font-size:22px!important;letter-spacing:.6px}
/* Recipiente cilindrico vertical con agua animada */
.tank-visual{display:grid;grid-template-columns:minmax(150px,1fr) 76px;gap:16px;align-items:center;padding:8px 4px 18px}
.tank-stage{height:330px;position:relative;display:flex;align-items:center;justify-content:center;isolation:isolate}
.tank-cylinder{height:300px;width:min(100%,190px);position:relative;overflow:hidden;border:5px solid #9fb6d2;border-top-width:9px;border-bottom-width:10px;border-radius:48%/5%;background:linear-gradient(90deg,rgba(148,163,184,.42),rgba(15,32,56,.18) 18%,rgba(125,211,252,.12) 50%,rgba(8,18,35,.65) 85%,rgba(148,163,184,.38));box-shadow:inset 9px 0 14px rgba(255,255,255,.10),inset -10px 0 18px rgba(0,0,0,.4),0 16px 32px rgba(0,0,0,.3),0 0 24px rgba(56,189,248,.08)}
.tank-cylinder:before,.tank-cylinder:after{content:"";position:absolute;left:-2%;width:104%;height:22px;border-radius:50%;z-index:4;pointer-events:none}
.tank-cylinder:before{top:-10px;background:linear-gradient(180deg,#dbeafe,#64748b 45%,#0f172a);border:1px solid #cbd5e1}
.tank-cylinder:after{bottom:-11px;background:linear-gradient(180deg,#0f172a,#94a3b8 45%,#cbd5e1);border:1px solid #cbd5e1}
.tank-water{position:absolute;left:0;right:0;bottom:0;height:0%;overflow:hidden;background:radial-gradient(ellipse at 28% 22%,rgba(125,211,252,.30),transparent 42%),linear-gradient(90deg,rgba(3,105,161,.94),rgba(14,165,233,.88) 24%,rgba(56,189,248,.76) 48%,rgba(2,132,199,.92) 76%,rgba(7,89,133,.96));transition:height 1.2s cubic-bezier(.22,.7,.25,1);box-shadow:inset 8px 0 16px rgba(186,230,253,.13),inset -10px 0 18px rgba(2,8,23,.28),inset 0 10px 20px rgba(125,211,252,.22)}
.tank-water:before{content:"";position:absolute;z-index:2;left:-60%;top:-13px;width:220%;height:26px;border-radius:44% 48% 42% 46%;background:linear-gradient(180deg,rgba(224,247,255,.94),rgba(125,211,252,.68) 40%,rgba(14,165,233,.10));box-shadow:0 0 12px rgba(56,189,248,.42);animation:waterWave 7s ease-in-out infinite alternate}
.tank-water:after{content:"";position:absolute;z-index:3;left:-55%;top:-7px;width:210%;height:19px;border-radius:47%;border-top:2px solid rgba(240,249,255,.78);border-bottom:1px solid rgba(186,230,253,.35);filter:drop-shadow(0 2px 4px rgba(125,211,252,.45));animation:waterWave 9s ease-in-out infinite alternate-reverse}
.water-shimmer{position:absolute;z-index:1;inset:8px 3px 0;background:linear-gradient(112deg,transparent 8%,rgba(224,247,255,.08) 20%,transparent 31%,rgba(186,230,253,.13) 47%,transparent 59%,rgba(224,247,255,.06) 72%,transparent 84%);background-size:200% 100%;animation:shimmer 7s linear infinite}
.water-bubbles{position:absolute;z-index:1;inset:0;opacity:.58;background-image:radial-gradient(circle at 18% 78%,rgba(224,247,255,.9) 0 1.5px,transparent 3px),radial-gradient(circle at 66% 62%,rgba(224,247,255,.8) 0 1.5px,transparent 3px),radial-gradient(circle at 43% 88%,rgba(224,247,255,.8) 0 1px,transparent 2.5px),radial-gradient(circle at 80% 82%,rgba(224,247,255,.75) 0 1px,transparent 2.5px),radial-gradient(circle at 32% 49%,rgba(224,247,255,.65) 0 1px,transparent 2.5px);animation:bubbles 5.5s ease-in-out infinite}
.tank-ruler{height:300px;display:flex;flex-direction:column;justify-content:space-between;color:#b9cbe3;font-size:11px;padding:2px 0}
.tank-ruler div{border-top:1px solid rgba(125,211,252,.55);padding-top:5px;white-space:nowrap}
.tank-ruler b{display:block;color:#f1f7ff;font-size:13px}
.tank-summary{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:4px}
@keyframes waterWave{0%{transform:translateX(-8%) rotate(-2deg)}50%{transform:translateX(4%) rotate(1deg)}100%{transform:translateX(10%) rotate(-1deg)}}
@keyframes shimmer{from{background-position:100% 0}to{background-position:-100% 0}}
@keyframes bubbles{0%,100%{transform:translateY(5px);opacity:.35}50%{transform:translateY(-6px);opacity:.72}}
@media(max-width:480px){.tank-visual{grid-template-columns:minmax(130px,1fr) 68px;gap:10px}.tank-stage{height:285px}.tank-cylinder{height:260px;width:150px}.tank-ruler{height:260px}.tank-summary .metric{padding:13px 7px}}
@media(prefers-reduced-motion:reduce){.tank-water:before,.tank-water:after,.water-shimmer,.water-bubbles{animation:none}}
@media(min-width:900px){.tank-card{grid-column:2;grid-row:2/4}.climate-card{grid-column:1;grid-row:2}.energy-card{grid-column:1;grid-row:3}.lights-card{grid-column:1/-1}.tank-card .grid{gap:14px}.tank-card .metric{padding:24px 14px}.tank-card #tankBar{margin-top:20px!important}}
@media(max-width:899px){.container{grid-template-columns:1fr}.header,.links,.footer{grid-column:1}.header{align-items:flex-start;flex-direction:column}.card{padding:19px}.footer{text-align:center}}
@media(max-width:420px){body{padding:17px 12px}.grid{gap:9px}.metric{padding:14px 7px}.buttons{gap:8px}.light-btn{padding:14px 5px}.links a,.links button{flex:1;text-align:center}}
</style>
</head>
<body>
<div class="container">
  <div class="header"><div><h1>Domótica inteligente</h1><p>MONITOREO EN TIEMPO REAL · CONTROL DE ENERGÍA Y AGUA</p></div><p><span class="dot" id="dot"></span> &nbsp;ESP32 conectado</p></div>

  <div class="card climate-card">
    <h3><span class="section-icon">◌</span> Clima interior</h3>
    <div class="grid">
      <div class="metric"><div class="emoji">🌡️</div><div><span class="value" id="temp">--</span><span class="unit">°C</span></div><div class="label">Temperatura</div></div>
      <div class="metric"><div class="emoji">💧</div><div><span class="value" id="hum">--</span><span class="unit">%</span></div><div class="label">Humedad</div></div>
    </div>
    <div class="warn" id="dhtWarn"></div>
  </div>

  <div class="card energy-card">
    <h3><span class="section-icon">⚡</span> Energía del sistema</h3>
    <div class="grid">
      <div class="metric"><div class="label">Panel Solar</div><div><span class="value" id="pV">--</span><span class="unit">V</span></div><div class="label" id="pEstado">--</div></div>
      <div class="metric"><div class="label">Bateria</div><div><span class="value" id="bV">--</span><span class="unit">V</span></div><div class="label" id="bPct">--%</div></div>
      <div class="metric"><div class="label">Consumo</div><div><span class="value" id="cW">--</span><span class="unit">W</span></div><div class="label">Potencia carga</div></div>
      <div class="metric"><div class="label">Corriente</div><div><span class="value" id="cA">--</span><span class="unit">A</span></div><div class="label">Sensor ACS712</div></div>
    </div>
    <div class="warn" id="acsWarn"></div>
  </div>

  <div class="card tank-card">
    <h3><span class="section-icon">◉</span> Tanque de agua <span style="margin-left:auto;color:var(--muted);font-size:11px;font-weight:500">Altura: 20 cm</span></h3>
    <div class="tank-visual">
      <div class="tank-stage" aria-label="Representacion grafica del nivel del agua">
        <div class="tank-cylinder"><div class="tank-water" id="tankWater"><div class="water-shimmer"></div><div class="water-bubbles"></div></div></div>
      </div>
      <div class="tank-ruler" aria-hidden="true">
        <div><b>100%</b><span>3 cm</span></div><div><b>75%</b><span>7.3 cm</span></div><div><b>50%</b><span>11.5 cm</span></div><div><b>25%</b><span>15.8 cm</span></div><div><b>0%</b><span>20 cm</span></div>
      </div>
    </div>
    <div class="tank-summary">
      <div class="metric"><div class="emoji">💧</div><div><span class="value" id="tankPct">--</span><span class="unit">%</span></div><div class="label">Nivel del tanque</div><div style="height:8px;background:#263449;border-radius:8px;overflow:hidden;margin-top:12px"><div id="tankBar" style="height:100%;width:0%;background:linear-gradient(90deg,#0284c7,#38bdf8);transition:width .6s;border-radius:8px"></div></div></div>
      <div class="metric"><div class="emoji">📏</div><div><span class="value" id="tankDist">--</span><span class="unit">cm</span></div><div class="label">Distancia al agua</div></div>
    </div>
    <div class="metric" style="margin-top:12px"><div class="label">Estado de la mini bomba · GPIO 25</div><div class="value" id="pumpState">--</div><div class="label" id="tankMsg">Esperando lectura del sensor</div></div>
    <div class="warn" id="tankWarn"></div>
  </div>

  <div class="card lights-card">
    <h3><span class="section-icon">✦</span> Control de luces</h3>
    <div class="buttons">
      <div class="light-btn" id="luz1Btn" onclick="toggleLight(1)"><span>💡</span><small>Luz 1</small></div>
      <div class="light-btn" id="luz2Btn" onclick="toggleLight(2)"><span>💡</span><small>Luz 2</small></div>
      <div class="light-btn" id="luz3Btn" onclick="toggleLight(3)"><span>💡</span><small>Luz 3</small></div>
    </div>
  </div>

  <div class="links">
    <a href="/login" id="authLink">Iniciar sesion</a>
    <a href="/config">Configurar WiFi</a>
    <button onclick="calibrar()">Recalibrar corriente</button>
    <button class="danger" onclick="resetWifi()">Resetear WiFi</button>
  </div>
  <div class="footer" id="footer">v3.1</div>
</div>

<script>
let luces = {luz1:false, luz2:false, luz3:false};
let enCurso = false;
const $ = id => document.getElementById(id);

async function actualizar() {
  if (enCurso) return;
  enCurso = true;
  const controlador = new AbortController();
  const temporizador = setTimeout(() => controlador.abort(), 7000);
  try {
    const r = await fetch('/api/status?_=' + Date.now(), {
      method: 'GET',
      cache: 'no-store',
      credentials: 'same-origin',
      signal: controlador.signal
    });
    if (r.status === 401) {
      $('dot').classList.add('off');
      $('authLink').textContent = 'Sesion vencida: iniciar sesion';
      location.href = '/login?next=/';
      return;
    }
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const d = await r.json();
    if (!d || !d.panel || !d.bateria || !d.carga || !d.luces || !d.tanque) {
      throw new Error('Respuesta incompleta del ESP32');
    }
    $('dot').classList.remove('off');
    $('temp').textContent = d.dht_ok ? Number(d.temperatura).toFixed(1) : '--';
    $('hum').textContent  = d.dht_ok ? Number(d.humedad).toFixed(1) : '--';
    $('dhtWarn').textContent = d.dht_ok ? '' : 'Sensor DHT11 sin lectura';
    $('pV').textContent = Number(d.panel.voltaje).toFixed(1);
    $('pEstado').textContent = d.panel.cargando ? 'Cargando' : 'Sin carga';
    $('bV').textContent = Number(d.bateria.voltaje).toFixed(1);
    $('bPct').textContent = Number(d.bateria.porcentaje).toFixed(0) + '%';
    $('cW').textContent = Number(d.carga.watts).toFixed(1);
    $('cA').textContent = Number(d.carga.corriente).toFixed(2);
    let w = [];
    if (d.alertas && d.alertas.saturado) w.push('Sensor saturado: corriente mayor a la medible');
    if (d.alertas && !d.alertas.offset_ok) w.push('Offset ACS712 fuera de rango');
    $('acsWarn').textContent = w.join(' | ');
    if (d.tanque.valido) {
      $('tankPct').textContent = Number(d.tanque.porcentaje).toFixed(0);
      $('tankDist').textContent = Number(d.tanque.distancia_cm).toFixed(1);
      $('tankBar').style.width = Number(d.tanque.porcentaje).toFixed(0) + '%';
      $('tankWater').style.height = Number(d.tanque.porcentaje).toFixed(1) + '%';
      $('pumpState').textContent = d.tanque.bomba ? 'ENCENDIDA' : 'APAGADA';
      $('pumpState').style.color = d.tanque.bomba ? '#38bdf8' : '#34d399';
      $('tankMsg').textContent = d.tanque.bomba ? 'Llenando el tanque' : 'Bomba detenida';
      $('tankWarn').textContent = '';
    } else {
      $('tankPct').textContent = '--';
      $('tankDist').textContent = '--';
      $('tankBar').style.width = '0%';
      $('tankWater').style.height = '0%';
      $('pumpState').textContent = d.tanque.bomba ? 'ENCENDIDA' : 'APAGADA';
      $('tankMsg').textContent = 'Lectura no disponible · bomba en modo seguro';
      $('tankWarn').textContent = 'Revisa el cableado y la posicion del HC-SR04';
    }
    luces = d.luces;
    for (let i = 1; i <= 3; i++) $('luz'+i+'Btn').classList.toggle('active', !!luces['luz'+i]);
    $('authLink').textContent = d.sesion ? 'Cerrar sesion' : 'Iniciar sesion';
    $('authLink').href = d.sesion ? '/logout' : '/login';
    $('footer').textContent = 'v3.5 | WiFi ' + d.rssi + ' dBm | activo ' + Math.floor(d.uptime/60) + ' min';
  } catch (e) {
    $('dot').classList.add('off');
    console.warn('No se pudo actualizar el estado:', e);
  } finally {
    clearTimeout(temporizador);
    enCurso = false;
  }
}

async function toggleLight(n) {
  const boton = $('luz'+n+'Btn');
  boton.style.pointerEvents = 'none';
  try {
    const controlador = new AbortController();
    const temporizador = setTimeout(() => controlador.abort(), 7000);
    const nuevo = !luces['luz'+n];
    let r;
    try {
      r = await fetch('/api/light', {
        method:'POST', cache:'no-store', credentials:'same-origin',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({luz:n, estado:nuevo}), signal:controlador.signal
      });
    } finally { clearTimeout(temporizador); }
    if (r.status === 401) { location.href = '/login?next=/'; return; }
    if (!r.ok) throw new Error('HTTP ' + r.status);
    await actualizar();
  } catch (e) {
    console.warn('No se pudo controlar la luz:', e);
    $('tankWarn').textContent = 'No se pudo enviar el comando. Verifica la conexión e intenta de nuevo.';
  } finally {
    boton.style.pointerEvents = '';
  }
}

async function calibrar() {
  if (!confirm('Apaga todas las cargas antes de recalibrar. Continuar?')) return;
  const r = await fetch('/api/calibrar', {method:'POST'});
  if (r.status === 401) { location.href = '/login?next=/'; return; }
  actualizar();
}

async function resetWifi() {
  if (!confirm('Resetear configuracion WiFi? El ESP32 se reiniciara.')) return;
  const r = await fetch('/resetwifi', {method:'POST'});
  if (r.status === 401) { location.href = '/login?next=/'; return; }
  setTimeout(() => location.reload(), 4000);
}

setInterval(actualizar, 2000);
actualizar();
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// EVENTOS WIFI (diagnostico)
// ============================================================================
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFi] Desconectado. Motivo: %d\n", info.wifi_sta_disconnected.reason);
      mdnsActivo = false;                    // se reinicia al reconectar
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] IP obtenida: %s\n", WiFi.localIP().toString().c_str());
      break;
    default:
      break;
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== SISTEMA DOMOTICO ESP32 v3.3 - TANQUE ===");

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

  // Bomba apagada al arrancar por seguridad.
  digitalWrite(BOMBA_PIN, LOW);
  pinMode(BOMBA_PIN, OUTPUT);
  estadoBomba = false;
  pinMode(ULTRASONICO_TRIG_PIN, OUTPUT);
  digitalWrite(ULTRASONICO_TRIG_PIN, LOW);
  pinMode(ULTRASONICO_ECHO_PIN, INPUT);

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
  ipGuardada = preferences.getString("ip", "");
  gatewayGuardado = preferences.getString("gateway", "");
  mascaraGuardada = preferences.getString("mask", "");
  dnsGuardado = preferences.getString("dns", "");
  preferences.end();

  WiFi.onEvent(onWiFiEvent);
  configurarRutas();

  if (ssidGuardado == "") {
    iniciarPortal(false);
  } else if (!conectarWiFi()) {
    Serial.println("No se pudo conectar. Portal de configuracion activo, se sigue reintentando.");
    iniciarPortal(true);                     // NO se borran las credenciales
  }

  iniciarMDNS();
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

  if (ahora - ultimoUltrasonico >= ULTRASONICO_INTERVAL_MS) {
    ultimoUltrasonico = ahora;
    leerNivelTanque();
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
    Serial.printf("Panel %.2fV | Bat %.2fV (%.0f%%) | I %.3fA | P %.2fW | Sat:%d OffsetOK:%d | Heap %u | WiFi:%d RSSI:%d IP:%s\n",
                  vPanel, vBat, pctBat, corriente, potenciaCarga,
                  sensorSaturado, offsetOk, ESP.getFreeHeap(),
                  WiFi.status() == WL_CONNECTED, WiFi.RSSI(),
                  WiFi.localIP().toString().c_str());
  }
}

// ============================================================================
// WIFI
// ============================================================================
// Ajustes comunes: desactiva el ahorro de energia (evita que deje de responder)
void configurarWiFiComun() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
}

// Aplica IP estatica si se configuro; si el campo IP esta vacio, usa DHCP.
void aplicarIPFija() {
  if (ipGuardada.length() == 0) {
    Serial.println("Red: IP automatica por DHCP");
    return;
  }
  IPAddress ip, gateway, mascara, dns;
  if (!ip.fromString(ipGuardada) || !gateway.fromString(gatewayGuardado) ||
      !mascara.fromString(mascaraGuardada)) {
    Serial.println("Configuracion IP invalida; se usara DHCP");
    return;
  }
  if (dnsGuardado.length() == 0 || !dns.fromString(dnsGuardado)) dns = gateway;
  if (WiFi.config(ip, gateway, mascara, dns)) {
    Serial.printf("IP estatica configurada: %s | Gateway: %s | Mascara: %s | DNS: %s\n",
                  ip.toString().c_str(), gateway.toString().c_str(),
                  mascara.toString().c_str(), dns.toString().c_str());
  } else {
    Serial.println("No se pudo aplicar IP estatica; se intentara DHCP");
  }
}

void mostrarInfoRed() {
  Serial.printf("IP: %s | Mascara: %s | Gateway: %s | DNS: %s | RSSI: %d dBm | Canal: %d\n",
                WiFi.localIP().toString().c_str(),
                WiFi.subnetMask().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.dnsIP().toString().c_str(),
                WiFi.RSSI(),
                WiFi.channel());
  Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());
}

void iniciarMDNS() {
  if (mdnsActivo) return;
  if (WiFi.status() != WL_CONNECTED) return;
  MDNS.end();
  if (MDNS.begin(MDNS_NOMBRE)) {
    MDNS.addService("http", "tcp", 80);
    mdnsActivo = true;
    Serial.printf("Tambien disponible en http://%s.local\n", MDNS_NOMBRE);
  } else {
    Serial.println("No se pudo iniciar mDNS");
  }
}

bool conectarWiFi() {
  WiFi.mode(WIFI_STA);
  configurarWiFiComun();

  aplicarIPFija();
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
      WiFi.setSleep(false);                  // reafirmar tras conectar
      Serial.printf("Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
      mostrarInfoRed();
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
  configurarWiFiComun();
  WiFi.softAP(AP_SSID, AP_PASS);
  portalActivo = true;
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("AP '%s' activo - http://%s\n", AP_SSID, apIP.toString().c_str());
  actualizarLCD("Conectate a:", AP_SSID);
  delay(2000);
  actualizarLCD("Web:", apIP.toString());
  if (conSTA && ssidGuardado != "") {
    aplicarIPFija();
    WiFi.begin(ssidGuardado.c_str(), passGuardado.c_str());
  }
}

void mantenerWiFi() {
  static unsigned long ultimo = 0;
  static int fallosGateway = 0;
  if (millis() - ultimo < REINTENTO_WIFI_MS) return;
  ultimo = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (portalActivo) {                      // ya hay red: cerrar el AP
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      configurarWiFiComun();
      portalActivo = false;
      Serial.printf("WiFi restablecido. IP: %s\n", WiFi.localIP().toString().c_str());
      mostrarInfoRed();
    }

    iniciarMDNS();                           // no hace nada si ya esta activo

    // Watchdog: "conectado" pero sin gateway valido = conexion zombie
    IPAddress gw = WiFi.gatewayIP();
    if (gw == IPAddress(0, 0, 0, 0)) {
      if (++fallosGateway >= 2) {
        Serial.println("Conexion invalida (sin gateway). Reconectando...");
        fallosGateway = 0;
        WiFi.disconnect();
        aplicarIPFija();
        WiFi.begin(ssidGuardado.c_str(), passGuardado.c_str());
      }
    } else {
      fallosGateway = 0;
    }
    return;
  }

  if (ssidGuardado == "") return;            // solo modo AP, nada que reintentar
  Serial.println("WiFi caido, reintentando...");
  WiFi.disconnect();
  aplicarIPFija();
  WiFi.begin(ssidGuardado.c_str(), passGuardado.c_str());
}

void borrarCredenciales() {
  preferences.begin("wifi", false);
  preferences.putString("ssid", "");
  preferences.putString("pass", "");
  preferences.putString("ip", "");
  preferences.putString("gateway", "");
  preferences.putString("mask", "");
  preferences.putString("dns", "");
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
  server.on("/ping", HTTP_GET, handlePing);
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

void handlePing() {
  Serial.printf("GET /ping desde %s\n", server.client().remoteIP().toString().c_str());
  if (!autenticado()) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(401, "text/plain", "No autorizado");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "pong");
}

void handleRoot() {
  Serial.printf("GET / desde %s\n", server.client().remoteIP().toString().c_str());
  // No se entrega el HTML principal sin una sesion valida.
  if (!autenticado()) {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Location", portalActivo ? "/login?next=/config" : "/login?next=/");
    server.send(302, "text/plain", "");
    return;
  }
  if (portalActivo) {
    server.sendHeader("Location", "/config");
    server.send(302, "text/plain", "");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PAGINA_HTML);
}

void handleNotFound() {
  Serial.printf("404: %s desde %s\n", server.uri().c_str(), server.client().remoteIP().toString().c_str());
  if (!autenticado()) {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
    return;
  }
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

  doc["tanque"]["valido"] = lecturaTanqueValida;
  doc["tanque"]["distancia_cm"] = distanciaAguaCm;
  doc["tanque"]["porcentaje"] = nivelTanquePct;
  doc["tanque"]["bomba"] = estadoBomba;

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

  int luz = 0;
  bool estado = false;

  // Metodo preferido: parametros de URL enviados por el servidor C#.
  // Ejemplo: POST /api/light?luz=2&estado=true
  if (server.hasArg("luz") && server.hasArg("estado")) {
    luz = server.arg("luz").toInt();
    String estadoTexto = server.arg("estado");
    estadoTexto.toLowerCase();
    estado = (estadoTexto == "true" || estadoTexto == "1" || estadoTexto == "on");
  } else if (server.hasArg("plain")) {
    // Compatibilidad con la pagina original que envia JSON en el cuerpo.
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      server.send(400, "application/json", "{\"error\":\"JSON invalido\"}");
      return;
    }
    luz = doc["luz"] | 0;
    estado = doc["estado"] | false;
  } else {
    server.send(400, "application/json", "{\"error\":\"Faltan parametros luz/estado\"}");
    return;
  }

  if (luz < 1 || luz > 3) {
    server.send(400, "application/json", "{\"error\":\"Luz invalida (1-3)\"}");
    return;
  }

  Serial.printf("Orden luz %d = %s\n", luz, estado ? "ENCENDIDA" : "APAGADA");
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
  "<style>body{font-family:sans-serif;background:#0a0a0a;color:#fff;display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
  ".card{background:#1a1a1a;padding:30px;border-radius:15px;width:320px;box-shadow:0 4px 15px rgba(0,0,0,.3)}"
  "h1{color:#39d98a;font-size:24px;margin-bottom:10px}"
  "input{width:100%;padding:10px;margin:10px 0;background:#2a2a2a;border:1px solid #3a3a3a;color:#fff;border-radius:5px;box-sizing:border-box}"
  "button{width:100%;padding:10px;background:#39d98a;border:none;border-radius:5px;color:#000;font-weight:bold;cursor:pointer}"
  ".error{color:#ff6b6b;margin-bottom:10px}.info{font-size:12px;color:#aaa;margin-top:15px;text-align:center}a{color:#39d98a}</style>";

void handleLogin() {
  String next = server.hasArg("next") ? server.arg("next") : (portalActivo ? "/config" : "/");
  if (next != "/config") next = "/";        // solo destinos permitidos

  if (server.method() == HTTP_GET && autenticado()) {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Location", next);
    server.send(302, "text/plain", "");
    return;
  }
  String error = "";

  if (server.method() == HTTP_POST) {
    if (millis() < bloqueoHasta) {
      error = "Demasiados intentos. Espera 1 minuto";
    } else if (server.arg("usuario") == ADMIN_USER && server.arg("clave") == ADMIN_PASS) {
      loginFallos = 0;
      String tokenNuevo;
      crearSesion(tokenNuevo);
      server.sendHeader("Set-Cookie", "sid=" + tokenNuevo + "; Path=/; HttpOnly; SameSite=Strict");
      server.sendHeader("Location", next);
      server.send(302, "text/plain", "");
      return;
    } else {
      error = "Credenciales incorrectas";
      if (++loginFallos >= 5) {
        bloqueoHasta = millis() + 60000UL;
        loginFallos = 0;
      }
      delay(500);
    }
  }

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width'><title>Ingresar</title>";
  html += FPSTR(ESTILO_PAGINAS);
  html += "</head><body><div class='card'><h1>Ingresar</h1>";
  if (error != "") html += "<div class='error'>" + error + "</div>";
  html += "<form method='POST' action='/login'>"
          "<input type='hidden' name='next' value='" + next + "'>"
          "<input type='text' name='usuario' placeholder='Usuario' required>"
          "<input type='password' name='clave' placeholder='Contrasena' required>"
          "<button type='submit'>Ingresar</button></form></div></body></html>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

void handleLogout() {
  if (!autenticado()) {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
    return;
  }
  eliminarSesionActual();
  server.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  server.sendHeader("Location", portalActivo ? "/login" : "/");
  server.send(302, "text/plain", "");
}

void handleConfig() {
  if (!autenticado()) {
    server.sendHeader("Location", "/login?next=/config");
    server.send(302, "text/plain", "");
    return;
  }
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width'><title>Configurar red</title>";
  html += FPSTR(ESTILO_PAGINAS);
  html += "</head><body><div class='card'><h1>Configurar red</h1>"
          "<form method='POST' action='/guardar'>"
          "<input type='text' name='ssid' placeholder='Nombre WiFi (SSID)' required>"
          "<input type='password' name='pass' placeholder='Contrasena WiFi (vacia si es red abierta)'>"
          "<hr><p>IP del ESP32 en la red WiFi</p>"
          "<input type='text' name='ip' placeholder='IP fija (ej. 192.168.1.50)'>"
          "<input type='text' name='gateway' placeholder='Puerta de enlace (ej. 192.168.1.1)'>"
          "<input type='text' name='mask' placeholder='Mascara (ej. 255.255.255.0)'>"
          "<input type='text' name='dns' placeholder='DNS (opcional; por defecto gateway)'>"
          "<div class='info'>Deje IP, gateway y mascara vacios para usar IP automatica (DHCP). La IP fija debe estar libre y pertenecer a la misma red del router.</div>"
          "<button type='submit'>Guardar y Conectar</button></form>"
          "<div class='info'>El dispositivo se reiniciara automaticamente.<br>Portal de configuracion: 192.168.4.1<br><br><a href='/'>Volver</a> | <a href='/logout'>Cerrar sesion</a></div>"
          "</div></body></html>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

void handleGuardar() {
  if (!autenticado()) {
    server.send(403, "text/plain", "No autorizado");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String ipTxt = server.arg("ip");
  String gatewayTxt = server.arg("gateway");
  String maskTxt = server.arg("mask");
  String dnsTxt = server.arg("dns");

  if (ssid == "") {
    server.send(400, "text/plain", "SSID requerido");
    return;
  }
  if (pass.length() > 0 && pass.length() < 8) {
    server.send(400, "text/plain", "La contrasena WPA debe tener al menos 8 caracteres");
    return;
  }

  // Si se indica IP fija, gateway y mascara son obligatorios y deben ser IPv4 validas.
  if (ipTxt.length() > 0) {
    IPAddress ipTmp, gwTmp, maskTmp, dnsTmp;
    if (gatewayTxt.length() == 0 || maskTxt.length() == 0 ||
        !ipTmp.fromString(ipTxt) || !gwTmp.fromString(gatewayTxt) ||
        !maskTmp.fromString(maskTxt) ||
        (dnsTxt.length() > 0 && !dnsTmp.fromString(dnsTxt))) {
      server.send(400, "text/plain", "IP fija invalida. Revise IP, gateway, mascara y DNS.");
      return;
    }
  } else if (gatewayTxt.length() > 0 || maskTxt.length() > 0 || dnsTxt.length() > 0) {
    server.send(400, "text/plain", "Para configurar gateway, mascara o DNS, primero indique la IP fija.");
    return;
  }

  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putString("ip", ipTxt);
  preferences.putString("gateway", gatewayTxt);
  preferences.putString("mask", maskTxt);
  preferences.putString("dns", dnsTxt);
  preferences.end();

  actualizarLCD("Guardando WiFi", "Reiniciando...");
  server.send(200, "text/html", "<html><body><h2>Datos guardados. Reiniciando...</h2></body></html>");
  delay(1500);
  ESP.restart();
}
