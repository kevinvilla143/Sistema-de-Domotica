# Panel Domótica en Ubuntu Server

```
Navegador ──HTTPS──► Ubuntu Server (nginx → Flask/gunicorn) ──HTTP──► ESP32 (relés, sensores)
                      login + sesión + interfaz                         solo su API, conexiones intactas
```

- El **login y la sesión viven en el servidor**. Al pulsar *Cerrar sesión* la sesión se destruye en el servidor:
  no se puede volver a entrar (ni con "atrás", ni con la cookie vieja, ni escribiendo `/`) sin usuario y contraseña.
- El servidor inicia sesión solo en el ESP32 con sus credenciales actuales y se reconecta si el ESP32 las invalida.
- No se cambió ningún pin, WiFi, factor de sensor ni credencial del ESP32.

## 1. Instalar en el Ubuntu Server

Desde tu PC (ajusta usuario e IP del servidor):

```bash
scp -r domotica_ubuntu usuario@IP_SERVIDOR:~/
ssh usuario@IP_SERVIDOR
cd ~/domotica_ubuntu
sudo bash deploy/install.sh
```

El script instala: `python3`, `python3-venv`, `python3-pip`, `nginx`, `openssl`, `ufw`, `curl`;
crea el entorno Python (Flask, requests, gunicorn), el servicio `domotica-panel`, el certificado HTTPS
autofirmado y abre solo SSH, 80 y 443. Te pregunta:

1. IP del ESP32 (la que muestra el LCD / monitor serie)
2. Usuario y clave admin **del ESP32** (por defecto `admin` / `admin1234`)
3. Usuario y contraseña **nuevos para entrar al panel**

Al terminar abre `https://IP_DEL_SERVIDOR` (el navegador avisará del certificado autofirmado: normal en red local).

> **Importante:** reserva en tu router una IP fija (reserva DHCP) para el ESP32 y para el servidor,
> para que `ESP32_URL` no cambie.

## 2. Firmware del ESP32 (opcional pero recomendado)

`firmware/domotica_esp32.ino` es tu código con **solo** estos cambios (el original está en
`domotica_esp32.original.ino`): tildes/ñ en las páginas, `charset=utf-8`, sin cursor de texto al pulsar,
y `/` y `/api/status` ahora exigen login (antes eran públicos). Reprográmalo con el IDE de Arduino como siempre.
El panel funciona igual con el firmware original o con el nuevo.

## 3. Operación

```bash
sudo systemctl status domotica-panel          # estado
sudo journalctl -u domotica-panel -f          # logs
sudo systemctl restart domotica-panel         # reiniciar
sudo nano /etc/domotica/panel.env             # IP del ESP32, tiempos de sesión
```

Cambiar la contraseña del panel:

```bash
/opt/domotica/venv/bin/python -c "from werkzeug.security import generate_password_hash as g; print(g('NUEVA_CLAVE'))"
# pega el resultado en PANEL_PASS_HASH de /etc/domotica/panel.env  y reinicia el servicio
```

Si cambias la IP del ESP32: edita `ESP32_URL` en `panel.env` y reinicia el servicio.

## 4. Seguridad: pendientes tuyos

Tu firmware sigue con `ADMIN_PASS = "admin1234"` y `AP_PASS = "domotica123"` (te lo avisaba el propio código: *CAMBIALA*).
No los toqué porque son parámetros de conexión; si los cambias, actualiza `ESP32_PASS` en `panel.env`.

Notas técnicas: sesión caduca a los 30 min sin uso (12 h máx.); 5 intentos fallidos bloquean 1 min;
las sesiones están en memoria, por eso gunicorn usa **un solo worker** (reiniciar el servicio cierra todas las sesiones).
