# Domótica ESP32 — servidor web (v4)

Panel web (ASP.NET Core, .NET 10) que **lee** los datos del ESP32 y **controla las 3 luces**.

```
 Navegador ──HTTPS──▶ ngrok ──▶ Ubuntu Server (:5036) ──HTTP──▶ ESP32 (misma red local)
   (cualquier lugar)            esta aplicación                   login propio + datos + luces
```

El ESP32 **nunca se expone a internet**: solo lo consulta el servidor, que debe estar en su misma red.

## Novedades de la v4

- **Iconos vectoriales (SVG)** en toda la página: ya no dependen de la fuente del dispositivo.
- **Ojo para mostrar/ocultar la contraseña** del ESP32 en el formulario de acceso (se vuelve a ocultar sola tras iniciar sesión).
- **Lectura compartida:** varios navegadores reutilizan una misma lectura del ESP32 (2,5 s), así que 1 o 5 dispositivos abiertos
  cargan igual al microcontrolador. Esto evita los errores 503/504 que se veían con dos dispositivos a la vez.
- **Tiempos coherentes:** el servidor responde con un error claro (a los 6 s) antes de que el navegador se rinda (a los 12 s).
- **Sesión vencida:** si el ESP32 se reinicia o cierra la sesión (30 min sin uso), la página vuelve sola al formulario con un aviso.
- **Registro de accesos** con la IP real del visitante (ver «Quién entra»).
- **Tope de sesiones** y sin sesiones para quien no se autentica (no se puede llenar la memoria con peticiones sueltas).
- Corregido: el tiempo activo mostraba la unidad dos veces («12s s»).

## Dos contraseñas (a propósito)

| # | Quién la pide | Dónde se define | Para qué |
|---|---|---|---|
| 1 | El navegador (ventana emergente) | `Auth__Password` en `/etc/domotica/domotica.env` | Que solo tú llegues a la página por ngrok |
| 2 | La página (formulario, con ojo para verla) | `ADMIN_USER` / `ADMIN_PASS` en el firmware `.ino` | Que el ESP32 te deje ver datos y mover luces |

La #1 protege a la #2: el ESP32 se bloquea **1 minuto para todos** tras 5 intentos fallidos. Sin la capa #1,
cualquiera con tu URL de ngrok podría bloquearte el acceso a propósito.
La ventana de la #1 la dibuja el navegador, por eso no tiene el botón del ojo.

> **Cambia `ADMIN_PASS` y `AP_PASS` en el firmware** (vienen como `admin1234` y `domotica123`) y vuelve a cargarlo.
> Con la web pública, una clave así se adivina enseguida.

## Quién entra

```bash
sudo journalctl -u domotica -f                                  # en vivo
sudo journalctl -u domotica --since today | grep -E "Inicio de sesión|Luz|Sesión|incorrecta"
```

Verás líneas como `Inicio de sesión en el ESP32 CORRECTO desde 181.54.x.x`, `Luz 2 -> ENCENDIDA desde ...`,
`Inicio de sesión ... FALLIDO desde ...` y `Contraseña de ENTRADA incorrecta desde ...`.
Para ver el historial de visitas en general, usa **Traffic Inspector** en el panel de ngrok.

## Actualizar el servidor Ubuntu que ya tienes

Tu configuración (IP del ESP32 y contraseña de entrada) **se conserva**: vive en `/etc/domotica/domotica.env`.

1. Sube el zip nuevo desde tu PC (nombre distinto al anterior):
   ```bash
   scp DomoticaESP32_v4.zip vboxuser@IP_DEL_SERVIDOR:~
   ```
2. En el servidor:
   ```bash
   cd ~
   sudo cp -a /opt/domotica /opt/domotica.bak       # copia de seguridad (opcional)
   rm -rf DomoticaESP32.Server Firmware_ESP32
   unzip -o DomoticaESP32_v4.zip
   cd DomoticaESP32.Server
   bash deploy/install.sh
   ```
3. Al terminar debe decir `Servidor web en marcha` y `ESP32 alcanzable ... respondió HTTP 401`
   (el 401 es normal: el ESP32 responde y pide login).
4. En el navegador, **Ctrl+F5**. Entra con la contraseña #1 y luego inicia sesión con la #2.

