"""Panel Domótica: servidor web para Ubuntu Server.

- Login y sesión viven AQUÍ (en el servidor), no en el ESP32.
- Al cerrar sesión la sesión se destruye en el servidor: para volver a entrar
  hay que escribir usuario y contraseña otra vez.
- Todo (página, API) exige sesión válida.
- Hace de proxy hacia el ESP32 y gestiona él solo el login del ESP32
  (el ESP32 no se modifica: sus conexiones/credenciales quedan igual).

IMPORTANTE: las sesiones están en memoria -> ejecutar con UN solo worker
(gunicorn --workers 1 --threads 4), como hace el servicio systemd incluido.
"""
import os
import secrets
import threading
import time

import requests
from flask import Flask, jsonify, make_response, redirect, request, send_from_directory
from werkzeug.security import check_password_hash

BASE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(BASE, "static")


def env(name, default=None, required=False):
    v = os.environ.get(name, default)
    if required and not v:
        raise SystemExit(f"Falta la variable {name} en /etc/domotica/panel.env")
    return v


ESP_URL = env("ESP32_URL", required=True).rstrip("/")
ESP_USER = env("ESP32_USER", "admin")
ESP_PASS = env("ESP32_PASS", required=True)
PANEL_USER = env("PANEL_USER", required=True)
PANEL_PASS_HASH = env("PANEL_PASS_HASH", required=True)
COOKIE_SECURE = env("COOKIE_SECURE", "0") == "1"
IDLE_SECONDS = int(env("SESSION_IDLE_MINUTES", "30")) * 60
MAX_SECONDS = int(env("SESSION_MAX_HOURS", "12")) * 3600
COOKIE = "domo_sid"

app = Flask(__name__, static_folder=None)

# ---------------------------------------------------------------- sesiones --
_sessions = {}            # token -> {"created": t, "last": t}
_sess_lock = threading.Lock()
_fails = {}               # ip -> [contador, bloqueado_hasta]


def _now():
    return time.monotonic()


def session_valid(touch=True):
    tok = request.cookies.get(COOKIE, "")
    if not tok:
        return False
    with _sess_lock:
        s = _sessions.get(tok)
        if not s:
            return False
        t = _now()
        if t - s["last"] > IDLE_SECONDS or t - s["created"] > MAX_SECONDS:
            _sessions.pop(tok, None)
            return False
        if touch:
            s["last"] = t
        return True


def new_session():
    tok = secrets.token_urlsafe(32)
    t = _now()
    with _sess_lock:
        # limpieza de sesiones vencidas
        for k in [k for k, v in _sessions.items()
                  if t - v["last"] > IDLE_SECONDS or t - v["created"] > MAX_SECONDS]:
            _sessions.pop(k, None)
        _sessions[tok] = {"created": t, "last": t}
    return tok


def destroy_session():
    tok = request.cookies.get(COOKIE, "")
    with _sess_lock:
        _sessions.pop(tok, None)


def no_store(resp):
    resp.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
    resp.headers["Pragma"] = "no-cache"
    resp.headers["Expires"] = "0"
    return resp


def clear_cookie(resp):
    resp.set_cookie(COOKIE, "", max_age=0, expires=0, path="/", httponly=True,
                    samesite="Strict", secure=COOKIE_SECURE)
    return resp


def api_guard():
    """None si todo bien; si no, respuesta de error."""
    if not session_valid():
        return no_store(clear_cookie(make_response(jsonify(error="Sesión no válida"), 401)))
    if request.method == "POST" and request.headers.get("X-Requested-With") != "domotica":
        return no_store(make_response(jsonify(error="Petición no permitida"), 403))
    return None


@app.after_request
def security_headers(resp):
    resp.headers.setdefault("X-Content-Type-Options", "nosniff")
    resp.headers.setdefault("X-Frame-Options", "DENY")
    resp.headers.setdefault("Referrer-Policy", "same-origin")
    return resp


