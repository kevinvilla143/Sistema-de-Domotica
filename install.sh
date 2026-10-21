#!/usr/bin/env bash
# Instala el Panel Domótica en Ubuntu Server (22.04 / 24.04).
# Uso:  sudo bash deploy/install.sh        (desde la carpeta del proyecto)
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "Ejecuta con sudo."; exit 1; }
SRC="$(cd "$(dirname "$0")/.." && pwd)"
APP=/opt/domotica
ENVF=/etc/domotica/panel.env

echo "==> 1/7 Paquetes del sistema"
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y python3 python3-venv python3-pip nginx openssl ufw curl

echo "==> 2/7 Usuario y carpetas"
id domotica >/dev/null 2>&1 || useradd --system --home "$APP" --shell /usr/sbin/nologin domotica
mkdir -p "$APP" /etc/domotica /etc/ssl/domotica
rm -rf "$APP/app"; cp -r "$SRC/app" "$APP/app"
cp "$SRC/deploy/requirements.txt" "$APP/requirements.txt"

echo "==> 3/7 Entorno Python"
python3 -m venv "$APP/venv"
"$APP/venv/bin/pip" install --quiet --upgrade pip
"$APP/venv/bin/pip" install --quiet -r "$APP/requirements.txt"
chown -R domotica:domotica "$APP"

echo "==> 4/7 Configuración (solo la primera vez)"
if [ ! -f "$ENVF" ]; then
  read -rp "IP del ESP32 en tu red (ej. 192.168.1.50): " ESPIP
  read -rp "Usuario admin del ESP32 [admin]: " EU; EU=${EU:-admin}
  read -rsp "Contraseña admin del ESP32 [admin1234]: " EP; echo; EP=${EP:-admin1234}
  read -rp "Usuario para entrar al panel: " PU
  while true; do
    read -rsp "Contraseña del panel (mín. 8 caracteres): " PP; echo
    read -rsp "Repite la contraseña: " PP2; echo
    [ "$PP" = "$PP2" ] && [ ${#PP} -ge 8 ] && break
    echo "No coinciden o son muy cortas. Intenta de nuevo."
  done
  HASH=$("$APP/venv/bin/python" -c 'import sys;from werkzeug.security import generate_password_hash as g;print(g(sys.argv[1]))' "$PP")
  umask 077
  cat > "$ENVF" <<EOT
ESP32_URL=http://$ESPIP
ESP32_USER=$EU
ESP32_PASS=$EP
PANEL_USER=$PU
PANEL_PASS_HASH=$HASH
COOKIE_SECURE=1
SESSION_IDLE_MINUTES=30
SESSION_MAX_HOURS=12
EOT
  chown root:domotica "$ENVF"; chmod 640 "$ENVF"
else
  echo "   $ENVF ya existe: no se modifica."
fi

echo "==> 5/7 Certificado HTTPS autofirmado (10 años)"
if [ ! -f /etc/ssl/domotica/domotica.crt ]; then
  IP=$(hostname -I | awk '{print $1}')
  openssl req -x509 -nodes -newkey rsa:2048 -days 3650 \
    -keyout /etc/ssl/domotica/domotica.key -out /etc/ssl/domotica/domotica.crt \
    -subj "/CN=domotica" -addext "subjectAltName=IP:${IP},DNS:domotica,DNS:$(hostname)"
  chmod 600 /etc/ssl/domotica/domotica.key
fi

echo "==> 6/7 Servicio y nginx"
cp "$SRC/deploy/domotica-panel.service" /etc/systemd/system/domotica-panel.service
cp "$SRC/deploy/nginx-domotica.conf" /etc/nginx/sites-available/domotica
ln -sf /etc/nginx/sites-available/domotica /etc/nginx/sites-enabled/domotica
rm -f /etc/nginx/sites-enabled/default
nginx -t
systemctl daemon-reload
systemctl enable --now domotica-panel
systemctl restart domotica-panel
systemctl enable nginx && systemctl restart nginx

echo "==> 7/7 Firewall (SSH + web)"
ufw allow OpenSSH >/dev/null
ufw allow 80/tcp >/dev/null
ufw allow 443/tcp >/dev/null
ufw --force enable >/dev/null

sleep 2
systemctl --no-pager --lines=0 status domotica-panel | head -4 || true
echo
echo "Listo. Abre:  https://$(hostname -I | awk '{print $1}')"
echo "(el navegador avisará del certificado autofirmado: es normal en red local)"
