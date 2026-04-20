#include "mainserver.h"
#include <math.h>

namespace {
  WebServer g_server(80);
  Preferences g_prefs;

  app_context_t *g_ctx = nullptr;
  sensor_data_t g_latestSensor = {NAN, NAN, 0};

  bool g_isApMode = false;
  bool g_isStaConnecting = false;
  bool g_serverStarted = false;
  bool g_lastConnectFailed = false;

  unsigned long g_staConnectStartMs = 0;
  String g_staSsid;
  String g_staPassword;
  String g_staIp;

  static const int HISTORY_SIZE = 20;
  float g_tempHistory[HISTORY_SIZE];
  float g_humHistory[HISTORY_SIZE];
  int g_historyCount = 0;
  int g_historyHead = 0;

  struct SavedWifi {
    String ssid;
    String pass;
  };

  String jsonEscape(const String &s) {
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
      char c = s[i];
      switch (c) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
      }
    }
    return out;
  }

  String wifiStatusText() {
    if (g_isApMode) return "AP MODE";
    if (g_isStaConnecting) return "STA CONNECTING";
    if (WiFi.status() == WL_CONNECTED) return "STA CONNECTED";
    return "DISCONNECTED";
  }

  String apIpText() {
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
      return WiFi.softAPIP().toString();
    }
    return "-";
  }

  String staIpText() {
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
    return "-";
  }

  int wifiQualityPercent(int32_t rssi) {
    if (rssi <= -100) return 0;
    if (rssi >= -50) return 100;
    return 2 * (rssi + 100);
  }

  String classifyWeatherText(float t, float h) {
    if (isnan(t) || isnan(h)) return "NO DATA";
    if (t >= 35.0f || h >= 85.0f) return "CRITICAL";
    if ((t >= 30.0f && t < 35.0f) || h >= 70.0f) return "WARNING";
    if (t >= 20.0f && t < 30.0f && h >= 40.0f && h < 70.0f) return "NORMAL";
    if (t < 20.0f) return "COOL";
    return "NORMAL";
  }

  int getSavedCount() {
    g_prefs.begin(WIFI_STORE_NAMESPACE, true);
    int count = g_prefs.getInt("count", 0);
    g_prefs.end();
    if (count < 0) count = 0;
    if (count > WIFI_MAX_SAVED) count = WIFI_MAX_SAVED;
    return count;
  }

  SavedWifi readSavedAt(int index) {
    SavedWifi item;
    g_prefs.begin(WIFI_STORE_NAMESPACE, true);
    item.ssid = g_prefs.getString(("ssid" + String(index)).c_str(), "");
    item.pass = g_prefs.getString(("pass" + String(index)).c_str(), "");
    g_prefs.end();
    return item;
  }

  void writeSavedAt(int index, const SavedWifi &item) {
    g_prefs.begin(WIFI_STORE_NAMESPACE, false);
    g_prefs.putString(("ssid" + String(index)).c_str(), item.ssid);
    g_prefs.putString(("pass" + String(index)).c_str(), item.pass);
    g_prefs.end();
  }

  void setSavedCount(int count) {
    g_prefs.begin(WIFI_STORE_NAMESPACE, false);
    g_prefs.putInt("count", count);
    g_prefs.end();
  }

  void clearSavedWifi() {
    g_prefs.begin(WIFI_STORE_NAMESPACE, false);
    g_prefs.clear();
    g_prefs.end();
  }

  bool loadMostRecentWifi(String &ssid, String &password) {
    int count = getSavedCount();
    if (count <= 0) return false;

    SavedWifi item = readSavedAt(0);
    ssid = item.ssid;
    password = item.pass;
    return !ssid.isEmpty();
  }

  void saveWifiRecent(const String &ssid, const String &password) {
    if (ssid.isEmpty()) return;

    SavedWifi list[WIFI_MAX_SAVED];
    int count = getSavedCount();

    for (int i = 0; i < count; ++i) {
      list[i] = readSavedAt(i);
    }

    SavedWifi newList[WIFI_MAX_SAVED];
    int newCount = 0;

    newList[newCount++] = {ssid, password};

    for (int i = 0; i < count && newCount < WIFI_MAX_SAVED; ++i) {
      if (list[i].ssid != ssid && !list[i].ssid.isEmpty()) {
        newList[newCount++] = list[i];
      }
    }

    for (int i = 0; i < newCount; ++i) {
      writeSavedAt(i, newList[i]);
    }

    g_prefs.begin(WIFI_STORE_NAMESPACE, false);
    for (int i = newCount; i < WIFI_MAX_SAVED; ++i) {
      g_prefs.remove(("ssid" + String(i)).c_str());
      g_prefs.remove(("pass" + String(i)).c_str());
    }
    g_prefs.end();

    setSavedCount(newCount);
  }

  String savedWifiJson() {
    int count = getSavedCount();
    String json = "[";
    for (int i = 0; i < count; ++i) {
      SavedWifi item = readSavedAt(i);
      if (i > 0) json += ",";
      json += "{";
      json += "\"ssid\":\"" + jsonEscape(item.ssid) + "\",";
      json += "\"pass\":\"" + jsonEscape(item.pass) + "\",";
      json += "\"hasPassword\":" + String(item.pass.isEmpty() ? "false" : "true");
      json += "}";
    }
    json += "]";
    return json;
  }

  String wifiScanJson() {
    int n = WiFi.scanNetworks(false, true);
    String json = "[";

    for (int i = 0; i < n; ++i) {
      if (i > 0) json += ",";

      String enc;
      wifi_auth_mode_t auth = WiFi.encryptionType(i);
      switch (auth) {
        case WIFI_AUTH_OPEN: enc = "OPEN"; break;
        case WIFI_AUTH_WEP: enc = "WEP"; break;
        case WIFI_AUTH_WPA_PSK: enc = "WPA"; break;
        case WIFI_AUTH_WPA2_PSK: enc = "WPA2"; break;
        case WIFI_AUTH_WPA_WPA2_PSK: enc = "WPA/WPA2"; break;
        case WIFI_AUTH_WPA2_ENTERPRISE: enc = "WPA2-ENT"; break;
        case WIFI_AUTH_WPA3_PSK: enc = "WPA3"; break;
        case WIFI_AUTH_WPA2_WPA3_PSK: enc = "WPA2/WPA3"; break;
        default: enc = "UNKNOWN"; break;
      }

      json += "{";
      json += "\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
      json += "\"enc\":\"" + enc + "\"";
      json += "}";
    }

    json += "]";
    WiFi.scanDelete();
    return json;
  }

  void pushHistory(float t, float h) {
    g_tempHistory[g_historyHead] = t;
    g_humHistory[g_historyHead] = h;
    g_historyHead = (g_historyHead + 1) % HISTORY_SIZE;
    if (g_historyCount < HISTORY_SIZE) g_historyCount++;
  }

  String apPage() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 AP Setup</title>
  <style>
    body{margin:0;font-family:Arial,sans-serif;background:#f3f6fb;color:#1e293b}
    .wrap{max-width:720px;margin:30px auto;padding:18px}
    .card{background:#fff;border-radius:16px;padding:20px;box-shadow:0 6px 20px rgba(0,0,0,.08)}
    h1{margin-top:0;font-size:26px}
    .small{color:#64748b;line-height:1.6}
    .btn{display:inline-block;margin-top:14px;padding:11px 14px;background:#2563eb;color:#fff;text-decoration:none;border-radius:10px;font-weight:700}
    .info{margin-top:14px;padding:12px;background:#eff6ff;border-radius:10px}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>ESP32 Wi-Fi Setup</h1>
      <div class="small">This mode is used to configure your Wi-Fi credentials before switching to STA mode.</div>

      <div class="info">
        <div><b>AP SSID:</b> )rawliteral";
    html += AP_SSID;
    html += R"rawliteral(</div>
        <div><b>AP IP:</b> )rawliteral";
    html += apIpText();
    html += R"rawliteral(</div>
      </div>

      <a class="btn" href="/settings">Open Wi-Fi Settings</a>
    </div>
  </div>
</body>
</html>
)rawliteral";
    return html;
  }

  String staPage() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 Dashboard</title>
  <style>
    :root{
      --bg:#eef3f9;
      --card:#ffffff;
      --text:#0f172a;
      --muted:#64748b;
      --line:#dbe4ee;
      --accent:#2563eb;
      --accent2:#0ea5e9;
      --ok:#16a34a;
    }
    *{box-sizing:border-box}
    body{margin:0;font-family:Arial,sans-serif;background:linear-gradient(180deg,#f7fbff 0%,var(--bg) 100%);color:var(--text)}
    .wrap{max-width:1000px;margin:0 auto;padding:16px}
    .top,.card{background:var(--card);border-radius:18px;box-shadow:0 6px 18px rgba(15,23,42,.07)}
    .top{padding:18px 18px 16px 18px;margin-bottom:16px}
    .topbar{display:flex;justify-content:space-between;align-items:flex-start;gap:14px;flex-wrap:wrap}
    .headline{margin:0 0 6px 0;font-size:28px}
    .muted{color:var(--muted);font-size:14px;line-height:1.6}
    .actions{margin-top:12px;display:flex;gap:10px;flex-wrap:wrap}
    .btn{display:inline-block;padding:10px 14px;background:var(--accent);color:#fff;text-decoration:none;border-radius:10px;font-weight:700}
    .btn.secondary{background:#475569}
    .grid2{display:grid;grid-template-columns:1fr 1fr;gap:16px}
    .grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}
    .card{padding:18px}
    .metricTitle{color:var(--muted);font-size:14px}
    .value{font-size:38px;font-weight:800;margin-top:8px;letter-spacing:-.02em}
    .unit{font-size:18px;font-weight:700;color:var(--muted)}
    .statusPill{display:inline-block;padding:8px 12px;border-radius:999px;background:#ecfdf5;color:#166534;font-weight:700;font-size:13px}
    .mini{padding:12px;background:#f8fbff;border:1px solid #e6edf5;border-radius:12px;min-height:68px}
    .mini b{display:block;margin-bottom:6px;font-size:13px;color:var(--muted)}
    .mini span{font-size:17px;font-weight:700;color:var(--text)}
    .chartWrap{margin-top:16px}
    .chartCardTitle{margin-bottom:10px;font-size:15px;font-weight:700}
    .chartSub{margin-top:-2px;margin-bottom:10px;color:var(--muted);font-size:13px}
    canvas{display:block;width:100%;height:220px;background:#fff;border-radius:12px;border:1px solid var(--line)}
    .footerNote{margin-top:10px;color:var(--muted);font-size:12px}
    @media(max-width:860px){.grid2,.grid3{grid-template-columns:1fr}}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="top">
      <div class="topbar">
        <div>
          <h1 class="headline">ESP32 Climate Dashboard</h1>
          <div class="muted">Station mode dashboard for temperature and humidity monitoring with lightweight real-time charts.</div>
          <div class="actions">
            <a class="btn" href="/settings">Wi-Fi Settings</a>
            <a class="btn secondary" href="/forget">Forget saved Wi-Fi</a>
          </div>
        </div>
        <div>
          <div class="statusPill" id="modeText">-</div>
        </div>
      </div>
    </div>

    <div class="grid2">
      <div class="card">
        <div class="metricTitle">🌡️ Temperature</div>
        <div class="value"><span id="tempText">--</span> <span class="unit">°C</span></div>
      </div>
      <div class="card">
        <div class="metricTitle">💧 Humidity</div>
        <div class="value"><span id="humText">--</span> <span class="unit">%</span></div>
      </div>
    </div>

    <div class="card" style="margin-top:16px">
      <div class="grid3">
        <div class="mini"><b>System status</b><span id="weatherText">-</span></div>
        <div class="mini"><b>Current time</b><span id="timeText">--:--:--</span></div>
        <div class="mini"><b>STA IP</b><span id="staIpText">-</span></div>
        <div class="mini"><b>SSID</b><span id="ssidText">-</span></div>
        <div class="mini"><b>Latest sample</b><span>20 points</span></div>
        <div class="mini"><b>XAxis unit</b><span>Recent samples</span></div>
      </div>
    </div>

    <div class="grid2 chartWrap">
      <div class="card">
        <div class="chartCardTitle">Temperature chart</div>
        <div class="chartSub">Y-axis: °C &nbsp; | &nbsp; X-axis: 20 most recent samples</div>
        <canvas id="tempChart" width="460" height="220"></canvas>
        <div class="footerNote">Shows up to the latest 20 temperature readings.</div>
      </div>
      <div class="card">
        <div class="chartCardTitle">Humidity chart</div>
        <div class="chartSub">Y-axis: %RH &nbsp; | &nbsp; X-axis: 20 most recent samples</div>
        <canvas id="humChart" width="460" height="220"></canvas>
        <div class="footerNote">Shows up to the latest 20 humidity readings.</div>
      </div>
    </div>
  </div>

<script>
const tempCanvas = document.getElementById('tempChart');
const humCanvas  = document.getElementById('humChart');

function pad2(n){ return String(n).padStart(2,'0'); }
function updateClock(){
  const now = new Date();
  const txt = pad2(now.getHours()) + ':' + pad2(now.getMinutes()) + ':' + pad2(now.getSeconds());
  document.getElementById('timeText').innerText = txt;
}

function formatValue(v, digits=1){
  return (v === null || v === undefined) ? '--' : Number(v).toFixed(digits);
}

function niceStep(range){
  if (range <= 5) return 1;
  if (range <= 10) return 2;
  if (range <= 20) return 5;
  if (range <= 50) return 10;
  return 20;
}

function drawAxisChart(canvas, values, options){
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  const left = 44;
  const right = 12;
  const top = 12;
  const bottom = 30;
  const cw = w - left - right;
  const ch = h - top - bottom;

  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0,0,w,h);

  const clean = values.filter(v => v !== null && !Number.isNaN(v));
  let minV = options.defaultMin;
  let maxV = options.defaultMax;

  if (clean.length) {
    minV = Math.min(...clean);
    maxV = Math.max(...clean);
    if (minV === maxV) {
      minV -= options.flatPadding;
      maxV += options.flatPadding;
    } else {
      minV -= options.pad;
      maxV += options.pad;
    }
  }

  if (options.clampMin !== null) minV = Math.min(minV, options.clampMin);
  if (options.clampMax !== null) maxV = Math.max(maxV, options.clampMax);

  const stepY = niceStep(maxV - minV);
  const axisMin = Math.floor(minV / stepY) * stepY;
  const axisMax = Math.ceil(maxV / stepY) * stepY;
  const axisRange = Math.max(1, axisMax - axisMin);

  ctx.strokeStyle = '#dbe4ee';
  ctx.lineWidth = 1;
  ctx.fillStyle = '#64748b';
  ctx.font = '11px Arial';

  for (let i = 0; i <= 4; i++) {
    const y = top + (ch * i / 4);
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(w - right, y);
    ctx.stroke();

    const labelValue = axisMax - (axisRange * i / 4);
    ctx.fillText(labelValue.toFixed(options.yDigits), 4, y + 4);
  }

  ctx.beginPath();
  ctx.moveTo(left, top);
  ctx.lineTo(left, h - bottom);
  ctx.lineTo(w - right, h - bottom);
  ctx.strokeStyle = '#94a3b8';
  ctx.stroke();

  const count = values.length;
  const stepX = count > 1 ? cw / (count - 1) : cw;
  const xLabels = [0, 4, 9, 14, 19];
  ctx.fillStyle = '#64748b';
  xLabels.forEach(i => {
    if (i >= count) return;
    const x = left + i * stepX;
    ctx.fillText(String(i + 1), x - 4, h - 10);
  });

  function mapY(v){
    return top + ((axisMax - v) / axisRange) * ch;
  }

  ctx.beginPath();
  let started = false;
  values.forEach((v, i) => {
    if (v === null || Number.isNaN(v)) return;
    const x = left + i * stepX;
    const y = mapY(v);
    if (!started) {
      ctx.moveTo(x, y);
      started = true;
    } else {
      ctx.lineTo(x, y);
    }
  });
  ctx.strokeStyle = options.lineColor;
  ctx.lineWidth = 2;
  ctx.stroke();

  ctx.fillStyle = options.lineColor;
  values.forEach((v, i) => {
    if (v === null || Number.isNaN(v)) return;
    const x = left + i * stepX;
    const y = mapY(v);
    ctx.beginPath();
    ctx.arc(x, y, 2.5, 0, Math.PI * 2);
    ctx.fill();
  });
}

async function refreshDashboard(){
  try {
    const r = await fetch('/api/state');
    const d = await r.json();

    document.getElementById('tempText').innerText = formatValue(d.temp, 1);
    document.getElementById('humText').innerText = formatValue(d.hum, 1);
    document.getElementById('weatherText').innerText = d.weather || '-';
    document.getElementById('modeText').innerText = d.mode || '-';
    document.getElementById('staIpText').innerText = d.staIp || '-';
    document.getElementById('ssidText').innerText = d.ssid || '-';

    drawAxisChart(tempCanvas, d.historyTemp || [], {
      lineColor: '#ef4444',
      defaultMin: 20,
      defaultMax: 40,
      pad: 1,
      flatPadding: 1,
      clampMin: null,
      clampMax: null,
      yDigits: 0
    });

    drawAxisChart(humCanvas, d.historyHum || [], {
      lineColor: '#0ea5e9',
      defaultMin: 0,
      defaultMax: 100,
      pad: 3,
      flatPadding: 2,
      clampMin: 0,
      clampMax: 100,
      yDigits: 0
    });
  } catch (e) {
    console.log(e);
  }
}

updateClock();
refreshDashboard();
setInterval(updateClock, 1000);
setInterval(refreshDashboard, 2500);
</script>
</body>
</html>
)rawliteral";
    return html;
  }

  String settingsPage() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Wi-Fi Settings</title>
  <style>
    body { margin: 0; font-family: Arial, sans-serif; background: #f4f7fb; color: #1f2937; }
    .wrap { max-width: 900px; margin: 0 auto; padding: 20px; }
    .card { background: white; border-radius: 16px; padding: 18px; box-shadow: 0 4px 16px rgba(0,0,0,0.07); margin-bottom: 16px; }
    input { width: 100%; padding: 12px; margin: 8px 0 14px 0; box-sizing: border-box; border-radius: 10px; border: 1px solid #cbd5e1; font-size: 15px; }
    button { border: none; border-radius: 10px; padding: 10px 14px; background: #2563eb; color: white; cursor: pointer; font-weight: 600; margin-right: 10px; margin-top: 6px; }
    button.secondary { background: #475569; }
    .wifiItem { padding: 12px; border: 1px solid #e2e8f0; border-radius: 10px; margin-bottom: 10px; display: flex; justify-content: space-between; align-items: center; gap: 12px; flex-wrap: wrap; }
    .small { color: #64748b; font-size: 14px; line-height: 1.6; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
    @media (max-width: 760px) { .grid { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>Wi-Fi Settings</h1>
      <p class="small">Scan nearby networks, review saved credentials, and configure the connection for STA mode.</p>
      <a href="/"><button class="secondary">Back</button></a>
    </div>

    <div class="grid">
      <div class="card">
        <h2>Available Wi-Fi Networks</h2>
        <p class="small">Use the scan function to detect nearby wireless networks.</p>
        <button onclick="scanWifi()">Scan Wi-Fi</button>
        <div id="wifiList" class="small" style="margin-top:12px;">No scan results available.</div>
      </div>

      <div class="card">
        <h2>Saved Wi-Fi Profiles</h2>
        <p class="small">Previously saved credentials can be reused for faster configuration.</p>
        <button onclick="loadSaved()">Refresh Saved</button>
        <div id="savedList" class="small" style="margin-top:12px;">No saved profiles found.</div>
      </div>
    </div>

    <div class="card">
      <h2>Connect to Wi-Fi</h2>
      <p class="small">Enter the network credentials below and submit to establish a connection.</p>
      <form id="wifiForm">
        <label>SSID</label>
        <input id="ssid" type="text" placeholder="Enter Wi-Fi SSID" required />

        <label>Password</label>
        <input id="pass" type="password" placeholder="Enter Wi-Fi password" />

        <button type="submit">Connect & Save</button>
      </form>
      <p class="small">Selecting a saved profile will automatically fill in both the SSID and password fields.</p>
    </div>
  </div>

<script>
  function scanWifi() {
    document.getElementById('wifiList').innerText = 'Scanning nearby Wi-Fi networks...';
    fetch('/scan')
      .then(r => r.json())
      .then(list => {
        const box = document.getElementById('wifiList');
        if (!list.length) {
          box.innerText = 'No Wi-Fi networks were detected.';
          return;
        }

        box.innerHTML = '';
        list.forEach(item => {
          const div = document.createElement('div');
          div.className = 'wifiItem';

          const left = document.createElement('div');
          left.innerHTML = '<b>' + (item.ssid || '(Hidden SSID)') + '</b><br><span class="small">RSSI: ' + item.rssi + ' dBm | Security: ' + item.enc + '</span>';

          const right = document.createElement('div');
          const btn = document.createElement('button');
          btn.type = 'button';
          btn.innerText = 'Use';
          btn.onclick = function() {
            document.getElementById('ssid').value = item.ssid;
            document.getElementById('pass').value = '';
            document.getElementById('pass').focus();
          };
          right.appendChild(btn);

          div.appendChild(left);
          div.appendChild(right);
          box.appendChild(div);
        });
      });
  }

  function loadSaved() {
    document.getElementById('savedList').innerText = 'Loading saved Wi-Fi profiles...';
    fetch('/saved')
      .then(r => r.json())
      .then(list => {
        const box = document.getElementById('savedList');
        if (!list.length) {
          box.innerText = 'No saved Wi-Fi profiles are available.';
          return;
        }

        box.innerHTML = '';
        list.forEach(item => {
          const div = document.createElement('div');
          div.className = 'wifiItem';

          const left = document.createElement('div');
          left.innerHTML = '<b>' + item.ssid + '</b><br><span class="small">' + (item.hasPassword ? 'Password stored' : 'No password stored') + '</span>';

          const right = document.createElement('div');
          const btn = document.createElement('button');
          btn.type = 'button';
          btn.innerText = 'Use';
          btn.onclick = function() {
            document.getElementById('ssid').value = item.ssid || '';
            document.getElementById('pass').value = item.pass || '';
          };
          right.appendChild(btn);

          div.appendChild(left);
          div.appendChild(right);
          box.appendChild(div);
        });
      });
  }

  document.getElementById('wifiForm').addEventListener('submit', function(e) {
    e.preventDefault();
    const ssid = document.getElementById('ssid').value;
    const pass = document.getElementById('pass').value;
    window.location.href = '/connect-page?ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass);
  });

  loadSaved();
</script>
</body>
</html>
)rawliteral";
    return html;
  }

  String connectPage(const String &ssid, const String &pass) {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Connecting...</title>
  <style>
    body { margin: 0; font-family: Arial, sans-serif; background: #f4f7fb; color: #1f2937; }
    .wrap { max-width: 720px; margin: 40px auto; padding: 20px; }
    .card { background: white; border-radius: 16px; padding: 24px; box-shadow: 0 4px 16px rgba(0,0,0,0.07); }
    .small { color: #64748b; font-size: 14px; line-height: 1.6; }
    .ok { color: #059669; font-weight: bold; }
    .warn { color: #d97706; font-weight: bold; }
    .err { color: #dc2626; font-weight: bold; }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>Connecting to Wi-Fi</h1>
      <p class="small">The ESP32 will now attempt to connect to the selected wireless network.</p>
      <p><b>SSID:</b> )rawliteral";
    html += jsonEscape(ssid);
    html += R"rawliteral(</p>
      <p id="status" class="warn">Sending connection request...</p>
      <p id="hint" class="small"></p>
    </div>
  </div>

<script>
  const ssid = )rawliteral";
    html += "\"" + jsonEscape(ssid) + "\"";
    html += R"rawliteral(;
  const pass = )rawliteral";
    html += "\"" + jsonEscape(pass) + "\"";
    html += R"rawliteral(;

  async function beginConnect() {
    try {
      const r = await fetch('/connect-start?ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass));
      const txt = await r.text();
      document.getElementById('status').innerText = txt;
      setTimeout(checkStatus, 1200);
    } catch (e) {
      document.getElementById('status').className = 'err';
      document.getElementById('status').innerText = 'Unable to send the connection request.';
    }
  }

  async function checkStatus() {
    try {
      const r = await fetch('/connect-status');
      const d = await r.json();

      if (d.state === 'connecting') {
        document.getElementById('status').className = 'warn';
        document.getElementById('status').innerText = 'The ESP32 is connecting to the selected Wi-Fi network...';
        setTimeout(checkStatus, 1500);
        return;
      }

      if (d.state === 'connected') {
        document.getElementById('status').className = 'ok';
        document.getElementById('status').innerText = 'Connection established successfully. New IP: ' + d.staIp;
        document.getElementById('hint').innerText =
          'If the page does not open automatically, connect your computer to the same Wi-Fi network and visit: http://' + d.staIp + '/';
        setTimeout(() => {
          window.location.href = 'http://' + d.staIp + '/';
        }, 1200);
        return;
      }

      if (d.state === 'failed') {
        document.getElementById('status').className = 'err';
        document.getElementById('status').innerText = 'The connection attempt failed. The ESP32 has returned to AP mode.';
        document.getElementById('hint').innerText = 'Please go back to the Wi-Fi settings page and try again.';
        return;
      }

      document.getElementById('status').className = 'warn';
      document.getElementById('status').innerText = 'Waiting for connection status...';
      setTimeout(checkStatus, 1500);
    } catch (e) {
      document.getElementById('status').className = 'warn';
      document.getElementById('status').innerText = 'Waiting for a response from the ESP32...';
      setTimeout(checkStatus, 1500);
    }
  }

  beginConnect();
</script>
</body>
</html>
)rawliteral";
    return html;
  }

  void ensureServerStarted() {
    if (!g_serverStarted) {
      g_server.begin();
      g_serverStarted = true;
      Serial.println("[Web] HTTP server started.");
    }
  }

  void startApMode() {
    WiFi.disconnect(true, true);
    delay(100);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    g_isApMode = true;
    g_isStaConnecting = false;
    g_lastConnectFailed = false;
    g_staIp = "";

    ensureServerStarted();

    Serial.print("[Web] AP started. IP: ");
    Serial.println(WiFi.softAPIP());
  }

  void startStaMode(const String &ssid, const String &password) {
    if (ssid.isEmpty()) {
      startApMode();
      return;
    }

    WiFi.disconnect(false, true);
    delay(100);

    WiFi.mode(WIFI_AP_STA);
    if (WiFi.softAPIP().toString() == "0.0.0.0") {
      WiFi.softAP(AP_SSID, AP_PASSWORD);
      delay(100);
    }

    if (password.isEmpty()) {
      WiFi.begin(ssid.c_str());
    } else {
      WiFi.begin(ssid.c_str(), password.c_str());
    }

    g_staSsid = ssid;
    g_staPassword = password;
    g_staConnectStartMs = millis();
    g_isStaConnecting = true;
    g_isApMode = false;
    g_lastConnectFailed = false;
    g_staIp = "";

    ensureServerStarted();

    Serial.printf("[Web] STA connecting to SSID: %s\n", ssid.c_str());
  }

  void handleRoot() {
    if (g_isApMode) {
      g_server.send(200, "text/html", apPage());
    } else {
      g_server.send(200, "text/html", staPage());
    }
  }

  void handleSettings() {
    g_server.send(200, "text/html", settingsPage());
  }

  void handleSensors() {
    String tempText = isnan(g_latestSensor.temperature) ? "null" : String(g_latestSensor.temperature, 1);
    String humText  = isnan(g_latestSensor.humidity) ? "null" : String(g_latestSensor.humidity, 1);

    String json = "{";
    json += "\"temp\":" + tempText + ",";
    json += "\"hum\":" + humText + ",";
    json += "\"mode\":\"" + wifiStatusText() + "\",";
    json += "\"weather\":\"" + classifyWeatherText(g_latestSensor.temperature, g_latestSensor.humidity) + "\"";
    json += "}";

    g_server.send(200, "application/json", json);
  }

  void handleApiState() {
    String json = "{";
    json += "\"temp\":";
    json += isnan(g_latestSensor.temperature) ? "null" : String(g_latestSensor.temperature, 1);
    json += ",";
    json += "\"hum\":";
    json += isnan(g_latestSensor.humidity) ? "null" : String(g_latestSensor.humidity, 1);
    json += ",";
    json += "\"weather\":\"" + classifyWeatherText(g_latestSensor.temperature, g_latestSensor.humidity) + "\",";
    json += "\"mode\":\"" + wifiStatusText() + "\",";
    json += "\"staIp\":\"" + staIpText() + "\",";
    json += "\"ssid\":\"" + jsonEscape(g_staSsid) + "\",";

    json += "\"historyTemp\":[";
    for (int i = 0; i < g_historyCount; ++i) {
      int idx = (g_historyHead - g_historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
      if (i > 0) json += ",";
      json += isnan(g_tempHistory[idx]) ? "null" : String(g_tempHistory[idx], 1);
    }
    json += "],";

    json += "\"historyHum\":[";
    for (int i = 0; i < g_historyCount; ++i) {
      int idx = (g_historyHead - g_historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
      if (i > 0) json += ",";
      json += isnan(g_humHistory[idx]) ? "null" : String(g_humHistory[idx], 1);
    }
    json += "]";

    json += "}";

    g_server.send(200, "application/json", json);
  }

  void handleScan() {
    g_server.send(200, "application/json", wifiScanJson());
  }

  void handleSaved() {
    g_server.send(200, "application/json", savedWifiJson());
  }

  void handleConnectPage() {
    String ssid = g_server.arg("ssid");
    String pass = g_server.arg("pass");

    if (ssid.isEmpty()) {
      g_server.send(400, "text/plain", "SSID must not be empty.");
      return;
    }

    g_server.send(200, "text/html", connectPage(ssid, pass));
  }

  void handleConnectStart() {
    String ssid = g_server.arg("ssid");
    String pass = g_server.arg("pass");

    if (ssid.isEmpty()) {
      g_server.send(400, "text/plain", "SSID must not be empty.");
      return;
    }

    saveWifiRecent(ssid, pass);
    g_server.send(200, "text/plain", "Connection request received. The ESP32 is starting the Wi-Fi connection process...");

    delay(200);
    startStaMode(ssid, pass);
  }

  void handleConnectStatus() {
    String json = "{";

    if (g_isStaConnecting) {
      json += "\"state\":\"connecting\"";
    } else if (!g_staIp.isEmpty() && WiFi.status() == WL_CONNECTED) {
      json += "\"state\":\"connected\",";
      json += "\"staIp\":\"" + g_staIp + "\"";
    } else if (g_lastConnectFailed) {
      json += "\"state\":\"failed\"";
    } else {
      json += "\"state\":\"idle\"";
    }

    json += "}";
    g_server.send(200, "application/json", json);
  }

  void handleForget() {
    clearSavedWifi();
    g_server.send(200, "text/plain", "All saved Wi-Fi credentials have been removed.");
    delay(200);
    startApMode();
  }

  void setupRoutes() {
    g_server.on("/", HTTP_GET, handleRoot);
    g_server.on("/settings", HTTP_GET, handleSettings);
    g_server.on("/sensors", HTTP_GET, handleSensors);
    g_server.on("/api/state", HTTP_GET, handleApiState);
    g_server.on("/scan", HTTP_GET, handleScan);
    g_server.on("/saved", HTTP_GET, handleSaved);
    g_server.on("/connect-page", HTTP_GET, handleConnectPage);
    g_server.on("/connect-start", HTTP_GET, handleConnectStart);
    g_server.on("/connect-status", HTTP_GET, handleConnectStatus);
    g_server.on("/forget", HTTP_GET, handleForget);
  }

  void updateLatestSensorFromQueue() {
    if (g_ctx == nullptr || g_ctx->webQueue == nullptr) return;

    sensor_data_t temp{};
    while (xQueueReceive(g_ctx->webQueue, &temp, 0) == pdTRUE) {
      g_latestSensor = temp;
      if (!isnan(temp.temperature) && !isnan(temp.humidity)) {
        pushHistory(temp.temperature, temp.humidity);
      }
    }
  }

  void handleBootButton() {
    static bool lastBootState = HIGH;
    bool current = digitalRead(BOOT_PIN);

    if (lastBootState == HIGH && current == LOW) {
      delay(30);
      if (digitalRead(BOOT_PIN) == LOW) {
        Serial.println("[Web] BOOT pressed. Switching to AP mode.");
        startApMode();
      }
    }
    lastBootState = current;
  }

  void processStaConnectionState() {
    if (!g_isStaConnecting) return;

    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      g_isStaConnecting = false;
      g_isApMode = false;
      g_lastConnectFailed = false;
      g_staIp = WiFi.localIP().toString();

      if (g_ctx != nullptr && g_ctx->internetSemaphore != nullptr) {
        xSemaphoreGive(g_ctx->internetSemaphore);
      }

      Serial.print("[Web] STA connected. IP: ");
      Serial.println(g_staIp);
      return;
    }

    if (millis() - g_staConnectStartMs > STA_CONNECT_TIMEOUT) {
      Serial.println("[Web] STA connect timeout. Back to AP mode.");
      g_lastConnectFailed = true;
      startApMode();
    }
  }

  void maintainConnection() {
    if (g_isApMode || g_isStaConnecting) return;

    if (!g_staSsid.isEmpty() && WiFi.status() != WL_CONNECTED) {
      Serial.println("[Web] Wi-Fi lost. Retry STA...");
      startStaMode(g_staSsid, g_staPassword);
    }
  }
}

void main_server_task(void *pvParameters) {
  g_ctx = static_cast<app_context_t *>(pvParameters);

  pinMode(BOOT_PIN, INPUT_PULLUP);

  setupRoutes();

  String savedSsid, savedPass;
  bool hasSavedWifi = loadMostRecentWifi(savedSsid, savedPass);

  if (digitalRead(BOOT_PIN) == LOW) {
    Serial.println("[Web] BOOT held at startup -> force AP mode.");
    startApMode();
  } else if (hasSavedWifi) {
    Serial.println("[Web] Found saved Wi-Fi -> try STA first.");
    startStaMode(savedSsid, savedPass);
  } else {
    Serial.println("[Web] No saved Wi-Fi -> AP mode.");
    startApMode();
  }

  while (true) {
    g_server.handleClient();
    updateLatestSensorFromQueue();
    handleBootButton();
    processStaConnectionState();
    maintainConnection();

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
