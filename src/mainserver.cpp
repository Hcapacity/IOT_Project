#include "mainserver.h"

namespace {
  WebServer g_server(80);
  Preferences g_prefs;

  app_context_t *g_ctx = nullptr;
  sensor_data_t g_latestSensor = {NAN, NAN, 0};

  bool g_isApMode = false;
  bool g_isStaConnecting = false;
  bool g_serverStarted = false;

  unsigned long g_staConnectStartMs = 0;
  String g_staSsid;
  String g_staPassword;

  struct SavedWifi {
    String ssid;
    String pass;
  };

  // =========================
  // Utility
  // =========================
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

  String ipText() {
    if (g_isApMode) return WiFi.softAPIP().toString();
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
    return "-";
  }

  String classifyWeatherText(float t, float h) {
    if (isnan(t) || isnan(h)) return "NO DATA";

    // bám theo logic LCD spec của m
    if (t >= 35.0f || h >= 85.0f) return "CRITICAL";
    if ((t >= 30.0f && t < 35.0f) || h >= 70.0f) return "WARNING";
    if (t >= 20.0f && t < 30.0f && h >= 40.0f && h < 70.0f) return "NORMAL";

    if (t < 20.0f) return "COOL";
    return "NORMAL";
  }

  // =========================
  // Saved Wi-Fi management
  // =========================
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

    // bỏ bản cũ nếu SSID đã tồn tại
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

    // xóa phần dư
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
      json += "\"hasPassword\":" + String(item.pass.isEmpty() ? "false" : "true");
      json += "}";
    }
    json += "]";
    return json;
  }

  // =========================
  // Wi-Fi scan
  // =========================
  String wifiScanJson() {
    int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
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

  // =========================
  // Web pages
  // =========================
  String mainPage() {
    String tempText = isnan(g_latestSensor.temperature) ? "--" : String(g_latestSensor.temperature, 1);
    String humText  = isnan(g_latestSensor.humidity) ? "--" : String(g_latestSensor.humidity, 1);
    String weatherText = classifyWeatherText(g_latestSensor.temperature, g_latestSensor.humidity);

    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>ESP32 Climate Dashboard</title>
  <style>
    body {
      margin: 0;
      font-family: Arial, sans-serif;
      background: linear-gradient(135deg, #dff3ff, #f7fbff);
      color: #1f2937;
    }
    .wrap {
      max-width: 900px;
      margin: 0 auto;
      padding: 20px;
    }
    .hero {
      background: white;
      border-radius: 20px;
      padding: 24px;
      box-shadow: 0 8px 24px rgba(0,0,0,0.08);
      margin-bottom: 18px;
    }
    .title {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      flex-wrap: wrap;
    }
    .title h1 {
      margin: 0;
      font-size: 28px;
    }
    .subtitle {
      color: #6b7280;
      margin-top: 8px;
    }
    .status {
      display: inline-block;
      background: #eef6ff;
      color: #2563eb;
      border-radius: 999px;
      padding: 8px 14px;
      font-weight: bold;
    }
    .cards {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));
      gap: 16px;
      margin-top: 18px;
    }
    .card {
      background: white;
      border-radius: 20px;
      padding: 20px;
      box-shadow: 0 8px 24px rgba(0,0,0,0.08);
    }
    .metric {
      display: flex;
      align-items: center;
      gap: 14px;
    }
    .icon {
      width: 56px;
      height: 56px;
      border-radius: 16px;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 28px;
      background: #f3f4f6;
    }
    .metric h2 {
      margin: 0;
      font-size: 16px;
      color: #6b7280;
    }
    .metric .value {
      font-size: 28px;
      font-weight: bold;
      margin-top: 4px;
    }
    .chartBox {
      background: white;
      border-radius: 20px;
      padding: 20px;
      margin-top: 18px;
      box-shadow: 0 8px 24px rgba(0,0,0,0.08);
    }
    canvas {
      width: 100%;
      height: 260px;
      border-radius: 12px;
      background: #f9fbff;
    }
    .links {
      margin-top: 16px;
      display: flex;
      gap: 12px;
      flex-wrap: wrap;
    }
    .btn {
      border: none;
      border-radius: 12px;
      padding: 10px 14px;
      background: #2563eb;
      color: white;
      cursor: pointer;
      font-weight: 600;
      text-decoration: none;
      display: inline-block;
    }
    .btn.secondary {
      background: #475569;
    }
    .small {
      color: #6b7280;
      font-size: 14px;
      margin-top: 14px;
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <div class="title">
        <div>
          <h1>ESP32 Climate Dashboard</h1>
          <div class="subtitle">Theo dõi nhiệt độ, độ ẩm và trạng thái môi trường theo thời gian thực</div>
        </div>
        <div class="status" id="modeText">)rawliteral";
    html += wifiStatusText();
    html += R"rawliteral(</div>
      </div>

      <div class="links">
        <a class="btn" href="/settings">Wi-Fi Settings</a>
        <a class="btn secondary" href="/forget">Forget saved Wi-Fi</a>
      </div>

      <div class="small">
        IP: <span id="ipText">)rawliteral";
    html += ipText();
    html += R"rawliteral(</span>
        &nbsp; | &nbsp;
        Weather: <span id="weatherText">)rawliteral";
    html += weatherText;
    html += R"rawliteral(</span>
      </div>
    </div>

    <div class="cards">
      <div class="card">
        <div class="metric">
          <div class="icon">🌡️</div>
          <div>
            <h2>Nhiệt độ</h2>
            <div class="value"><span id="tempText">)rawliteral";
    html += tempText;
    html += R"rawliteral(</span> °C</div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="metric">
          <div class="icon">💧</div>
          <div>
            <h2>Độ ẩm</h2>
            <div class="value"><span id="humText">)rawliteral";
    html += humText;
    html += R"rawliteral(</span> %</div>
          </div>
        </div>
      </div>
    </div>

    <div class="chartBox">
      <h3>Biểu đồ nhiệt độ và độ ẩm</h3>
      <canvas id="sensorChart" width="860" height="260"></canvas>
      <div class="small">Biểu đồ được giữ lịch sử ở trình duyệt để giảm tải bộ nhớ cho ESP32.</div>
    </div>
  </div>

