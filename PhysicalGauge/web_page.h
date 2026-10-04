// Web pages served by the ESP32 (see setupRoutes()).
//   DISPLAY_HTML - live CO2 pressure dial + keg / air temps, at /       (open)
//   CONFIG_HTML  - calibration, system info, links, at /config          (password)
//   WIFI_HTML    - home Wi-Fi setup, at /wifi (%OPTIONS% / %CURRENT% filled in at runtime)
// All live values come from /data (JSON).
#pragma once

const char DISPLAY_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kegerator</title>
<style>
  body { font-family: sans-serif; margin: 0; padding: 16px; background: #0b0b0c; color: #eee;
         max-width: 480px; margin: 0 auto; }
  h1 { font-size: 16px; margin: 4px 0 8px; font-weight: normal; color: #888;
       letter-spacing: 2px; text-transform: uppercase; text-align: center; }
  .dial { background: #151517; border-radius: 14px; padding: 8px 8px 14px; text-align: center; }
  #psi { font-size: 40px; font-weight: bold; margin-top: -8px; }
  #psi small { font-size: 16px; color: #888; font-weight: normal; }
  .label { color: #888; font-size: 13px; letter-spacing: 1px; text-transform: uppercase; }
  .row { display: flex; gap: 12px; margin-top: 12px; }
  .card { flex: 1; background: #151517; border-radius: 14px; padding: 14px; text-align: center; }
  .temp { font-size: 34px; font-weight: bold; margin-top: 4px; }
  .na { color: #555; font-size: 18px; font-weight: normal; }
  .foot { display: flex; justify-content: space-between; margin-top: 14px; color: #666;
          font-size: 13px; }
  a { color: #6af; }
  #offline { display: none; background: #5a1d1d; color: #fbb; padding: 8px; border-radius: 8px;
             margin-bottom: 10px; text-align: center; font-size: 14px; }
</style></head><body>
<h1>Kegerator</h1>
<div id="offline">Not receiving data from the kegerator</div>

<div class="dial">
  <svg id="gauge" viewBox="0 0 300 220" width="100%"></svg>
  <div id="psi">--<small> psi</small></div>
  <div class="label">CO2 pressure</div>
</div>

<div class="row">
  <div class="card"><div class="label">Keg</div><div class="temp" id="keg">--</div></div>
  <div class="card"><div class="label">Fridge air</div><div class="temp" id="air">--</div></div>
</div>

<div class="foot"><span id="status">connecting...</span><a href="/config">Config</a></div>

<script>
const PSI_MAX = 60, SWEEP = 240, CX = 150, CY = 150, R = 120;
const SERVE_LO = 10, SERVE_HI = 14;          // green "serving" band
const NS = 'http://www.w3.org/2000/svg';
const svg = document.getElementById('gauge');

function ang(psi) { return -SWEEP / 2 + Math.min(Math.max(psi, 0), PSI_MAX) / PSI_MAX * SWEEP; }
function pt(a, r) {
  const t = a * Math.PI / 180;
  return [CX + r * Math.sin(t), CY - r * Math.cos(t)];
}
function el(tag, attrs) {
  const e = document.createElementNS(NS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  svg.appendChild(e);
  return e;
}
function arc(p1, p2, r, attrs) {
  const a = pt(ang(p1), r), b = pt(ang(p2), r);
  const large = (ang(p2) - ang(p1)) > 180 ? 1 : 0;
  return el('path', Object.assign({ d: `M${a} A${r} ${r} 0 ${large} 1 ${b}`, fill: 'none' }, attrs));
}

arc(0, PSI_MAX, R, { stroke: '#333', 'stroke-width': 3 });
arc(SERVE_LO, SERVE_HI, R - 8, { stroke: '#2e9d4a', 'stroke-width': 10 });
for (let p = 0; p <= PSI_MAX; p += 5) {
  const major = p % 10 === 0;
  const [x1, y1] = pt(ang(p), R), [x2, y2] = pt(ang(p), R - (major ? 18 : 10));
  el('line', { x1, y1, x2, y2, stroke: '#ddd', 'stroke-width': major ? 3 : 1.5 });
  if (major) {
    const [tx, ty] = pt(ang(p), R - 34);
    el('text', { x: tx, y: ty + 5, fill: '#bbb', 'font-size': 15, 'text-anchor': 'middle' })
      .textContent = p;
  }
}
const needle = el('line', { x1: CX, y1: CY, x2: CX, y2: CY - R + 14, stroke: '#ff6a00',
                            'stroke-width': 4, 'stroke-linecap': 'round' });
el('circle', { cx: CX, cy: CY, r: 9, fill: '#ff6a00' });
needle.style.transformOrigin = CX + 'px ' + CY + 'px';
needle.style.transition = 'transform 0.6s ease-out';
needle.style.transform = `rotate(${ang(0)}deg)`;

// f: reading or null; assigned: role has a probe; found: probes on the bus
function temp(id, f, assigned, found) {
  let msg = assigned ? 'probe offline' : (found ? 'not assigned' : 'no probe');
  document.getElementById(id).innerHTML =
    f === null ? '<span class="na">' + msg + '</span>' : f.toFixed(1) + '&deg;F';
}

let lastOk = 0;
function poll() {
  fetch('/data').then(r => r.json()).then(d => {
    lastOk = Date.now();
    document.getElementById('offline').style.display = 'none';
    if (d.psi === null) {
      document.getElementById('psi').innerHTML = 'sensor fault';
    } else {
      const psi = Math.max(0, d.psi);   // never show negative here (config page shows raw)
      document.getElementById('psi').innerHTML = psi.toFixed(1) + '<small> psi</small>';
      needle.style.transform = `rotate(${ang(psi)}deg)`;
    }
    temp('keg', d.kegF, d.kegSet, d.probes.length);
    temp('air', d.airF, d.airSet, d.probes.length);
    document.getElementById('status').textContent =
      d.rssi ? 'Wi-Fi ' + d.rssi + ' dBm' : 'Wi-Fi --';
  }).catch(() => {});
}
setInterval(() => {
  if (Date.now() - lastOk > 5000) document.getElementById('offline').style.display = 'block';
}, 1000);
setInterval(poll, 1000);
poll();
</script>
</body></html>)HTML";

const char CONFIG_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kegerator Config</title>
<style>
  body { font-family: sans-serif; margin: 0 auto; padding: 16px; background: #0b0b0c; color: #eee;
         max-width: 480px; }
  h1 { font-size: 18px; margin: 0 0 12px; font-weight: normal; color: #aaa; }
  h2 { font-size: 15px; margin: 0 0 6px; }
  .big { font-size: 40px; font-weight: bold; margin: 4px 0; }
  .small { color: #999; font-size: 14px; }
  .card { background: #151517; border-radius: 10px; padding: 14px; margin: 14px 0; }
  button { font-size: 18px; padding: 12px; width: 100%; border: 0; border-radius: 8px;
           background: #2f6fd6; color: #fff; margin-top: 8px; }
  input { font-size: 18px; padding: 10px; width: 100%; box-sizing: border-box;
          border-radius: 8px; border: 1px solid #444; background: #000; color: #eee; }
  table { width: 100%; font-size: 14px; border-collapse: collapse; }
  td { padding: 3px 0; } td:first-child { color: #999; }
  a { color: #6af; }
  #msg { color: #8fd18f; min-height: 1.2em; margin-top: 8px; }
</style></head><body>
<h1>Kegerator - configuration</h1>

<div class="card">
  <h2>Pressure sensor</h2>
  <div class="big" id="psi">--</div>
  <div class="small" id="volts">sensor -- V, pin -- mV</div>
  <div class="small" id="cal">zero -- V, span -- V/psi</div>
</div>

<div class="card">
  <h2>1. Zero</h2>
  <div class="small">Sensor open to air (0 psi).</div>
  <button onclick="zero()">Set zero</button>
</div>

<div class="card">
  <h2>2. Span</h2>
  <div class="small">Apply pressure, let it settle, enter what your reference gauge reads.</div>
  <input id="known" type="number" inputmode="decimal" step="0.1" value="30">
  <button onclick="span()">Set span</button>
  <div id="msg"></div>
</div>

<div class="card">
  <h2>Temperature probes</h2>
  <div class="small">Hold one probe in your hand - the reading that rises is that probe.
    Then pick Keg or Air for it.</div>
  <div id="probes" class="small" style="margin-top:8px">searching...</div>
  <div id="pmsg" class="small" style="color:#8fd18f;margin-top:6px"></div>
</div>

<div class="card">
  <h2>System</h2>
  <table>
    <tr><td>Firmware</td><td id="build">--</td></tr>
    <tr><td>Uptime</td><td id="uptime">--</td></tr>
    <tr><td>Last restart</td><td id="reset">--</td></tr>
    <tr><td>Wi-Fi</td><td id="wifi">--</td></tr>
    <tr><td>IP address</td><td id="ip">--</td></tr>
    <tr><td>Free memory</td><td id="heap">--</td></tr>
  </table>
</div>

<div class="card">
  <a href="/">Display</a> &nbsp;|&nbsp; <a href="/wifi">Wi-Fi setup</a> &nbsp;|&nbsp;
  <a href="/update">Firmware update</a>
</div>

<script>
function dur(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + 'd ' : '') + h + 'h ' + m + 'm';
}
function set(id, t) { document.getElementById(id).textContent = t; }
function poll() {
  fetch('/data').then(r => r.json()).then(d => {
    set('psi', (d.psi === null ? '--' : d.psi.toFixed(2)) + ' psi');
    set('volts', 'sensor ' + d.v.toFixed(3) + ' V, pin ' + d.mv.toFixed(0) + ' mV');
    set('cal', 'zero ' + d.zero.toFixed(4) + ' V, span ' + d.vpp.toFixed(5) + ' V/psi');
    set('build', d.build);
    set('uptime', dur(d.uptime));
    set('reset', d.reset);
    set('wifi', d.ssid + (d.rssi ? ' (' + d.rssi + ' dBm)' : ''));
    set('ip', d.ip);
    set('heap', Math.round(d.heap / 1024) + ' KB');
    showProbes(d.probes);
  }).catch(() => {});
}
// Rebuild the probe list only when it changes, so buttons stay clickable.
let probeKey = '';
function showProbes(list) {
  const key = list.map(p => p.id + p.role).join();
  if (!list.length) { probeKey = ''; set('probes', 'No probes found - check wiring.'); return; }
  if (key !== probeKey) {
    probeKey = key;
    document.getElementById('probes').innerHTML = list.map(p => {
      const btn = (r, label) => `<button style="width:auto;padding:6px 12px;font-size:14px;` +
        `margin:4px 4px 0 0;${p.role === r ? 'background:#2e9d4a' : ''}" ` +
        `onclick="assign('${p.id}','${p.role === r ? 'none' : r}')">${label}</button>`;
      return `<div style="padding:6px 0;border-top:1px solid #333">` +
        `<span style="font-family:monospace">${p.id}</span> ` +
        `<b id="t${p.id}" style="font-size:18px;color:#eee"></b><br>` +
        btn('keg', 'Keg') + btn('air', 'Air') + `</div>`;
    }).join('');
  }
  list.forEach(p => {
    document.getElementById('t' + p.id).textContent = p.f === null ? 'no reading' : p.f.toFixed(1) + ' °F';
  });
}
function assign(id, role) {
  fetch('/probe?id=' + id + '&role=' + role, { method: 'POST' })
    .then(r => r.text()).then(t => { set('pmsg', t); poll(); });
}
function post(url) {
  fetch(url, { method: 'POST' }).then(r => r.text()).then(t => set('msg', t));
}
function zero() {
  if (confirm('Sensor open to air (0 psi)?')) post('/zero');
}
function span() {
  const p = parseFloat(document.getElementById('known').value);
  if (!(p > 0)) { set('msg', 'Enter the reference psi first'); return; }
  if (confirm('Set span at ' + p + ' psi?')) post('/span?psi=' + p);
}
setInterval(poll, 500);
poll();
</script>
</body></html>)HTML";

const char WIFI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kegerator Wi-Fi</title>
<style>
  body { font-family: sans-serif; margin: 0 auto; padding: 16px; background: #0b0b0c; color: #eee;
         max-width: 480px; }
  h1 { font-size: 18px; margin: 0 0 12px; font-weight: normal; color: #aaa; }
  .small { color: #999; font-size: 14px; }
  label { display: block; margin: 14px 0 4px; }
  input { font-size: 18px; padding: 10px; width: 100%; box-sizing: border-box;
          border-radius: 8px; border: 1px solid #444; background: #000; color: #eee; }
  button { font-size: 18px; padding: 12px; width: 100%; border: 0; border-radius: 8px;
           background: #2f6fd6; color: #fff; margin-top: 16px; }
</style></head><body>
<h1>Kegerator - home Wi-Fi</h1>
<div class="small">Saved network: %CURRENT%</div>
<form method="post" action="/wifi">
  <label for="ssid">Network (2.4 GHz)</label>
  <input id="ssid" name="ssid" list="nets" autocomplete="off" required>
  <datalist id="nets">%OPTIONS%</datalist>
  <label for="pass">Password</label>
  <input id="pass" name="pass" type="password" autocomplete="off">
  <button type="submit">Save and reboot</button>
</form>
<p class="small">The ESP32 reboots and joins this network. If it can't connect within
20 seconds it turns the "Kegerator-Setup" network back on so you can try again.</p>
<p class="small"><a style="color:#6af" href="/config">Back</a></p>
</body></html>)HTML";
