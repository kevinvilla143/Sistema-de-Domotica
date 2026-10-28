#!/usr/bin/env python3
"""
Simulador del ESP32 (firmware "corregido") para probar el servidor SIN el hardware.
Imita: login con cookie sid (302), /api/status (401 sin sesión), /api/light, /logout,
límite de 8 sesiones y bloqueo de 1 min tras 5 intentos fallidos.

    python3 tools/esp32_mock.py                # escucha en 0.0.0.0:8081
    python3 tools/esp32_mock.py --port 80      # (puerto 80 requiere sudo)

Usuario/clave por defecto: admin / admin1234.  Solo usa la librería estándar de Python 3.
Apunta el servidor a él:  Esp32__BaseUrl=http://127.0.0.1:8081/
"""
import argparse, json, math, secrets, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

STARTED = time.time()
LOCK = threading.Lock()
SESSIONS = {}                      # sid -> último uso
LIGHTS = {1: False, 2: False, 3: False}
FAILS = 0
BLOCKED_UNTIL = 0.0
HITS = {"status": 0}
ARGS = None
MAX_SESSIONS, SESSION_TIMEOUT = 8, 30 * 60


def sid_of(handler):
    for part in handler.headers.get("Cookie", "").split(";"):
        k, _, v = part.strip().partition("=")
        if k == "sid":
            return v
    return ""


def authenticated(handler):
    sid = sid_of(handler)
    with LOCK:
        last = SESSIONS.get(sid)
        if last is None or time.time() - last > SESSION_TIMEOUT:
            SESSIONS.pop(sid, None)
            return False
        SESSIONS[sid] = time.time()
        return True


def status_json():
    t = time.time() - STARTED
    level = 55 + 30 * math.sin(t / 40)
    bat = 12.4 + 0.3 * math.sin(t / 60)
    panel = 18.2 + 1.5 * math.sin(t / 25)
    return {
        "temperatura": round(24 + 2 * math.sin(t / 30), 1), "humedad": int(55 + 5 * math.sin(t / 50)), "dht_ok": True,
        "panel": {"voltaje": round(panel, 2), "cargando": panel > bat + 0.5},
        "bateria": {"voltaje": round(bat, 2), "porcentaje": int((bat - 11.5) / 1.5 * 100)},
        "carga": {"watts": round(10 + 2 * math.sin(t / 10), 1), "corriente": round(0.8 + 0.2 * math.sin(t / 10), 2)},
        "luces": {"luz1": LIGHTS[1], "luz2": LIGHTS[2], "luz3": LIGHTS[3]},
        "tanque": {"valido": True, "distancia_cm": round(20 * (1 - level / 100), 1), "porcentaje": round(level, 1), "bomba": level < 40},
        "alertas": {"saturado": False, "offset_ok": True},
        "sesion": True, "rssi": -58, "uptime": int(t),
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"      # como el WebServer del ESP32: una respuesta por conexión

    def _send(self, code, body=b"", ctype="application/json", headers=()):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        global HITS
        path = urlparse(self.path).path
        if path == "/api/status":
            if ARGS.delay:
                time.sleep(ARGS.delay)
            if not authenticated(self):
                return self._send(401, {"error": "No autorizado"})
            HITS["status"] += 1
            return self._send(200, status_json())
        if path == "/ping":
            return self._send(200, "pong", "text/plain") if authenticated(self) else self._send(401, "No autorizado", "text/plain")
        if path == "/logout":
            with LOCK:
                SESSIONS.pop(sid_of(self), None)
            return self._send(302, "", "text/plain", [("Location", "/"), ("Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly")])
        if path == "/__hits":
            return self._send(200, str(HITS["status"]), "text/plain")
        if path == "/":
            return self._send(200, "<h1>ESP32 simulado</h1>", "text/html")
        self._send(404, "No encontrado", "text/plain")

    def do_POST(self):
        global FAILS, BLOCKED_UNTIL
        url = urlparse(self.path)
        length = int(self.headers.get("Content-Length") or 0)
        form = parse_qs(self.rfile.read(length).decode()) if length else {}
        query = parse_qs(url.query)
        arg = lambda k: (query.get(k) or form.get(k) or [""])[0]

        if url.path == "/login":
            with LOCK:
                if time.time() < BLOCKED_UNTIL:
                    return self._send(200, "Demasiados intentos. Espera 1 minuto", "text/html")
                if arg("usuario") == ARGS.user and arg("clave") == ARGS.password:
                    FAILS = 0
                    now = time.time()
                    for k in [k for k, v in SESSIONS.items() if now - v > SESSION_TIMEOUT]:
                        del SESSIONS[k]
                    if len(SESSIONS) >= MAX_SESSIONS:                 # como el firmware: reemplaza la más antigua
                        del SESSIONS[min(SESSIONS, key=SESSIONS.get)]
                    sid = secrets.token_hex(16)
                    SESSIONS[sid] = now
                    return self._send(302, "", "text/plain", [("Location", "/"), ("Set-Cookie", f"sid={sid}; Path=/; HttpOnly; SameSite=Strict")])
                FAILS += 1
                if FAILS >= 5:
                    BLOCKED_UNTIL, FAILS = time.time() + 60, 0
            time.sleep(0.5)
            return self._send(200, "Credenciales incorrectas", "text/html")

        if url.path == "/api/light":
            if not authenticated(self):
                return self._send(401, {"error": "No autorizado"})
            luz, estado = arg("luz"), arg("estado").lower()
            if not luz:                                               # compatibilidad: JSON en el cuerpo
                return self._send(400, {"error": "Faltan parametros luz/estado"})
            if not luz.isdigit() or not 1 <= int(luz) <= 3:
                return self._send(400, {"error": "Luz invalida (1-3)"})
            LIGHTS[int(luz)] = estado in ("true", "1", "on")
            return self._send(200, {"ok": True})

        self._send(404, "No encontrado", "text/plain")

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin1234")
    ap.add_argument("--delay", type=float, default=0, help="segundos de retraso en /api/status (simula un ESP32 lento)")
    ARGS = ap.parse_args()
    print(f"ESP32 simulado en http://{ARGS.host}:{ARGS.port}/  usuario={ARGS.user}  (Ctrl+C para salir)")
    ThreadingHTTPServer((ARGS.host, ARGS.port), Handler).serve_forever()
