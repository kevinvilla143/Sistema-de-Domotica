#!/usr/bin/env bash
# =====================================================================================
#  Instala / actualiza "Domotica ESP32" como servicio systemd en Ubuntu Server.
#
#  Uso (desde la carpeta del proyecto, por SSH):
#      bash deploy/install.sh
#  o sin preguntas:
#      sudo ESP32_URL=http://192.168.1.112/ bash deploy/install.sh
#
#  Variables opcionales: ESP32_URL, APP_PORT (5036), AUTH_USER (admin),
#                        AUTH_PASSWORD (si no se da, se genera una), AUTH_DISABLE=1
#  Es idempotente: volver a ejecutarlo ACTUALIZA la app y conserva tu configuración.
# =====================================================================================
set -euo pipefail

SERVICE_NAME="domotica"
APP_USER="domotica"
INSTALL_DIR="/opt/domotica"
ENV_DIR="/etc/domotica"
ENV_FILE="${ENV_DIR}/domotica.env"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

log()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m !!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m XX\033[0m %s\n' "$*" >&2; exit 1; }

# --- root -----------------------------------------------------------------------------
if [[ $EUID -ne 0 ]]; then
  command -v sudo >/dev/null || die "Ejecuta como root o instala sudo."
  exec sudo -E bash "$0" "$@"
fi

[[ -f "${PROJECT_DIR}/DomoticaESP32.Server.csproj" ]] || die "No encuentro DomoticaESP32.Server.csproj en ${PROJECT_DIR}"
command -v systemctl >/dev/null || die "Se necesita systemd (Ubuntu Server lo trae por defecto)."
# shellcheck disable=SC1091
[[ -r /etc/os-release ]] && . /etc/os-release
[[ "${ID:-}" == "ubuntu" ]] || warn "Pensado para Ubuntu (detectado: ${PRETTY_NAME:-desconocido}); puede requerir ajustes."

export DEBIAN_FRONTEND=noninteractive

# --- 1. .NET 10 -----------------------------------------------------------------------
have_dotnet10() { command -v dotnet >/dev/null && dotnet --list-sdks 2>/dev/null | grep -q '^10\.'; }

install_dotnet() {
  log "Instalando .NET 10 SDK..."
  apt-get update -y
  apt-get install -y curl ca-certificates tar zlib1g
  if [[ "${VERSION_ID:-}" == "22.04" ]]; then
    apt-get install -y software-properties-common
    add-apt-repository -y ppa:dotnet/backports
    apt-get update -y
  fi
  if apt-get install -y dotnet-sdk-10.0; then
    return
  fi
  warn "apt no tiene dotnet-sdk-10.0; uso el instalador oficial de Microsoft (dot.net)."
  curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh
  bash /tmp/dotnet-install.sh --channel 10.0 --install-dir /usr/share/dotnet
  ln -sf /usr/share/dotnet/dotnet /usr/local/bin/dotnet
}

if have_dotnet10; then
  log ".NET 10 SDK ya instalado ($(dotnet --version))."
else
  install_dotnet
  have_dotnet10 || die "No se pudo instalar .NET 10."
fi
DOTNET_BIN="$(readlink -f "$(command -v dotnet)")"
command -v curl >/dev/null || apt-get install -y curl

# --- 2. Usuario del servicio ----------------------------------------------------------
if ! id -u "${APP_USER}" >/dev/null 2>&1; then
  log "Creando usuario de sistema '${APP_USER}'..."
  useradd --system --no-create-home --shell /usr/sbin/nologin "${APP_USER}"
fi

# --- 3. Compilar y publicar -----------------------------------------------------------
log "Compilando (Release)..."
BUILD_DIR="$(mktemp -d /tmp/domotica-build.XXXXXX)"
trap 'rm -rf "${BUILD_DIR}"' EXIT
mkdir -p "${BUILD_DIR}/src" "${BUILD_DIR}/home"
# Se compila desde una copia: así no quedan bin/obj de root dentro de tu carpeta de proyecto.
tar -C "${PROJECT_DIR}" --exclude='./bin' --exclude='./obj' --exclude='./publish' --exclude='./.git' -cf - . \
  | tar -C "${BUILD_DIR}/src" -xf -
export HOME="${BUILD_DIR}/home" DOTNET_CLI_HOME="${BUILD_DIR}/home" DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
# Sin servidores de compilación residentes: en un servidor pequeño solo gastarían RAM durante ~10 min.
dotnet publish "${BUILD_DIR}/src/DomoticaESP32.Server.csproj" -c Release -o "${BUILD_DIR}/out" --nologo -v minimal \
  -p:UseSharedCompilation=false -nodeReuse:false
dotnet build-server shutdown >/dev/null 2>&1 || true

log "Desplegando en ${INSTALL_DIR}..."
rm -rf "${INSTALL_DIR}.new"
cp -a "${BUILD_DIR}/out" "${INSTALL_DIR}.new"
systemctl stop "${SERVICE_NAME}" 2>/dev/null || true
rm -rf "${INSTALL_DIR}.old"
[[ -d "${INSTALL_DIR}" ]] && mv "${INSTALL_DIR}" "${INSTALL_DIR}.old"
mv "${INSTALL_DIR}.new" "${INSTALL_DIR}"
rm -rf "${INSTALL_DIR}.old"
chown -R root:root "${INSTALL_DIR}"
chmod -R go-w "${INSTALL_DIR}"

# --- 4. Configuración (solo si no existe) ---------------------------------------------
install -d -m 750 -o root -g "${APP_USER}" "${ENV_DIR}"
GENERATED_PASSWORD=""

if [[ ! -f "${ENV_FILE}" ]]; then
  esp32_url="${ESP32_URL:-}"
  if [[ -z "${esp32_url}" && -t 0 ]]; then
    read -r -p "IP o URL del ESP32 en tu red local [http://192.168.1.112/]: " esp32_url || true
  fi
  esp32_url="${esp32_url:-http://192.168.1.112/}"
  [[ "${esp32_url}" =~ ^https?:// ]] || esp32_url="http://${esp32_url}"

  auth_user="${AUTH_USER:-admin}"
  auth_password="${AUTH_PASSWORD:-}"
  if [[ "${AUTH_DISABLE:-0}" == "1" ]]; then
    auth_password=""
    warn "Contraseña desactivada (AUTH_DISABLE=1): no lo expongas con ngrok sin protección."
  elif [[ -z "${auth_password}" ]]; then
    auth_password="$(head -c 24 /dev/urandom | base64 | tr -d '/+=\n' | cut -c1-16)"
    GENERATED_PASSWORD="${auth_password}"
  fi
  [[ "${auth_password}" =~ ^[A-Za-z0-9._~@%+=:,-]*$ ]] || die "AUTH_PASSWORD: usa solo letras, números y . _ ~ @ % + = : , -"
  [[ "${auth_user}" =~ ^[A-Za-z0-9._-]+$ ]] || die "AUTH_USER: usa solo letras, números, punto, guion y guion bajo."

  log "Creando ${ENV_FILE}..."
  cat > "${ENV_FILE}" <<ENVEOF
# Configuración de Domotica ESP32.  Tras editar:  sudo systemctl restart ${SERVICE_NAME}
ASPNETCORE_ENVIRONMENT=Production
# Para aceptar solo conexiones locales (ngrok / túnel SSH) usa http://127.0.0.1:${APP_PORT:-5036}
ASPNETCORE_URLS=http://0.0.0.0:${APP_PORT:-5036}
DOTNET_NOLOGO=1
DOTNET_CLI_TELEMETRY_OPTOUT=1

# ESP32 en la red local (el servidor Ubuntu debe estar en la MISMA red)
Esp32__BaseUrl=${esp32_url}
# Opcionales (valores por defecto):
#Esp32__TimeoutSeconds=6        # espera máxima por respuesta del ESP32
#Esp32__CacheMilliseconds=2500  # los navegadores comparten una misma lectura durante este tiempo
#Esp32__MaxSessions=20          # tope de sesiones iniciadas a la vez

# Contraseña de ENTRADA a la web (capa previa). Es distinta del usuario/clave del ESP32 (firmware),
# que se pide después dentro de la página para ver datos y controlar luces. Vacía = sin esta capa.
Auth__User=${auth_user}
Auth__Password=${auth_password}
ENVEOF
else
  log "Conservo la configuración existente (${ENV_FILE})."
fi
chown root:"${APP_USER}" "${ENV_FILE}"
chmod 640 "${ENV_FILE}"

# --- 5. Servicio systemd --------------------------------------------------------------
log "Instalando servicio systemd..."
sed "s|@DOTNET@|${DOTNET_BIN}|g" "${SCRIPT_DIR}/domotica.service" > "${SERVICE_FILE}"
systemctl daemon-reload
systemctl enable "${SERVICE_NAME}" >/dev/null
systemctl restart "${SERVICE_NAME}"

# --- 6. Verificación ------------------------------------------------------------------
port="$(sed -n 's/^ASPNETCORE_URLS=.*:\([0-9]\{2,5\}\)\/\{0,1\}$/\1/p' "${ENV_FILE}" | tail -1)"; port="${port:-5036}"
esp32_cfg="$(sed -n 's/^Esp32__BaseUrl=//p' "${ENV_FILE}" | tail -1)"

log "Esperando a que el servicio responda en el puerto ${port}..."
ok=0
for _ in $(seq 1 20); do
  if curl -fsS -m 2 "http://127.0.0.1:${port}/healthz" >/dev/null 2>&1; then ok=1; break; fi
  sleep 1
done
if [[ "${ok}" -ne 1 ]]; then
  journalctl -u "${SERVICE_NAME}" -n 40 --no-pager || true
  die "El servicio no arrancó. Revisa el log de arriba."
fi
log "Servidor web en marcha."

# El firmware exige login también en /api/status: un 401 significa "el ESP32 respondió", que es lo que buscamos.
esp32_code="$(curl -s -m 5 -o /dev/null -w '%{http_code}' "${esp32_cfg%/}/ping" 2>/dev/null || true)"
if [[ "${esp32_code}" =~ ^[1-5][0-9][0-9]$ ]]; then
  log "ESP32 alcanzable desde este servidor (${esp32_cfg}, respondió HTTP ${esp32_code})."
else
  warn "Este servidor NO consigue hablar con el ESP32 en ${esp32_cfg}."
  warn "Comprueba: que el ESP32 esté encendido, que su IP sea esa y que el servidor esté en la misma red local."
fi

cat <<EOF

--------------------------------------------------------------------------
 Listo.  Web local:  http://<IP-del-servidor>:${port}/
 Estado:   sudo systemctl status ${SERVICE_NAME}
 Logs:     sudo journalctl -u ${SERVICE_NAME} -f
 Config:   sudo nano ${ENV_FILE}   (luego: sudo systemctl restart ${SERVICE_NAME})
 Actualizar: copia la nueva versión y vuelve a ejecutar  bash deploy/install.sh
--------------------------------------------------------------------------
EOF
if [[ -n "${GENERATED_PASSWORD}" ]]; then
  cat <<EOF
 CONTRASEÑA DE ENTRADA A LA WEB (guárdala, solo se muestra ahora):
     usuario: ${auth_user}
     clave:   ${GENERATED_PASSWORD}
 (también está en ${ENV_FILE})
 Después, la página pide el usuario y la clave del ESP32 (los del firmware).
--------------------------------------------------------------------------
EOF
fi
echo " Siguiente paso: exponerlo con ngrok ->  sudo NGROK_AUTHTOKEN=tu_token bash deploy/install-ngrok.sh"
