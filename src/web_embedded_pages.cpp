#include "web_embedded_pages.h"

const char kApDashboardPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 AP Dashboard</title>
  <style>
    body { margin: 0; font-family: Arial, sans-serif; background: #0f172a; color: #e2e8f0; }
    .wrap { max-width: 1100px; margin: auto; padding: 20px; }
    h1, h2 { margin: 0 0 12px 0; }
    p { opacity: .9; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 14px; }
    .card { background: #1e293b; border-radius: 18px; padding: 16px; box-shadow: 0 8px 24px rgba(0, 0, 0, .25); }
    .metric { font-size: 32px; font-weight: 700; }
    .label { font-size: 13px; opacity: .8; text-transform: uppercase; letter-spacing: .08em; }
    .hero { display: grid; grid-template-columns: 1.2fr .8fr; gap: 16px; margin-bottom: 18px; }
    .btn { background: #38bdf8; border: none; color: #062033; padding: 10px 14px; border-radius: 12px; font-weight: 700; cursor: pointer; }
    .btn.secondary { background: #334155; color: #f8fafc; }
    .btn.warn { background: #f59e0b; color: #1f1300; }
    .badge { display: inline-block; padding: 6px 10px; border-radius: 999px; background: #334155; font-size: 12px; }
    canvas { width: 100%; background: #0b1220; border-radius: 14px; }
    .small { font-size: 12px; opacity: .8; }
    .list { display: grid; gap: 8px; margin-top: 10px; }
    .item { padding: 10px; border-radius: 10px; background: #0f172a; }
    .slider-wrap { display: flex; align-items: center; gap: 12px; margin-top: 12px; }
    .slider-wrap input[type=range] { flex: 1; }
    .value-box { min-width: 52px; text-align: center; padding: 8px 10px; border-radius: 10px; background: #0f172a; }
    a.link { color: #7dd3fc; text-decoration: none; }
    @media (max-width: 900px) { .hero { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <div class="card">
        <h1>ESP32 Climate Dashboard</h1>
        <p>AP mode dashboard with temperature, humidity, TinyML prediction, Wi-Fi setup, chart history, and LED brightness control.</p>
        <div style="display:flex;gap:10px;flex-wrap:wrap">
          <span class="badge">AP SSID: <span id="apSsid">-</span></span>
          <span class="badge">AP IP: <span id="apIp">-</span></span>
          <span class="badge">Status: <span id="wifiStatus">-</span></span>
          <a class="link" href="/settings">Wi-Fi settings</a>
        </div>
      </div>

      <div class="card">
        <h2>External LED</h2>
        <div style="display:flex;gap:10px;align-items:center;flex-wrap:wrap">
          <button class="btn" onclick="toggleLight()">Toggle ON/OFF</button>
          <span class="badge">State: <span id="lightState">-</span></span>
        </div>

        <div class="slider-wrap">
          <input type="range" id="brightnessSlider" min="0" max="100" value="50" oninput="updateSliderValue(this.value)">
          <div class="value-box"><span id="sliderValue">50</span>%</div>
        </div>

        <div style="margin-top:12px">
          <button class="btn secondary" onclick="applyBrightness()">Apply Brightness</button>
        </div>

        <p class="small">Brightness only affects the LED when it is ON.</p>
      </div>
    </div>

    <div class="grid" style="margin-bottom:18px">
      <div class="card">
        <div class="label">Temperature</div>
        <div class="metric"><span id="tempValue">--</span> C</div>
      </div>
      <div class="card">
        <div class="label">Humidity</div>
        <div class="metric"><span id="humValue">--</span> %</div>
      </div>
      <div class="card">
        <div class="label">TinyML Prediction</div>
        <div class="metric"><span id="tinyLabel">--</span></div>
        <div class="small">Rain probability: <span id="tinyProb">--</span>%</div>
      </div>
      <div class="card">
        <div class="label">Wi-Fi Target</div>
        <div class="metric"><span id="staSsid">-</span></div>
        <div class="small">STA IP: <span id="staIp">-</span></div>
      </div>
    </div>

    <div class="card" style="margin-bottom:18px">
      <h2>Temperature / Humidity History</h2>
      <canvas id="chart" width="1000" height="300"></canvas>
    </div>

    <div class="grid">
      <div class="card">
        <h2>Quick Wi-Fi Scan</h2>
        <div style="display:flex;gap:10px;flex-wrap:wrap">
          <button class="btn warn" onclick="loadScan()">Scan Wi-Fi</button>
        </div>
        <div id="scanList" class="list"></div>
      </div>
      <div class="card">
        <h2>Mode Notes</h2>
        <p class="small">BOOT button forces ESP32 back to AP mode. If STA connection fails after 10 seconds, the board automatically returns to AP mode.</p>
      </div>
    </div>
  </div>

  <script>
    let sliderDirty = false;
    let lastHistoryVersion = -1;
    let stateRequestInFlight = false;
    let historyRequestInFlight = false;

    function updateSliderValue(val) {
      document.getElementById('sliderValue').textContent = val;
      sliderDirty = true;
    }

    function drawChart(temp, hum) {
      const c = document.getElementById('chart');
      const ctx = c.getContext('2d');
      const tempData = Array.isArray(temp) ? temp.map(v => Number(v)) : [];
      const humData = Array.isArray(hum) ? hum.map(v => Number(v)) : [];
      const sampleCount = Math.max(tempData.length, humData.length);
      const sampleIntervalSec = 2;

      const plot = { left: 72, right: c.width - 72, top: 26, bottom: c.height - 40 };
      const plotWidth = plot.right - plot.left;
      const plotHeight = plot.bottom - plot.top;

      const getRange = (arr, fallbackMin, fallbackMax) => {
        const valid = arr.filter(Number.isFinite);
        if (!valid.length) return { min: fallbackMin, max: fallbackMax };

        let min = Math.min(...valid);
        let max = Math.max(...valid);

        if (min === max) {
          const pad = Math.max(1, Math.abs(min) * 0.1);
          min -= pad;
          max += pad;
        } else {
          const pad = (max - min) * 0.15;
          min -= pad;
          max += pad;
        }

        return { min, max };
      };

      const tempRange = getRange(tempData, 20, 40);
      const humRange = getRange(humData, 30, 90);

      const getX = index => sampleCount <= 1
        ? plot.left + plotWidth / 2
        : plot.left + (index * plotWidth) / (sampleCount - 1);

      const getY = (value, range) =>
        plot.bottom - ((value - range.min) / (range.max - range.min)) * plotHeight;

      const drawSeries = (arr, color, range) => {
        let hasPoint = false;
        let started = false;

        ctx.strokeStyle = color;
        ctx.fillStyle = color;
        ctx.lineWidth = 3;
        ctx.beginPath();

        for (let i = 0; i < sampleCount; i++) {
          const value = arr[i];
          if (!Number.isFinite(value)) {
            started = false;
            continue;
          }

          const x = getX(i);
          const y = getY(value, range);
          if (!started) {
            ctx.moveTo(x, y);
            started = true;
          } else {
            ctx.lineTo(x, y);
          }
          hasPoint = true;
        }

        if (hasPoint) ctx.stroke();

        for (let i = 0; i < sampleCount; i++) {
          const value = arr[i];
          if (!Number.isFinite(value)) continue;
          const x = getX(i);
          const y = getY(value, range);
          ctx.beginPath();
          ctx.arc(x, y, 3, 0, Math.PI * 2);
          ctx.fill();
        }
      };

      ctx.clearRect(0, 0, c.width, c.height);
      ctx.fillStyle = '#0b1220';
      ctx.fillRect(0, 0, c.width, c.height);

      ctx.strokeStyle = '#1e293b';
      ctx.lineWidth = 1;
      ctx.font = '12px Arial';

      for (let i = 0; i <= 4; i++) {
        const y = plot.top + (plotHeight * i) / 4;
        ctx.beginPath();
        ctx.moveTo(plot.left, y);
        ctx.lineTo(plot.right, y);
        ctx.stroke();

        const tempVal = tempRange.max - ((tempRange.max - tempRange.min) * i) / 4;
        const humVal = humRange.max - ((humRange.max - humRange.min) * i) / 4;

        ctx.fillStyle = '#f87171';
        ctx.fillText(tempVal.toFixed(1) + 'C', 8, y + 4);
        ctx.fillStyle = '#4ade80';
        ctx.fillText(humVal.toFixed(0) + '%', c.width - 54, y + 4);
      }

      const labelCount = sampleCount > 1 ? Math.min(sampleCount, 5) : sampleCount;
      ctx.fillStyle = '#94a3b8';
      ctx.textAlign = 'center';

      for (let i = 0; i < labelCount; i++) {
        const index = labelCount === 1 ? 0 : Math.round((i * (sampleCount - 1)) / (labelCount - 1));
        const x = getX(index);
        const secondsAgo = (sampleCount - 1 - index) * sampleIntervalSec;
        const label = secondsAgo === 0 ? 'now' : `${secondsAgo}s`;
        ctx.fillText(label, x, c.height - 12);
      }

      ctx.textAlign = 'left';
      ctx.fillStyle = '#f87171';
      ctx.fillText('Temperature', plot.left, 18);
      ctx.fillStyle = '#4ade80';
      ctx.fillText('Humidity', plot.left + 120, 18);

      drawSeries(tempData, '#f87171', tempRange);
      drawSeries(humData, '#4ade80', humRange);
    }

    async function loadState() {
      if (stateRequestInFlight) return;
      stateRequestInFlight = true;
      try {
        const res = await fetch('/api/state', { cache: 'no-store' });
        if (!res.ok) return;
        const s = await res.json();

        document.getElementById('tempValue').textContent = s.temperatureText;
        document.getElementById('humValue').textContent = s.humidityText;
        document.getElementById('tinyLabel').textContent = s.tinyLabel;
        document.getElementById('tinyProb').textContent = s.tinyProbability;
        document.getElementById('wifiStatus').textContent = s.wifiStatus;
        document.getElementById('apSsid').textContent = s.apSsid || '-';
        document.getElementById('apIp').textContent = s.apIp;
        document.getElementById('staIp').textContent = s.staIp;
        document.getElementById('staSsid').textContent = s.staSsid || '-';
        document.getElementById('lightState').textContent = s.userLightOn ? 'ON' : 'OFF';

        const slider = document.getElementById('brightnessSlider');
        if (!sliderDirty && document.activeElement !== slider) {
          slider.value = s.userLightBrightness;
          document.getElementById('sliderValue').textContent = s.userLightBrightness;
        }

        if (typeof s.historyVersion === 'number' && s.historyVersion !== lastHistoryVersion) {
          await loadHistory();
        }
      } catch (_) {
        // Browsers may briefly lose the AP/STA route while ESP32 changes mode.
      } finally {
        stateRequestInFlight = false;
      }
    }

    async function loadHistory() {
      if (historyRequestInFlight) return;
      historyRequestInFlight = true;
      try {
        const res = await fetch('/api/history', { cache: 'no-store' });
        if (!res.ok) return;
        const h = await res.json();
        if (typeof h.historyVersion === 'number') {
          lastHistoryVersion = h.historyVersion;
        }
        drawChart(h.tempHistory, h.humHistory);
      } catch (_) {
        // Ignore transient fetch errors while the Wi-Fi interface is switching.
      } finally {
        historyRequestInFlight = false;
      }
    }

    async function toggleLight() {
      await fetch('/api/light/toggle', { method: 'POST' });
      await loadState();
    }

    async function applyBrightness() {
      const val = document.getElementById('brightnessSlider').value;
      await fetch('/api/light/brightness', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'value=' + encodeURIComponent(val)
      });
      sliderDirty = false;
      await loadState();
    }

    async function loadScan() {
      const res = await fetch('/api/scan', { cache: 'no-store' });
      if (!res.ok) return;
      const arr = await res.json();
      const box = document.getElementById('scanList');
      box.innerHTML = '';
      if (!arr.length) {
        box.innerHTML = '<div class="item">Scanning in background or no networks found yet.</div>';
        return;
      }
      arr.forEach(w => {
        const div = document.createElement('div');
        div.className = 'item';

        const title = document.createElement('div');
        title.textContent = `${w.ssid || '(hidden)'} | ${w.quality}% | ${w.enc}`;

        const actions = document.createElement('div');
        actions.style.marginTop = '8px';

        const button = document.createElement('button');
        button.className = 'btn secondary';
        button.type = 'button';
        button.textContent = 'Use this Wi-Fi';
        button.disabled = !w.ssid;
        button.addEventListener('click', () => {
          if (!w.ssid) return;
          window.location.href = '/settings?ssid=' + encodeURIComponent(w.ssid);
        });

        actions.appendChild(button);
        div.appendChild(title);
        div.appendChild(actions);
        box.appendChild(div);
      });
    }

    loadState();
    setInterval(loadState, 1500);
  </script>
</body>
</html>
)rawliteral";

const char kStaDashboardPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 STA CoreIOT</title>
  <style>
    body { margin: 0; background: #0f172a; color: #e2e8f0; font-family: Arial, sans-serif; }
    .wrap { max-width: 560px; margin: auto; padding: 16px; }
    .card { background: #1e293b; border-radius: 12px; padding: 14px; margin-bottom: 12px; }
    .row { display: flex; justify-content: space-between; gap: 10px; padding: 6px 0; border-bottom: 1px solid #334155; }
    .row:last-child { border-bottom: none; }
    .label { opacity: .8; }
    .value { font-weight: 700; }
    .btn { background: #38bdf8; border: none; color: #062033; padding: 10px 12px; border-radius: 10px; font-weight: 700; cursor: pointer; width: 100%; }
    input { width: 100%; padding: 10px; border-radius: 10px; border: 1px solid #475569; background: #0f172a; color: #fff; box-sizing: border-box; }
    .toggle { display: flex; align-items: center; gap: 8px; margin-bottom: 10px; }
    .small { font-size: 12px; opacity: .8; }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h2 style="margin:0 0 8px 0">STA Network</h2>
      <div class="row"><span class="label">WiFi</span><span class="value" id="wifiStatus">-</span></div>
      <div class="row"><span class="label">SSID</span><span class="value" id="staSsid">-</span></div>
      <div class="row"><span class="label">IP</span><span class="value" id="staIp">-</span></div>
    </div>

    <div class="card">
      <h2 style="margin:0 0 8px 0">CoreIOT</h2>
      <div class="row"><span class="label">MQTT</span><span class="value" id="coreiotStatus">-</span></div>
      <div class="row"><span class="label">Retry</span><span class="value" id="coreiotRetrySec">0s</span></div>
    </div>

    <div class="card">
      <h2 style="margin:0 0 10px 0">CoreIOT Config</h2>
      <div class="toggle">
        <input type="checkbox" id="coreiotEnabled" style="width:auto">
        <span>Enable publish to CoreIOT</span>
      </div>
      <div style="margin-bottom:10px">
        <input id="coreiotHost" placeholder="MQTT broker host (e.g. app.coreiot.io or 192.168.1.10)">
      </div>
      <div style="margin-bottom:10px">
        <input id="coreiotUsername" placeholder="MQTT username">
      </div>
      <div style="margin-bottom:10px">
        <input id="coreiotPassword" placeholder="MQTT password">
      </div>
      <button class="btn" onclick="saveCoreiotConfig()">Save</button>
      <div class="small" id="coreiotMsg" style="margin-top:8px"></div>
    </div>
  </div>

  <script>
    let stateRequestInFlight = false;

    async function loadState() {
      if (stateRequestInFlight) return;
      stateRequestInFlight = true;
      try {
        const res = await fetch('/api/state', { cache: 'no-store' });
        if (!res.ok) return;
        const s = await res.json();
        document.getElementById('wifiStatus').textContent = s.wifiStatus || '-';
        document.getElementById('staSsid').textContent = s.staSsid || '-';
        document.getElementById('staIp').textContent = s.staIp || '-';
        document.getElementById('coreiotStatus').textContent = s.coreiotMqtt ? 'CONNECTED' : 'DISCONNECTED';
        document.getElementById('coreiotRetrySec').textContent = String(s.coreiotRetrySec || 0) + 's';
      } catch (_) {
        // Ignore transient loss while the client changes between AP and STA paths.
      } finally {
        stateRequestInFlight = false;
      }
    }

    async function loadCoreiotConfig() {
      const res = await fetch('/api/coreiot/config', { cache: 'no-store' });
      if (!res.ok) return;
      const cfg = await res.json();
      document.getElementById('coreiotEnabled').checked = !!cfg.enabled;
      document.getElementById('coreiotHost').value = cfg.host || '';
      document.getElementById('coreiotUsername').value = cfg.username || '';
      document.getElementById('coreiotPassword').value = cfg.password || '';
    }

    async function saveCoreiotConfig() {
      const enabled = document.getElementById('coreiotEnabled').checked;
      const host = document.getElementById('coreiotHost').value || '';
      const username = document.getElementById('coreiotUsername').value || '';
      const password = document.getElementById('coreiotPassword').value || '';
      const body = 'enabled=' + encodeURIComponent(enabled ? '1' : '0') +
                   '&host=' + encodeURIComponent(host) +
                   '&username=' + encodeURIComponent(username) +
                   '&password=' + encodeURIComponent(password);

      const res = await fetch('/api/coreiot/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body
      });

      const msg = document.getElementById('coreiotMsg');
      msg.textContent = res.ok ? 'Saved' : 'Save failed';
      await loadState();
    }

    loadState();
    loadCoreiotConfig();
    setInterval(loadState, 3500);
  </script>
</body>
</html>
)rawliteral";

const char kSettingsPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Wi-Fi Settings</title>
  <style>
    body { margin: 0; font-family: Arial, sans-serif; background: #0f172a; color: #e2e8f0; }
    .wrap { max-width: 960px; margin: auto; padding: 20px; }
    .card { background: #1e293b; border-radius: 18px; padding: 16px; margin-bottom: 16px; }
    .row { display: flex; gap: 10px; flex-wrap: wrap; }
    .btn { background: #38bdf8; border: none; color: #062033; padding: 10px 14px; border-radius: 12px; font-weight: 700; cursor: pointer; }
    .btn.secondary { background: #334155; color: #f8fafc; }
    input { width: 100%; padding: 10px; border-radius: 10px; border: 1px solid #475569; background: #0f172a; color: #fff; box-sizing: border-box; }
    .list { display: grid; gap: 8px; margin-top: 10px; }
    .item { padding: 10px; border-radius: 10px; background: #0f172a; }
    a { color: #7dd3fc; text-decoration: none; }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>Wi-Fi Settings</h1>
      <a href="/">Back to dashboard</a>
    </div>

    <div class="card">
      <h2>Available Wi-Fi Networks</h2>
      <div class="row">
        <button class="btn" onclick="loadScan()">Scan Wi-Fi</button>
      </div>
      <div id="scanList" class="list"></div>
    </div>

    <div class="card">
      <h2>Saved Wi-Fi Profiles</h2>
      <div class="row">
        <button class="btn secondary" onclick="loadSaved()">Refresh Saved</button>
        <button class="btn secondary" onclick="forgetAll()">Forget All</button>
      </div>
      <div id="savedList" class="list"></div>
    </div>

    <div class="card">
      <h2>Connect to Wi-Fi</h2>
      <form method="POST" action="/connect">
        <div class="row">
          <input id="ssid" name="ssid" placeholder="SSID">
          <input id="password" name="password" placeholder="Password">
        </div>
        <div class="row" style="margin-top:12px">
          <button class="btn" type="submit" name="save" value="1">Connect & Save</button>
          <button class="btn secondary" type="submit" name="save" value="0">Connect</button>
        </div>
      </form>
    </div>
  </div>

  <script>
    function fillWifi(ssid, pass) {
      document.getElementById('ssid').value = ssid || '';
      document.getElementById('password').value = pass || '';
      document.getElementById('password').focus();
    }

    function applySelectedSsidFromQuery() {
      const params = new URLSearchParams(window.location.search);
      const selectedSsid = params.get('ssid');
      if (!selectedSsid) return;
      fillWifi(selectedSsid, '');
    }

    async function loadScan() {
      const res = await fetch('/api/scan', { cache: 'no-store' });
      if (!res.ok) return;
      const arr = await res.json();
      const box = document.getElementById('scanList');
      box.innerHTML = '';
      if (!arr.length) {
        box.innerHTML = '<div class="item">Scanning in background or no scan results available yet.</div>';
        return;
      }
      arr.forEach(w => {
        const div = document.createElement('div');
        div.className = 'item';

        const title = document.createElement('b');
        title.textContent = `${w.ssid || '(hidden)'} | ${w.quality}% | ${w.enc}`;

        const actions = document.createElement('div');
        actions.style.marginTop = '8px';

        const button = document.createElement('button');
        button.className = 'btn secondary';
        button.type = 'button';
        button.textContent = 'Use scanned SSID';
        button.disabled = !w.ssid;
        button.addEventListener('click', () => fillWifi(w.ssid || '', ''));

        actions.appendChild(button);
        div.appendChild(title);
        div.appendChild(actions);
        box.appendChild(div);
      });
    }

    async function loadSaved() {
      const res = await fetch('/api/saved', { cache: 'no-store' });
      if (!res.ok) return;
      const arr = await res.json();
      const box = document.getElementById('savedList');
      box.innerHTML = '';
      if (!arr.length) {
        box.innerHTML = '<div class="item">No saved profiles found.</div>';
        return;
      }
      arr.forEach(w => {
        const div = document.createElement('div');
        div.className = 'item';

        const title = document.createElement('b');
        title.textContent = w.ssid || '(hidden)';

        const actions = document.createElement('div');
        actions.style.marginTop = '8px';

        const button = document.createElement('button');
        button.className = 'btn secondary';
        button.type = 'button';
        button.textContent = 'Use saved profile';
        button.disabled = !w.ssid;
        button.addEventListener('click', () => fillWifi(w.ssid || '', w.pass || ''));

        actions.appendChild(button);
        div.appendChild(title);
        div.appendChild(actions);
        box.appendChild(div);
      });
    }

    async function forgetAll() {
      await fetch('/forget', { method: 'POST' });
      loadSaved();
    }

    loadSaved();
    applySelectedSsidFromQuery();
  </script>
</body>
</html>
)rawliteral";

const char kConnectStatusPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Connecting</title>
  <style>
    body { margin: 0; font-family: Arial, sans-serif; background: #0f172a; color: #e2e8f0; }
    .wrap { max-width: 760px; margin: auto; padding: 24px; }
    .card { background: #1e293b; border-radius: 18px; padding: 18px; }
    a { color: #7dd3fc; text-decoration: none; }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>Connecting to Wi-Fi</h1>
      <p>ESP32 is trying to connect to SSID: <b>{{SSID}}</b></p>
      <p>If no link is established within 10 seconds, the board automatically returns to AP mode.</p>
      <p><a href="/">Back</a></p>
    </div>
  </div>
</body>
</html>
)rawliteral";