No hace falta tocar ngrok: sigue apuntando al mismo puerto (5036).

Si el ESP32 aún no tiene el firmware nuevo, ábrelo en Arduino IDE
(`Firmware_ESP32/DomoticaESP32_Firmware_corregido.ino`), elige placa y puerto, y cárgalo.
**Esta carpeta no cambió respecto a la tuya.**

## Instalación desde cero

```bash
unzip DomoticaESP32_v4.zip && cd DomoticaESP32.Server
bash deploy/install.sh                       # pregunta la IP del ESP32 y genera la contraseña de entrada
sudo NGROK_AUTHTOKEN=tu_token NGROK_DOMAIN=tu-dominio.ngrok-free.app bash deploy/install-ngrok.sh
```

`install.sh` instala .NET 10, compila, crea el servicio systemd `domotica` (arranca solo y se reinicia si
falla) y comprueba el ESP32. `install-ngrok.sh` deja ngrok como servicio y muestra la URL pública.

## Configuración (`/etc/domotica/domotica.env`; luego `sudo systemctl restart domotica`)

| Variable | Por defecto | Para qué |
|---|---|---|
| `Esp32__BaseUrl` | `http://192.168.1.112/` | IP o URL del ESP32 |
| `Esp32__TimeoutSeconds` | `6` | Espera máxima por respuesta del ESP32 |
| `Esp32__CacheMilliseconds` | `2500` | Tiempo que los navegadores comparten una misma lectura |
| `Esp32__MaxSessions` | `20` | Tope de sesiones iniciadas a la vez (el ESP32 admite 8) |
| `Auth__User` / `Auth__Password` | `admin` / generada | Contraseña de entrada (#1). Vacía = sin esa capa |
| `ASPNETCORE_URLS` | `http://0.0.0.0:5036` | Puerto/interfaz. `http://127.0.0.1:5036` = solo ngrok y túnel SSH |

## Si cambia la IP del ESP32

- Mejor: fíjala. En la página `http://IP_ESP32/config` del propio ESP32 (tras iniciar sesión) hay campos de
  **IP fija, puerta de enlace y máscara**. Elige una IP libre de tu red.
- Si cambió igualmente: `sudo nano /etc/domotica/domotica.env`, corrige `Esp32__BaseUrl`, y
  `sudo systemctl restart domotica`.
- El firmware también se anuncia como `domotica.local`; en Ubuntu necesitas `sudo apt install libnss-mdns`
  para usar `Esp32__BaseUrl=http://domotica.local/`.

## Operación

```bash
sudo systemctl status domotica        # estado          sudo journalctl -u domotica -f   # logs
sudo systemctl status ngrok           # túnel           sudo journalctl -u ngrok -f
curl http://127.0.0.1:5036/healthz    # debe responder: ok
```

## Probar sin el ESP32

```bash
python3 tools/esp32_mock.py                          # ESP32 simulado (admin / admin1234) en :8081
Esp32__BaseUrl=http://127.0.0.1:8081/ dotnet run     # en otra terminal
```

## Problemas frecuentes

| Síntoma | Causa / solución |
|---|---|
| «Error de comunicación con el ESP32» | Desde el servidor: `curl -v http://IP_ESP32/ping` (debe responder, aunque sea 401). Revisa IP y red. |
| «El ESP32 está ocupado» (503) o «Tiempo de espera agotado» (504) | El ESP32 tarda en responder (señal WiFi débil o hotspot lento). Prueba: `curl -w "%{time_total}\n" -o /dev/null -s http://IP_ESP32/ping`. Se reintenta solo. |
| «Demasiados intentos. Espera 1 minuto» | 5 logins fallidos seguidos en el ESP32. Espera 1 min. |
| «La sesión venció» | El ESP32 cierra sesiones tras 30 min sin uso o si se reinició. Inicia sesión otra vez. |
| La web pide dos claves | Es lo esperado (ver tabla de arriba). |
| Los iconos no aparecen | Recarga sin caché (Ctrl+F5): el navegador tenía la versión vieja de la página. |
| `install.sh` falla con `\r` / `invalid option` | Finales de línea de Windows: `sed -i 's/\r$//' deploy/*.sh` |
| El reloj de la VM va atrasado | `sudo timedatectl set-ntp true` (con la hora mal, ngrok puede fallar). |