# ------------------------------------------------------------- cliente ESP32 --
class Esp:
    """Habla con el ESP32 (un solo hilo a la vez: el ESP32 atiende de a uno)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.sid = None
        self.cache = None          # (momento, json)

    def _login(self):
        r = requests.post(
            ESP_URL + "/login",
            data={"usuario": ESP_USER, "clave": ESP_PASS, "next": "/"},
            headers={"Connection": "close"},
            allow_redirects=False, timeout=5,
        )
        sid = r.cookies.get("sid")
        if r.status_code == 302 and sid:
            self.sid = sid
            return True
        self.sid = None
        return False

    def call(self, method, path, timeout=5, **kw):
        """Devuelve (response, error). Reintenta una vez si el ESP32 cerró su sesión."""
        with self.lock:
            for intento in (1, 2):
                try:
                    if not self.sid and not self._login():
                        return None, "El ESP32 rechazó las credenciales configuradas"
                    r = requests.request(
                        method, ESP_URL + path, timeout=timeout,
                        headers={"Cookie": f"sid={self.sid}", "Connection": "close"}, **kw)
                except requests.RequestException:
                    return None, "No hay conexión con el ESP32"
                if r.status_code in (401, 403):
                    self.sid = None
                    continue
                return r, None
            return None, "No se pudo autenticar en el ESP32"

    def status(self):
        with self.lock:
            if self.cache and _now() - self.cache[0] < 1.0:
                return self.cache[1], None
        for intento in (1, 2):
            r, err = self.call("GET", "/api/status", timeout=4)
            if err:
                return None, err
            try:
                data = r.json()
            except ValueError:
                return None, "Respuesta inválida del ESP32"
            if data.get("sesion") is False and intento == 1:
                self.sid = None          # sesión del ESP32 caducada: reingresar
                continue
            with self.lock:
                self.cache = (_now(), data)
            return data, None
        return None, "No se pudo leer el estado"


esp = Esp()


# ------------------------------------------------------------------ páginas --
def page(name):
    resp = make_response(send_from_directory(STATIC, name))
    return no_store(resp)


@app.get("/")
def index():
    if not session_valid():
        return no_store(clear_cookie(redirect("/login")))
    return page("index.html")


@app.get("/login")
def login_page():
    if session_valid(touch=False):
        return no_store(redirect("/"))
    return page("login.html")


@app.post("/login")
def login_post():
    ip = request.headers.get("X-Real-IP", request.remote_addr)
    t = _now()
    rec = _fails.get(ip, [0, 0])
    if t < rec[1]:
        espera = int(rec[1] - t) + 1
        return no_store(make_response(
            jsonify(error=f"Demasiados intentos. Espera {espera} s e inténtalo de nuevo."), 429))

    data = request.get_json(silent=True) or {}
    user = str(data.get("usuario", ""))
    pw = str(data.get("clave", ""))
    ok_user = secrets.compare_digest(user.encode(), PANEL_USER.encode())
    ok_pass = check_password_hash(PANEL_PASS_HASH, pw)
    if ok_user and ok_pass:
        _fails.pop(ip, None)
        tok = new_session()
        resp = make_response(jsonify(ok=True))
        resp.set_cookie(COOKIE, tok, httponly=True, samesite="Strict",
                        secure=COOKIE_SECURE, path="/")   # cookie de sesión (sin Max-Age)
        return no_store(resp)

    rec[0] += 1
    if rec[0] >= 5:
        rec = [0, t + 60]
    _fails[ip] = rec
    time.sleep(0.6)
    return no_store(make_response(jsonify(error="Usuario o contraseña incorrectos"), 401))


@app.route("/logout", methods=["GET", "POST"])
def logout():
    destroy_session()
    if request.method == "GET":
        resp = redirect("/login")
    else:
        resp = make_response(jsonify(ok=True))
    return no_store(clear_cookie(resp))


@app.get("/static/<path:name>")
def static_files(name):
    resp = make_response(send_from_directory(STATIC, name))
    resp.headers["Cache-Control"] = "no-cache"
    return resp


# --------------------------------------------------------------------- API --
@app.get("/api/status")
def api_status():
    bad = api_guard()
    if bad:
        return bad
    data, err = esp.status()
    if err:
        return no_store(jsonify(online=False, error=err))
    data = dict(data)
    data["online"] = True
    data["sesion"] = True
    data["esp_url"] = ESP_URL
    return no_store(jsonify(data))


@app.post("/api/light")
def api_light():
    bad = api_guard()
    if bad:
        return bad
    body = request.get_json(silent=True) or {}
    try:
        luz = int(body.get("luz"))
        estado = bool(body.get("estado"))
    except (TypeError, ValueError):
        return no_store(make_response(jsonify(error="Datos inválidos"), 400))
    if luz not in (1, 2, 3):
        return no_store(make_response(jsonify(error="Luz inválida"), 400))
    r, err = esp.call("POST", "/api/light", json={"luz": luz, "estado": estado})
    if err or r.status_code != 200:
        return no_store(make_response(jsonify(error=err or "El ESP32 no aceptó el cambio"), 502))
    esp.cache = None
    return no_store(jsonify(ok=True))


@app.post("/api/calibrar")
def api_calibrar():
    bad = api_guard()
    if bad:
        return bad
    r, err = esp.call("POST", "/api/calibrar", timeout=12)
    if err or r.status_code != 200:
        return no_store(make_response(jsonify(error=err or "No se pudo recalibrar"), 502))
    esp.cache = None
    return no_store(jsonify(ok=True))


@app.post("/api/resetwifi")
def api_resetwifi():
    bad = api_guard()
    if bad:
        return bad
    r, err = esp.call("POST", "/resetwifi", timeout=6)
    if err and "conexión" not in err:
        return no_store(make_response(jsonify(error=err), 502))
    esp.sid = None
    esp.cache = None
    return no_store(jsonify(ok=True))


@app.errorhandler(404)
def nf(_):
    if request.path.startswith("/api/"):
        return no_store(make_response(jsonify(error="No encontrado"), 404))
    return no_store(redirect("/login"))


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=int(env("PORT", "8000")))