<script>
  const maxPoints = 30;
  const temps = [];
  const hums = [];
  const labels = [];

  function pushPoint(t, h) {
    const now = new Date();
    const label = now.getHours().toString().padStart(2,'0') + ':' +
                  now.getMinutes().toString().padStart(2,'0') + ':' +
                  now.getSeconds().toString().padStart(2,'0');

    labels.push(label);
    temps.push(t);
    hums.push(h);

    while (labels.length > maxPoints) labels.shift();
    while (temps.length > maxPoints) temps.shift();
    while (hums.length > maxPoints) hums.shift();
  }

  function drawChart() {
    const canvas = document.getElementById('sensorChart');
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;

    ctx.clearRect(0, 0, w, h);

    ctx.fillStyle = '#f9fbff';
    ctx.fillRect(0, 0, w, h);

    const padLeft = 50;
    const padRight = 20;
    const padTop = 20;
    const padBottom = 35;
    const chartW = w - padLeft - padRight;
    const chartH = h - padTop - padBottom;

    // grid
    ctx.strokeStyle = '#dbeafe';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 5; i++) {
      const y = padTop + (chartH / 5) * i;
      ctx.beginPath();
      ctx.moveTo(padLeft, y);
      ctx.lineTo(padLeft + chartW, y);
      ctx.stroke();
    }

    ctx.strokeStyle = '#94a3b8';
    ctx.beginPath();
    ctx.moveTo(padLeft, padTop);
    ctx.lineTo(padLeft, padTop + chartH);
    ctx.lineTo(padLeft + chartW, padTop + chartH);
    ctx.stroke();

    if (temps.length < 2) return;

    const allValues = temps.concat(hums);
    let minV = Math.min(...allValues);
    let maxV = Math.max(...allValues);

    if (minV === maxV) {
      minV -= 1;
      maxV += 1;
    }

    minV = Math.floor(minV - 1);
    maxV = Math.ceil(maxV + 1);

    function toX(i) {
      return padLeft + (i * chartW / (maxPoints - 1));
    }

    function toY(v) {
      return padTop + chartH - ((v - minV) / (maxV - minV)) * chartH;
    }

    // y labels
    ctx.fillStyle = '#475569';
    ctx.font = '12px Arial';
    for (let i = 0; i <= 5; i++) {
      const value = minV + (maxV - minV) * (5 - i) / 5;
      const y = padTop + (chartH / 5) * i + 4;
      ctx.fillText(value.toFixed(0), 8, y);
    }

    // temperature line
    ctx.strokeStyle = '#ef4444';
    ctx.lineWidth = 2;
    ctx.beginPath();
    temps.forEach((v, i) => {
      const x = toX(i);
      const y = toY(v);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();

    // humidity line
    ctx.strokeStyle = '#2563eb';
    ctx.lineWidth = 2;
    ctx.beginPath();
    hums.forEach((v, i) => {
      const x = toX(i);
      const y = toY(v);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();

    // legend
    ctx.fillStyle = '#ef4444';
    ctx.fillRect(w - 180, 16, 14, 14);
    ctx.fillStyle = '#111827';
    ctx.fillText('Temperature', w - 160, 28);

    ctx.fillStyle = '#2563eb';
    ctx.fillRect(w - 90, 16, 14, 14);
    ctx.fillStyle = '#111827';
    ctx.fillText('Humidity', w - 70, 28);
  }

  function refreshSensor() {
    fetch('/sensors')
      .then(r => r.json())
      .then(d => {
        document.getElementById('tempText').innerText = d.temp;
        document.getElementById('humText').innerText = d.hum;
        document.getElementById('modeText').innerText = d.mode;
        document.getElementById('ipText').innerText = d.ip;
        document.getElementById('weatherText').innerText = d.weather;

        if (d.temp !== null && d.hum !== null) {
          pushPoint(Number(d.temp), Number(d.hum));
          drawChart();
        }
      });
  }

  refreshSensor();
  setInterval(refreshSensor, 2000);
</script>
</body>
</html>
)rawliteral";

    return html;
  }

  String settingsPage() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>Wi-Fi Settings</title>
  <style>
    body {
      margin: 0;
      font-family: Arial, sans-serif;
      background: linear-gradient(135deg, #edf6ff, #f8fbff);
      color: #1f2937;
    }
    .wrap {
      max-width: 900px;
      margin: 0 auto;
      padding: 20px;
    }
    .card {
      background: white;
      border-radius: 20px;
      padding: 20px;
      box-shadow: 0 8px 24px rgba(0,0,0,0.08);
      margin-bottom: 18px;
    }
    h1,h2 { margin-top: 0; }
    input, select {
      width: 100%;
      padding: 12px;
      margin: 8px 0 14px 0;
      box-sizing: border-box;
      border-radius: 12px;
      border: 1px solid #cbd5e1;
      font-size: 15px;
    }
    button {
      border: none;
      border-radius: 12px;
      padding: 10px 14px;
      background: #2563eb;
      color: white;
      cursor: pointer;
      font-weight: 600;
      margin-right: 10px;
      margin-top: 6px;
    }
    button.secondary {
      background: #475569;
    }
    .wifiItem {
      padding: 12px;
      border: 1px solid #e2e8f0;
      border-radius: 12px;
      margin-bottom: 10px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      flex-wrap: wrap;
    }
    .small {
      color: #64748b;
      font-size: 14px;
    }
    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 18px;
    }
    @media (max-width: 760px) {
      .grid { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>Wi-Fi Settings</h1>
      <p class="small">Quét mạng Wi-Fi xung quanh, chọn nhanh SSID và lưu tối đa )rawliteral";
    html += String(WIFI_MAX_SAVED);
    html += R"rawliteral( mạng gần nhất vào flash ESP32.</p>
      <a href="/"><button class="secondary">Back Dashboard</button></a>
    </div>

    <div class="grid">
      <div class="card">
        <h2>Available Wi-Fi</h2>
        <button onclick="scanWifi()">Scan Wi-Fi</button>
        <div id="wifiList" class="small" style="margin-top:12px;">Chưa quét Wi-Fi.</div>
      </div>

      <div class="card">
        <h2>Saved Wi-Fi</h2>
        <button onclick="loadSaved()">Refresh Saved</button>
        <div id="savedList" class="small" style="margin-top:12px;">Chưa có dữ liệu.</div>
      </div>
    </div>

    <div class="card">
      <h2>Connect</h2>
      <form id="wifiForm">
        <label>SSID</label>
        <input id="ssid" type="text" placeholder="Wi-Fi SSID" required />

        <label>Password</label>
        <input id="pass" type="password" placeholder="Wi-Fi Password" />

        <button type="submit">Connect & Save</button>
      </form>
      <p id="msg" class="small"></p>
    </div>
  </div>

<script>
  function scanWifi() {
    document.getElementById('wifiList').innerText = 'Đang quét...';
    fetch('/scan')
      .then(r => r.json())
      .then(list => {
        if (!list.length) {
          document.getElementById('wifiList').innerText = 'Không tìm thấy Wi-Fi.';
          return;
        }

        let html = '';
        list.forEach(item => {
          html += `
            <div class="wifiItem">
              <div>
                <b>${item.ssid || '(Hidden SSID)'}</b><br>
                <span class="small">RSSI: ${item.rssi} dBm | ${item.enc}</span>
              </div>
              <div>
                <button onclick="useSsid('${String.raw``}${''}${'`'.replace('`','') || ''}')">Use</button>
              </div>
            </div>
          `;
        });

        // workaround vì template string với quote động dễ lỗi
        const box = document.getElementById('wifiList');
        box.innerHTML = '';
        list.forEach(item => {
          const div = document.createElement('div');
          div.className = 'wifiItem';

          const left = document.createElement('div');
          const title = document.createElement('b');
          title.innerText = item.ssid || '(Hidden SSID)';
          left.appendChild(title);

          const br = document.createElement('br');
          left.appendChild(br);

          const meta = document.createElement('span');
          meta.className = 'small';
          meta.innerText = 'RSSI: ' + item.rssi + ' dBm | ' + item.enc;
          left.appendChild(meta);

          const right = document.createElement('div');
          const btn = document.createElement('button');
          btn.innerText = 'Use';
          btn.onclick = function() {
            useSsid(item.ssid);
          };
          right.appendChild(btn);

          div.appendChild(left);
          div.appendChild(right);
          box.appendChild(div);
        });
      });
  }

  function useSsid(ssid) {
    document.getElementById('ssid').value = ssid;
    document.getElementById('pass').focus();
  }

  function loadSaved() {
    document.getElementById('savedList').innerText = 'Đang tải...';
    fetch('/saved')
      .then(r => r.json())
      .then(list => {
        if (!list.length) {
          document.getElementById('savedList').innerText = 'Chưa có Wi-Fi đã lưu.';
          return;
        }

        const box = document.getElementById('savedList');
        box.innerHTML = '';

        list.forEach(item => {
          const div = document.createElement('div');
          div.className = 'wifiItem';

          const left = document.createElement('div');
          const title = document.createElement('b');
          title.innerText = item.ssid;
          left.appendChild(title);

          const br = document.createElement('br');
          left.appendChild(br);

          const meta = document.createElement('span');
          meta.className = 'small';
          meta.innerText = item.hasPassword ? 'Đã lưu mật khẩu' : 'Không có mật khẩu';
          left.appendChild(meta);

          const right = document.createElement('div');
          const btn = document.createElement('button');
          btn.innerText = 'Use';
          btn.onclick = function() {
            document.getElementById('ssid').value = item.ssid;
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

    fetch('/connect?ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass))
      .then(r => r.text())
      .then(msg => {
        document.getElementById('msg').innerText = msg;
        loadSaved();
      });
  });

  loadSaved();
</script>
</body>
</html>
)rawliteral";

    return html;
  }

  // =========================
  // Server lifecycle
  // =========================
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

    ensureServerStarted();

    Serial.print("[Web] AP started. IP: ");
    Serial.println(WiFi.softAPIP());
  }

  void startStaMode(const String &ssid, const String &password) {
    if (ssid.isEmpty()) {
      startApMode();
      return;
    }

    WiFi.disconnect(true, true);
    delay(100);

    WiFi.mode(WIFI_STA);
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

    ensureServerStarted();

    Serial.printf("[Web] STA connecting to SSID: %s\n", ssid.c_str());
  }

  // =========================
  // Handlers
  // =========================
  void handleRoot() {
    g_server.send(200, "text/html", mainPage());
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
    json += "\"ip\":\"" + ipText() + "\",";
    json += "\"weather\":\"" + classifyWeatherText(g_latestSensor.temperature, g_latestSensor.humidity) + "\"";
    json += "}";

    g_server.send(200, "application/json", json);
  }

  void handleScan() {
    g_server.send(200, "application/json", wifiScanJson());
  }

  void handleSaved() {
    g_server.send(200, "application/json", savedWifiJson());
  }

  void handleConnect() {
    String ssid = g_server.arg("ssid");
    String pass = g_server.arg("pass");

    if (ssid.isEmpty()) {
      g_server.send(400, "text/plain", "SSID must not be empty.");
      return;
    }

    saveWifiRecent(ssid, pass);
    startStaMode(ssid, pass);
    g_server.send(200, "text/plain", "Connecting... Wi-Fi has been saved to recent list.");
  }

  void handleForget() {
    clearSavedWifi();
    g_server.send(200, "text/plain", "All saved Wi-Fi removed.");
    startApMode();
  }

  void setupRoutes() {
    g_server.on("/", HTTP_GET, handleRoot);
    g_server.on("/settings", HTTP_GET, handleSettings);
    g_server.on("/sensors", HTTP_GET, handleSensors);
    g_server.on("/scan", HTTP_GET, handleScan);
    g_server.on("/saved", HTTP_GET, handleSaved);
    g_server.on("/connect", HTTP_GET, handleConnect);
    g_server.on("/forget", HTTP_GET, handleForget);
  }

  // =========================
  // Runtime helpers
  // =========================
  void updateLatestSensorFromQueue() {
    if (g_ctx == nullptr || g_ctx->webQueue == nullptr) return;

    sensor_data_t temp{};
    while (xQueueReceive(g_ctx->webQueue, &temp, 0) == pdTRUE) {
      g_latestSensor = temp;
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

      if (g_ctx != nullptr && g_ctx->internetSemaphore != nullptr) {
        xSemaphoreGive(g_ctx->internetSemaphore);
      }

      Serial.print("[Web] STA connected. IP: ");
      Serial.println(WiFi.localIP());
      return;
    }

    if (millis() - g_staConnectStartMs > STA_CONNECT_TIMEOUT) {
      Serial.println("[Web] STA connect timeout. Back to AP mode.");
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