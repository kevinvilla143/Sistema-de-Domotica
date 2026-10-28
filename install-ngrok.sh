#!/usr/bin/env bash
# =====================================================================================
#  Instala el agente de ngrok como servicio systemd y publica la web de Domotica ESP32.
#
#      sudo NGROK_AUTHTOKEN=xxxxxxxx bash deploy/install-ngrok.sh
#  Con dominio estático (recomendado: la URL no cambia al reiniciar):
#      sudo NGROK_AUTHTOKEN=xxxxxxxx NGROK_DOMAIN=mi-casa.ngrok-free.app bash deploy/install-ngrok.sh
#
#  El authtoken y tu dominio gratuito están en https://dashboard.ngrok.com
# =====================================================================================
set -euo pipefail

CONFIG_DIR="/etc/ngrok"
CONFIG_FILE="${CONFIG_DIR}/ngrok.yml"
ENV_FILE="/etc/domotica/domotica.env"

log()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m !!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m XX\033[0m %s\n' "$*" >&2; exit 1; }

if [[ $EUID -ne 0 ]]; then
  command -v sudo >/dev/null || die "Ejecuta como root o instala sudo."
  exec sudo -E bash "$0" "$@"
fi
command -v systemctl >/dev/null || die "Se necesita systemd."
command -v curl >/dev/null || { apt-get update -y && apt-get install -y curl ca-certificates; }

# --- Parámetros -----------------------------------------------------------------------
token="${NGROK_AUTHTOKEN:-}"
if [[ -z "${token}" && -t 0 ]]; then
  read -r -s -p "Authtoken de ngrok (https://dashboard.ngrok.com/get-started/your-authtoken): " token || true
  echo
fi
[[ -n "${token}" ]] || die "Falta NGROK_AUTHTOKEN."
[[ "${token}" =~ ^[A-Za-z0-9_]+$ ]] || die "El authtoken solo debe contener letras, números y guion bajo."

domain="${NGROK_DOMAIN:-}"
domain="${domain#http://}"; domain="${domain#https://}"; domain="${domain%/}"
[[ -z "${domain}" || "${domain}" =~ ^[A-Za-z0-9.-]+$ ]] || die "NGROK_DOMAIN no es un dominio válido."

port="${APP_PORT:-}"
if [[ -z "${port}" && -r "${ENV_FILE}" ]]; then
  port="$(sed -n 's/^ASPNETCORE_URLS=.*:\([0-9]\{2,5\}\)\/\{0,1\}$/\1/p' "${ENV_FILE}" | tail -1)"
fi
port="${port:-5036}"

# --- 1. Agente ------------------------------------------------------------------------
case "$(uname -m)" in
  x86_64|amd64)   arch="amd64" ;;
  aarch64|arm64)  arch="arm64" ;;
  armv7l|armv6l)  arch="arm"   ;;
  *) die "Arquitectura no soportada: $(uname -m)" ;;
esac
log "Descargando el agente de ngrok (linux-${arch})..."
tmp="$(mktemp /tmp/ngrok.XXXXXX.tgz)"
trap 'rm -f "${tmp}"' EXIT
curl -fsSL "https://bin.equinox.io/c/bNyj1mQVY4c/ngrok-v3-stable-linux-${arch}.tgz" -o "${tmp}" \
  || die "No pude descargar ngrok. Alternativa: instálalo con 'sudo snap install ngrok' y vuelve a ejecutar."
tar -xzf "${tmp}" -C /usr/local/bin ngrok
chmod 755 /usr/local/bin/ngrok
log "$(ngrok version | head -1)"

# --- 2. Configuración (formato v3) ----------------------------------------------------
install -d -m 700 "${CONFIG_DIR}"
{
  echo 'version: 3'
  echo 'agent:'
  echo "  authtoken: ${token}"
  echo 'endpoints:'
  echo '  - name: domotica'
  [[ -n "${domain}" ]] && echo "    url: https://${domain}"
  echo '    upstream:'
  echo "      url: ${port}"
} > "${CONFIG_FILE}"
chmod 600 "${CONFIG_FILE}"
ngrok config check --config "${CONFIG_FILE}" || die "La configuración de ngrok no es válida."

# --- 3. Servicio ----------------------------------------------------------------------
log "Instalando ngrok como servicio (arranca solo con el servidor)..."
ngrok service stop      >/dev/null 2>&1 || true
ngrok service uninstall >/dev/null 2>&1 || true
ngrok service install --config "${CONFIG_FILE}"
ngrok service start

# --- 4. URL pública -------------------------------------------------------------------
log "Esperando a que el túnel se establezca..."
url=""
for _ in $(seq 1 20); do
  url="$(curl -fsS -m 2 http://127.0.0.1:4040/api/tunnels 2>/dev/null | grep -o '"public_url":"https://[^"]*"' | head -1 | cut -d'"' -f4 || true)"
  [[ -n "${url}" ]] && break
  sleep 1
done
[[ -z "${url}" && -n "${domain}" ]] && url="https://${domain}"

echo
echo "--------------------------------------------------------------------------"
if [[ -n "${url}" ]]; then
  echo " Tu web está publicada en:  ${url}"
else
  echo " El servicio está instalado pero no pude leer la URL automáticamente."
  echo " Míralo en https://dashboard.ngrok.com/endpoints  o con:  sudo systemctl status ngrok"
fi
echo " Estado:  sudo systemctl status ngrok"
echo " Logs:    sudo journalctl -u ngrok -f"
echo "--------------------------------------------------------------------------"
echo " Si ves un aviso de ngrok al abrir la URL por primera vez, es normal en el plan gratuito"
echo " (pulsa 'Visit Site'). Después, la web pedirá el usuario y la clave de Domotica."
