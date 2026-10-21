(() => {
  const $ = id => document.getElementById(id);
  const H = { 'Content-Type': 'application/json', 'X-Requested-With': 'domotica' };
  const pendientes = new Set();   // luces con un cambio en curso: el sondeo no las pisa
  let luces = { luz1: false, luz2: false, luz3: false };

  // Si el navegador restaura la página desde caché (botón "atrás" tras cerrar sesión), recargar:
  // el servidor pedirá login otra vez.
  window.addEventListener('pageshow', e => { if (e.persisted) location.reload(); });

  const toLogin = () => location.replace('/login');
  async function api(path, opts = {}) {
    const r = await fetch(path, { cache: 'no-store', ...opts });
    if (r.status === 401) { toLogin(); throw new Error('sesion'); }
    return r;
  }

  function toast(msg) {
    const t = $('toast'); t.textContent = msg; t.classList.add('show');
    clearTimeout(toast.h); toast.h = setTimeout(() => t.classList.remove('show'), 3200);
  }

  function confirmar(titulo, texto, peligro) {
    return new Promise(res => {
      const d = $('dlg');
      $('dlgTitle').textContent = titulo; $('dlgText').textContent = texto;
      $('dlgYes').className = 'btn-solid' + (peligro ? ' danger' : '');
      const fin = v => { d.close(); $('dlgYes').onclick = $('dlgNo').onclick = null; res(v); };
      $('dlgYes').onclick = () => fin(true); $('dlgNo').onclick = () => fin(false);
      d.oncancel = () => res(false);
      d.showModal();
    });
  }

  function pintarLuz(n, on) {
    const b = $('luz' + n);
    b.setAttribute('aria-checked', on ? 'true' : 'false');
    b.querySelector('.state').textContent = on ? 'Encendida' : 'Apagada';
  }

  const f1 = (v, d = 1) => (typeof v === 'number' && isFinite(v)) ? v.toFixed(d) : '--';

  function pintar(d) {
    const on = d.online !== false;
    $('dot').classList.toggle('off', !on);
    $('connText').textContent = on ? ('En línea' + (d.rssi ? ' · WiFi ' + d.rssi + ' dBm' : '')) : 'Sin conexión';
    const bn = $('banner');
    bn.classList.toggle('show', !on);
    if (!on) { bn.textContent = (d.error || 'No hay conexión con el ESP32') + '. Reintentando automáticamente…'; return; }

    $('temp').textContent = d.dht_ok ? f1(d.temperatura) : '--';
    $('hum').textContent = d.dht_ok ? f1(d.humedad) : '--';
    $('dhtWarn').textContent = d.dht_ok ? '' : 'El sensor de temperatura no está respondiendo.';

    $('pV').textContent = f1(d.panel.voltaje);
    $('pEstado').textContent = d.panel.cargando ? 'Cargando la batería' : 'Sin carga';
    $('bV').textContent = f1(d.bateria.voltaje);
    const pct = Math.max(0, Math.min(100, d.bateria.porcentaje));
    $('bPct').textContent = f1(pct, 0);
    const fill = $('barFill'); fill.style.width = pct + '%';
    fill.className = pct < 20 ? 'low' : pct < 50 ? 'mid' : '';
    $('bar').setAttribute('aria-valuenow', Math.round(pct));
    $('cW').textContent = f1(d.carga.watts);
    $('cA').textContent = f1(d.carga.corriente, 2);

    const w = [];
    if (d.alertas.saturado) w.push('Sensor saturado: la corriente supera lo que puede medir.');
    if (!d.alertas.offset_ok) w.push('La calibración del sensor de corriente está fuera de rango.');
    $('acsWarn').textContent = w.join(' ');

    for (let i = 1; i <= 3; i++) {
      if (pendientes.has(i)) continue;          // esa luz tiene un cambio en curso
      luces['luz' + i] = !!d.luces['luz' + i]; pintarLuz(i, luces['luz' + i]);
    }
    $('cfgLink').href = (d.esp_url || '#') + '/config';
    $('footer').textContent = 'Activo hace ' + Math.floor(d.uptime / 60) + ' min';
  }

  async function sondear() {
    try {
      const r = await api('/api/status');
      pintar(await r.json());
    } catch (e) {
      if (e.message !== 'sesion') pintar({ online: false, error: 'No hay conexión con el servidor' });
    }
  }

  async function alternar(n) {
    const b = $('luz' + n);
    if (pendientes.has(n)) return;
    pendientes.add(n); b.classList.add('busy');
    const nuevo = !luces['luz' + n];
    pintarLuz(n, nuevo);                      // respuesta inmediata
    try {
      const r = await api('/api/light', { method: 'POST', headers: H, body: JSON.stringify({ luz: n, estado: nuevo }) });
      if (!r.ok) throw new Error();
      luces['luz' + n] = nuevo;
    } catch (e) {
      if (e.message !== 'sesion') { pintarLuz(n, !nuevo); toast('No se pudo cambiar la Luz ' + n + '.'); }
    }
    b.classList.remove('busy'); pendientes.delete(n);
  }

  document.querySelectorAll('.light').forEach(b => b.addEventListener('click', () => alternar(+b.dataset.n)));

  $('calib').addEventListener('click', async () => {
    if (!await confirmar('Recalibrar corriente', 'Apaga todas las cargas antes de continuar. La medición tarda unos segundos.')) return;
    try {
      const r = await api('/api/calibrar', { method: 'POST', headers: H });
      toast(r.ok ? 'Sensor recalibrado.' : 'No se pudo recalibrar.'); sondear();
    } catch (e) {}
  });

  $('reset').addEventListener('click', async () => {
    if (!await confirmar('Restablecer WiFi', 'El ESP32 olvidará la red guardada y se reiniciará en modo configuración. Perderás el control remoto hasta volver a configurarlo.', true)) return;
    try {
      await api('/api/resetwifi', { method: 'POST', headers: H });
      toast('Reiniciando el ESP32…');
    } catch (e) {}
  });

  $('logout').addEventListener('click', async () => {
    $('logout').disabled = true;
    try { await fetch('/logout', { method: 'POST', headers: H, cache: 'no-store' }); } catch (e) {}
    toLogin();
  });

  setInterval(sondear, 2000);
  sondear();
})();
