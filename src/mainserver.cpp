#include "mainserver.h"
#include <math.h>
#include <ArduinoJson.h>
#include "app_time_utils.h"
#include "coreiot.h"

namespace {

WebServer g_server(80);
Preferences g_prefs;
app_context_t *g_ctx = nullptr;

sensor_data_t g_latestSensor = {NAN, NAN, 0};
tinyml_result_t g_latestTiny = {0.0f, false, 0, 0};
bool g_hasTinyResult = false;

bool g_isApMode = false;
bool g_isStaConnecting = false;
bool g_serverStarted = false;
unsigned long g_staConnectStartMs = 0;
wl_status_t g_lastWifiStatus = WL_IDLE_STATUS;

String g_staSsid;
String g_staPassword;
String g_staIp;
constexpr int32_t kNtpGmtOffsetSec = 7 * 3600;
constexpr int32_t kNtpDaylightOffsetSec = 0;
static const char *kNtpServer = "pool.ntp.org";
bool g_ntpSynced = false;
unsigned long g_lastNtpTryMs = 0;
static const unsigned long kNtpRetryIntervalMs = 30000UL;
static const char *kPrefCoreiotEnabled = "ciot_en";
static const char *kPrefCoreiotHost = "ciot_host";
static const char *kPrefCoreiotUser = "ciot_user";
static const char *kPrefCoreiotPass = "ciot_pass";

static const int HISTORY_SIZE = 20;
float g_tempHistory[HISTORY_SIZE] = {0};
float g_humHistory[HISTORY_SIZE] = {0};
int g_historyCount = 0;
int g_historyHead = 0;
uint32_t g_historyVersion = 0;
uint32_t g_cachedHistoryVersion = UINT32_MAX;
String g_cachedHistoryJson;

struct SavedWifi {
  String ssid;
  String pass;
};

struct UserLightState {
  bool isOn;
  uint8_t brightnessPercent;
};

UserLightState g_userLight = {false, 50};

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

String tinymlLabel() {
  if (!g_hasTinyResult) return "COLLECTING";
  return g_latestTiny.isRain ? "RAIN" : "SUNNY";
}

String tinymlProbText() {
  if (!g_hasTinyResult) return "--";
  return String(g_latestTiny.rainProbability * 100.0f, 1);
}

void pushHistory(float t, float h) {
  g_tempHistory[g_historyHead] = t;
  g_humHistory[g_historyHead] = h;
  g_historyHead = (g_historyHead + 1) % HISTORY_SIZE;
  if (g_historyCount < HISTORY_SIZE) g_historyCount++;
  g_historyVersion++;
}

String historyArrayJson(const float *arr) {
  String json = "[";
  for (int i = 0; i < g_historyCount; ++i) {
    int index = (g_historyHead - g_historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
    if (i > 0) json += ",";
    json += String(arr[index], 2);
  }
  json += "]";
  return json;
}

String historyJson() {
  if (g_cachedHistoryVersion == g_historyVersion && !g_cachedHistoryJson.isEmpty()) {
    return g_cachedHistoryJson;
  }

  String json = "{";
  json += "\"historyVersion\":" + String(g_historyVersion) + ",";
  json += "\"tempHistory\":" + historyArrayJson(g_tempHistory) + ",";
  json += "\"humHistory\":" + historyArrayJson(g_humHistory);
  json += "}";

  g_cachedHistoryJson = json;
  g_cachedHistoryVersion = g_historyVersion;
  return g_cachedHistoryJson;
}

uint32_t percentToDuty(uint8_t percent) {
  const uint32_t maxDuty = (1u << USER_LED_PWM_RESOLUTION) - 1u;
  return (uint32_t)((percent * maxDuty) / 100u);
}

// ====== ĐIỀU KHIỂN GPIO10 NẰM Ở ĐÂY ======
void applyUserLight() {
  uint32_t duty = g_userLight.isOn ? percentToDuty(g_userLight.brightnessPercent) : 0;
  ledcWrite(USER_LED_PWM_CHANNEL, duty);
}

void loadUserLightPrefs() {
  g_prefs.begin(WIFI_STORE_NAMESPACE, true);
  bool on = g_prefs.getBool("usr_led_on", false);
  int pct = g_prefs.getInt("usr_led_pct", 50);
  g_prefs.end();

  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  g_userLight.isOn = on;
  g_userLight.brightnessPercent = static_cast<uint8_t>(pct);
}

void saveUserLightPrefs() {
  g_prefs.begin(WIFI_STORE_NAMESPACE, false);
  g_prefs.putBool("usr_led_on", g_userLight.isOn);
  g_prefs.putInt("usr_led_pct", g_userLight.brightnessPercent);
  g_prefs.end();
}

void initUserLight() {
  pinMode(USER_LED_GPIO, OUTPUT);
  ledcSetup(USER_LED_PWM_CHANNEL, USER_LED_PWM_FREQ, USER_LED_PWM_RESOLUTION);
  ledcAttachPin(USER_LED_GPIO, USER_LED_PWM_CHANNEL);
  loadUserLightPrefs();
  applyUserLight();
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
    const String ssidKey = "ssid" + String(i);
    const String passKey = "pass" + String(i);
    if (g_prefs.isKey(ssidKey.c_str())) {
      g_prefs.remove(ssidKey.c_str());
    }
    if (g_prefs.isKey(passKey.c_str())) {
      g_prefs.remove(passKey.c_str());
    }
  }
  g_prefs.end();

  setSavedCount(newCount);
}

void loadCoreiotPrefsAndApply() {
  g_prefs.begin(WIFI_STORE_NAMESPACE, true);
  const bool enabled = g_prefs.getBool(kPrefCoreiotEnabled, true);
  String host = g_prefs.getString(kPrefCoreiotHost, "");
  String username = g_prefs.getString(kPrefCoreiotUser, "");
  String password = g_prefs.getString(kPrefCoreiotPass, "");
  g_prefs.end();

  coreiot_set_publish_enabled(enabled);

  host.trim();
  username.trim();

  if (!host.isEmpty()) {
    coreiot_set_broker_host(host.c_str());
  }
  if (!username.isEmpty()) {
    coreiot_set_credentials(username.c_str(), password.c_str());
  }
}

void saveCoreiotEnabledPref(bool enabled) {
  g_prefs.begin(WIFI_STORE_NAMESPACE, false);
  g_prefs.putBool(kPrefCoreiotEnabled, enabled);
  g_prefs.end();
}

void saveCoreiotConnectionPrefs(const String &host, const String &username, const String &password) {
  g_prefs.begin(WIFI_STORE_NAMESPACE, false);
  g_prefs.putString(kPrefCoreiotHost, host);
  g_prefs.putString(kPrefCoreiotUser, username);
  g_prefs.putString(kPrefCoreiotPass, password);
  g_prefs.end();
}

String savedWifiJson() {
  int count = getSavedCount();
  String json = "[";

  for (int i = 0; i < count; ++i) {
    SavedWifi item = readSavedAt(i);
    if (i > 0) json += ",";
    json += "{";
    json += "\"ssid\":\"" + jsonEscape(item.ssid) + "\",";
    json += "\"pass\":\"" + jsonEscape(item.pass) + "\"";
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
    json += "\"quality\":" + String(wifiQualityPercent(WiFi.RSSI(i))) + ",";
    json += "\"enc\":\"" + enc + "\"";
    json += "}";
  }

  json += "]";
  WiFi.scanDelete();
  return json;
}

void ensureServerStarted() {
  if (!g_serverStarted) {
    g_server.begin();
    g_serverStarted = true;
  }
}

void notifyInternetReady() {
  if (g_ctx == nullptr || g_ctx->internetSemaphore == nullptr) {
    Serial.println("[Web] Cannot signal internet semaphore: invalid context");
    return;
  }

  BaseType_t ok = xSemaphoreGive(g_ctx->internetSemaphore);
  Serial.printf("[Web] internetSemaphore -> %s\n", ok == pdTRUE ? "GIVE OK" : "GIVE FAIL");
}

void trySyncNtp(bool forceNow) {
  if (WiFi.status() != WL_CONNECTED) return;

  const unsigned long nowMs = millis();
  if (!forceNow && g_ntpSynced) return;
  if (!forceNow && (nowMs - g_lastNtpTryMs < kNtpRetryIntervalMs)) return;

  g_lastNtpTryMs = nowMs;

  const uint32_t beforeEpoch = static_cast<uint32_t>(time(nullptr));
  Serial.printf("[Time] NTP try | force=%d | epoch_before=%lu\n",
                forceNow ? 1 : 0,
                static_cast<unsigned long>(beforeEpoch));

  const bool ok = appTimeSyncNtp(
    kNtpServer,
    kNtpGmtOffsetSec,
    kNtpDaylightOffsetSec,
    1704067200U,
    15000U,
    250U
  );

  g_ntpSynced = ok;
  const uint32_t afterEpoch = static_cast<uint32_t>(time(nullptr));
  Serial.printf("[Time] NTP sync -> %s | epoch_after=%lu\n",
                ok ? "OK" : "FAIL",
                static_cast<unsigned long>(afterEpoch));
}

void startApMode() {
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  delay(200);

  WiFi.mode(WIFI_AP);
  delay(100);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  g_isApMode = true;
  g_isStaConnecting = false;
  g_staConnectStartMs = 0;
  g_staIp = "";
  g_staSsid = "";
  g_staPassword = "";
  app_set_wifi_connected(g_ctx, false);
  g_lastWifiStatus = WiFi.status();

  ensureServerStarted();

  Serial.printf("[Web] AP mode started | mode=%d | status=%d | AP IP=%s\n",
                WiFi.getMode(),
                WiFi.status(),
                WiFi.softAPIP().toString().c_str());
}

void startStaConnect(const String &ssid, const String &password) {
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  g_isApMode = false;
  g_isStaConnecting = true;
  g_staConnectStartMs = millis();
  g_staSsid = ssid;
  g_staPassword = password;
  g_staIp = "";
  app_set_wifi_connected(g_ctx, false);
  g_lastWifiStatus = WiFi.status();

  ensureServerStarted();

  Serial.printf("[Web] STA connect start | SSID=%s | mode=%d | status=%d\n",
                ssid.c_str(),
                WiFi.getMode(),
                WiFi.status());
}

String apPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 AP Dashboard</title>
<style>
body{margin:0;font-family:Arial,sans-serif;background:#0f172a;color:#e2e8f0}
.wrap{max-width:1100px;margin:auto;padding:20px}
h1,h2{margin:0 0 12px 0}
p{opacity:.9}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px}
.card{background:#1e293b;border-radius:18px;padding:16px;box-shadow:0 8px 24px rgba(0,0,0,.25)}
.metric{font-size:32px;font-weight:700}
.label{font-size:13px;opacity:.8;text-transform:uppercase;letter-spacing:.08em}
.hero{display:grid;grid-template-columns:1.2fr .8fr;gap:16px;margin-bottom:18px}
@media(max-width:900px){.hero{grid-template-columns:1fr}}
.btn{background:#38bdf8;border:none;color:#062033;padding:10px 14px;border-radius:12px;font-weight:700;cursor:pointer}
.btn.secondary{background:#334155;color:#f8fafc}
.btn.warn{background:#f59e0b;color:#1f1300}
.badge{display:inline-block;padding:6px 10px;border-radius:999px;background:#334155;font-size:12px}
canvas{width:100%;background:#0b1220;border-radius:14px}
.icon{font-size:30px}
.small{font-size:12px;opacity:.8}
.list{display:grid;gap:8px;margin-top:10px}
.item{padding:10px;border-radius:10px;background:#0f172a}
.slider-wrap{display:flex;align-items:center;gap:12px;margin-top:12px}
.slider-wrap input[type=range]{flex:1}
.value-box{min-width:52px;text-align:center;padding:8px 10px;border-radius:10px;background:#0f172a}
a.link{color:#7dd3fc;text-decoration:none}
</style>
</head>
<body>
<div class="wrap">
  <div class="hero">
    <div class="card">
      <h1>ESP32 Climate Dashboard</h1>
      <p>AP mode dashboard with temperature, humidity, TinyML prediction, Wi-Fi setup, chart history, and LED brightness control.</p>
      <div style="display:flex;gap:10px;flex-wrap:wrap">
        <span class="badge">AP SSID: )rawliteral";
  html += AP_SSID;
  html += R"rawliteral(</span>
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
      <div class="metric"><span class="icon">🌡️</span> <span id="tempValue">--</span> °C</div>
    </div>
    <div class="card">
      <div class="label">Humidity</div>
      <div class="metric"><span class="icon">💧</span> <span id="humValue">--</span> %</div>
    </div>
    <div class="card">
      <div class="label">TinyML Prediction</div>
      <div class="metric"><span class="icon">🧠</span> <span id="tinyLabel">--</span></div>
      <div class="small">Rain probability: <span id="tinyProb">--</span>%</div>
    </div>
    <div class="card">
      <div class="label">Wi-Fi Target</div>
      <div class="metric"><span class="icon">📶</span> <span id="staSsid">-</span></div>
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
        <a class="link" href="/settings">Advanced settings</a>
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

function updateSliderValue(val){
  document.getElementById('sliderValue').textContent = val;
  sliderDirty = true;
}

function drawChart(temp, hum){
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
    if (!valid.length) {
      return { min: fallbackMin, max: fallbackMax };
    }

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

  const getX = index => {
    if (sampleCount <= 1) {
      return plot.left + plotWidth / 2;
    }
    return plot.left + (index * plotWidth) / (sampleCount - 1);
  };

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

    if (hasPoint) {
      ctx.stroke();
    }

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

  ctx.strokeStyle = '#334155';
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = plot.top + (i * plotHeight) / 4;
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(plot.right, y);
    ctx.stroke();
  }

  for (let i = 0; i <= 4; i++) {
    const x = plot.left + (i * plotWidth) / 4;
    ctx.beginPath();
    ctx.moveTo(x, plot.top);
    ctx.lineTo(x, plot.bottom);
    ctx.stroke();
  }

  ctx.strokeStyle = '#64748b';
  ctx.beginPath();
  ctx.moveTo(plot.left, plot.bottom);
  ctx.lineTo(plot.right, plot.bottom);
  ctx.moveTo(plot.left, plot.top);
  ctx.lineTo(plot.left, plot.bottom);
  ctx.moveTo(plot.right, plot.top);
  ctx.lineTo(plot.right, plot.bottom);
  ctx.stroke();

  ctx.font = '12px Arial';
  ctx.textBaseline = 'middle';

  const drawAxisLabels = (range, x, color, align, suffix) => {
    const values = [range.max, (range.max + range.min) / 2, range.min];
    const positions = [plot.top, (plot.top + plot.bottom) / 2, plot.bottom];
    ctx.fillStyle = color;
    ctx.textAlign = align;
    values.forEach((value, index) => {
      ctx.fillText(`${value.toFixed(1)}${suffix}`, x, positions[index]);
    });
  };

  drawAxisLabels(tempRange, plot.left - 10, '#22d3ee', 'right', 'C');
  drawAxisLabels(humRange, plot.right + 10, '#4ade80', 'left', '%');

  ctx.fillStyle = '#cbd5e1';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  for (let i = 0; i <= 4; i++) {
    const x = plot.left + (i * plotWidth) / 4;
    const sampleIndex = sampleCount <= 1 ? 0 : Math.round((i * (sampleCount - 1)) / 4);
    const secondsAgo = (sampleCount - 1 - sampleIndex) * sampleIntervalSec;
    ctx.fillText(`-${secondsAgo}s`, x, plot.bottom + 8);
  }

  ctx.fillStyle = '#22d3ee';
  ctx.textAlign = 'left';
  ctx.textBaseline = 'top';
  ctx.fillText('Temp (C)', plot.left, 6);

  ctx.fillStyle = '#4ade80';
  ctx.textAlign = 'right';
  ctx.fillText('Humidity (%)', plot.right, 6);

  if (!sampleCount) {
    ctx.fillStyle = '#94a3b8';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText('Waiting for sensor history...', c.width / 2, c.height / 2);
    return;
  }

  drawSeries(tempData, '#22d3ee', tempRange);
  drawSeries(humData, '#4ade80', humRange);
}

async function loadState(){
  const res = await fetch('/api/state');
  const s = await res.json();

  document.getElementById('tempValue').textContent = s.temperatureText;
  document.getElementById('humValue').textContent = s.humidityText;
  document.getElementById('tinyLabel').textContent = s.tinyLabel;
  document.getElementById('tinyProb').textContent = s.tinyProbability;
  document.getElementById('wifiStatus').textContent = s.wifiStatus;
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
}

async function loadHistory(){
  const res = await fetch('/api/history');
  const h = await res.json();
  if (typeof h.historyVersion === 'number') {
    lastHistoryVersion = h.historyVersion;
  }
  drawChart(h.tempHistory, h.humHistory);
}

async function toggleLight(){
  await fetch('/api/light/toggle', {method:'POST'});
  await loadState();
}

async function applyBrightness(){
  const val = document.getElementById('brightnessSlider').value;
  await fetch('/api/light/brightness', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'value=' + encodeURIComponent(val)
  });
  sliderDirty = false;
  await loadState();
}

async function loadScan(){
  const res = await fetch('/api/scan');
  const arr = await res.json();
  const box = document.getElementById('scanList');
  box.innerHTML = '';
  if(!arr.length){
    box.innerHTML = '<div class="item">No networks found.</div>';
    return;
  }
  arr.forEach(w=>{
    const div=document.createElement('div');
    div.className='item';
    div.textContent = `${w.ssid || '(hidden)'} | ${w.quality}% | ${w.enc}`;
    box.appendChild(div);
  });
}

loadState();
loadHistory();
setInterval(loadState, 1000);
setInterval(loadHistory, 3000);
</script>
</body>
</html>
)rawliteral";
  return html;
}

String staPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 STA CoreIOT</title>
<style>
body{margin:0;background:#0f172a;color:#e2e8f0;font-family:Arial,sans-serif}
.wrap{max-width:560px;margin:auto;padding:16px}
.card{background:#1e293b;border-radius:12px;padding:14px;margin-bottom:12px}
.row{display:flex;justify-content:space-between;gap:10px;padding:6px 0;border-bottom:1px solid #334155}
.row:last-child{border-bottom:none}
.label{opacity:.8}
.value{font-weight:700}
.btn{background:#38bdf8;border:none;color:#062033;padding:10px 12px;border-radius:10px;font-weight:700;cursor:pointer;width:100%}
input{width:100%;padding:10px;border-radius:10px;border:1px solid #475569;background:#0f172a;color:#fff;box-sizing:border-box}
.toggle{display:flex;align-items:center;gap:8px;margin-bottom:10px}
.small{font-size:12px;opacity:.8}
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
async function loadState(){
  const res = await fetch('/api/state');
  const s = await res.json();
  document.getElementById('wifiStatus').textContent = s.wifiStatus || '-';
  document.getElementById('staSsid').textContent = s.staSsid || '-';
  document.getElementById('staIp').textContent = s.staIp || '-';
  document.getElementById('coreiotStatus').textContent = s.coreiotMqtt ? 'CONNECTED' : 'DISCONNECTED';
  document.getElementById('coreiotRetrySec').textContent = String(s.coreiotRetrySec || 0) + 's';
}

async function loadCoreiotConfig(){
  const res = await fetch('/api/coreiot/config');
  const cfg = await res.json();
  document.getElementById('coreiotEnabled').checked = !!cfg.enabled;
  document.getElementById('coreiotHost').value = cfg.host || '';
  document.getElementById('coreiotUsername').value = cfg.username || '';
  document.getElementById('coreiotPassword').value = cfg.password || '';
}

async function saveCoreiotConfig(){
  const enabled = document.getElementById('coreiotEnabled').checked;
  const host = document.getElementById('coreiotHost').value || '';
  const username = document.getElementById('coreiotUsername').value || '';
  const password = document.getElementById('coreiotPassword').value || '';
  const body = 'enabled=' + encodeURIComponent(enabled ? '1' : '0') +
               '&host=' + encodeURIComponent(host) +
               '&username=' + encodeURIComponent(username) +
               '&password=' + encodeURIComponent(password);

  const res = await fetch('/api/coreiot/config', {
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body
  });

  const msg = document.getElementById('coreiotMsg');
  msg.textContent = res.ok ? 'Saved' : 'Save failed';
  await loadState();
}

loadState();
loadCoreiotConfig();
setInterval(loadState, 3000);
</script>
</body>
</html>
)rawliteral";
  return html;
}

String settingsPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wi-Fi Settings</title>
<style>
body{margin:0;font-family:Arial,sans-serif;background:#0f172a;color:#e2e8f0}
.wrap{max-width:960px;margin:auto;padding:20px}
.card{background:#1e293b;border-radius:18px;padding:16px;margin-bottom:16px}
.row{display:flex;gap:10px;flex-wrap:wrap}
.btn{background:#38bdf8;border:none;color:#062033;padding:10px 14px;border-radius:12px;font-weight:700;cursor:pointer}
.btn.secondary{background:#334155;color:#f8fafc}
input{width:100%;padding:10px;border-radius:10px;border:1px solid #475569;background:#0f172a;color:#fff}
.list{display:grid;gap:8px;margin-top:10px}
.item{padding:10px;border-radius:10px;background:#0f172a}
a{color:#7dd3fc;text-decoration:none}
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
        <button class="btn" type="submit">Connect & Save</button>
      </div>
    </form>
  </div>
</div>

<script>
function fillWifi(ssid, pass){
  document.getElementById('ssid').value = ssid || '';
  document.getElementById('password').value = pass || '';
}

async function loadScan(){
  const res = await fetch('/api/scan');
  const arr = await res.json();
  const box = document.getElementById('scanList');
  box.innerHTML = '';
  if(!arr.length){
    box.innerHTML = '<div class="item">No scan results available.</div>';
    return;
  }
  arr.forEach(w=>{
    const div=document.createElement('div');
    div.className='item';
    div.textContent = `${w.ssid || '(hidden)'} | ${w.quality}% | ${w.enc}`;
    box.appendChild(div);
  });
}

async function loadSaved(){
  const res = await fetch('/api/saved');
  const arr = await res.json();
  const box = document.getElementById('savedList');
  box.innerHTML = '';
  if(!arr.length){
    box.innerHTML = '<div class="item">No saved profiles found.</div>';
    return;
  }
  arr.forEach(w=>{
    const div = document.createElement('div');
    div.className = 'item';
    div.innerHTML = `<b>${w.ssid}</b>
                     <div style="margin-top:8px"><button class="btn secondary" onclick="fillWifi('${w.ssid.replace(/'/g,"\\'")}','${(w.pass || '').replace(/'/g,"\\'")}')">Use saved profile</button></div>`;
    box.appendChild(div);
  });
}

async function forgetAll(){
  await fetch('/forget', {method:'POST'});
  loadSaved();
}

loadSaved();
</script>
</body>
</html>
)rawliteral";
  return html;
}

String connectPage(const String &ssid) {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connecting</title>
<style>
body{margin:0;font-family:Arial,sans-serif;background:#0f172a;color:#e2e8f0}
.wrap{max-width:760px;margin:auto;padding:24px}
.card{background:#1e293b;border-radius:18px;padding:18px}
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <h1>Connecting to Wi-Fi</h1>
    <p>ESP32 is trying to connect to SSID: <b>)rawliteral";
  html += jsonEscape(ssid);
  html += R"rawliteral(</b></p>
    <p>If no link is established within 10 seconds, the board automatically returns to AP mode.</p>
    <p><a href="/">Back</a></p>
  </div>
</div>
</body>
</html>
)rawliteral";
  return html;
}

String stateJson() {
  StaticJsonDocument<384> doc;
  doc["wifiStatus"] = wifiStatusText();
  doc["apIp"] = apIpText();
  doc["staIp"] = staIpText();
  doc["staSsid"] = g_staSsid;
  doc["temperatureText"] = isnan(g_latestSensor.temperature) ? "--" : String(g_latestSensor.temperature, 1);
  doc["humidityText"] = isnan(g_latestSensor.humidity) ? "--" : String(g_latestSensor.humidity, 1);
  doc["tinyLabel"] = tinymlLabel();
  doc["tinyProbability"] = tinymlProbText();
  doc["userLightOn"] = g_userLight.isOn;
  doc["userLightBrightness"] = g_userLight.brightnessPercent;
  doc["coreiotMqtt"] = app_get_coreiot_mqtt_connected(g_ctx);
  doc["coreiotRetrySec"] = app_get_coreiot_retry_sec(g_ctx);
  doc["historyVersion"] = g_historyVersion;

  String json;
  json.reserve(320);
  serializeJson(doc, json);
  return json;
}

void handleApiHistory() {
  g_server.send(200, "application/json", historyJson());
}

void handleRoot() {
  if (g_isApMode) g_server.send(200, "text/html", apPage());
  else g_server.send(200, "text/html", staPage());
}

void handleSettings() {
  if (!g_isApMode) {
    g_server.sendHeader("Location", "/", true);
    g_server.send(302, "text/plain", "Redirect");
    return;
  }
  g_server.send(200, "text/html", settingsPage());
}

void handleApiState() {
  g_server.send(200, "application/json", stateJson());
}

void handleApiScan() {
  g_server.send(200, "application/json", wifiScanJson());
}

void handleApiSaved() {
  g_server.send(200, "application/json", savedWifiJson());
}

void handleApiCoreiotConfigGet() {
  char host[64] = {0};
  char username[80] = {0};
  char password[80] = {0};
  coreiot_get_broker_host(host, sizeof(host));
  coreiot_get_credentials(username, sizeof(username), password, sizeof(password));

  StaticJsonDocument<320> doc;
  doc["enabled"] = coreiot_get_publish_enabled();
  doc["host"] = host;
  doc["username"] = username;
  doc["password"] = password;

  String payload;
  serializeJson(doc, payload);
  g_server.send(200, "application/json", payload);
}

void handleApiCoreiotConfigPost() {
  bool enabled = coreiot_get_publish_enabled();
  if (g_server.hasArg("enabled")) {
    String en = g_server.arg("enabled");
    en.trim();
    en.toLowerCase();
    enabled = (en == "1" || en == "true" || en == "on" || en == "yes");
  }

  String host;
  String username;
  String password;

  if (g_server.hasArg("host")) {
    host = g_server.arg("host");
    host.trim();
  }
  if (g_server.hasArg("username")) {
    username = g_server.arg("username");
    username.trim();
  }
  if (g_server.hasArg("password")) {
    password = g_server.arg("password");
  }

  if (!host.isEmpty() && !coreiot_set_broker_host(host.c_str())) {
    g_server.send(400, "application/json", "{\"ok\":false,\"reason\":\"invalid_host\"}");
    return;
  }

  if (!username.isEmpty()) {
    if (!coreiot_set_credentials(username.c_str(), password.c_str())) {
      g_server.send(400, "application/json", "{\"ok\":false,\"reason\":\"invalid_credentials\"}");
      return;
    }
  }

  coreiot_set_publish_enabled(enabled);
  saveCoreiotConnectionPrefs(host, username, password);
  saveCoreiotEnabledPref(enabled);

  g_server.send(200, "application/json", "{\"ok\":true}");
}

void handleConnect() {
  String ssid = g_server.arg("ssid");
  String password = g_server.arg("password");
  ssid.trim();

  if (ssid.isEmpty()) {
    g_server.send(400, "text/plain", "SSID is required.");
    return;
  }

  saveWifiRecent(ssid, password);
  startStaConnect(ssid, password);
  g_server.send(200, "text/html", connectPage(ssid));
}

void handleForget() {
  clearSavedWifi();
  g_server.send(200, "application/json", "{\"ok\":true}");
}

void handleToggleLight() {
  g_userLight.isOn = !g_userLight.isOn;
  applyUserLight();
  saveUserLightPrefs();

  Serial.printf("[LED] Toggle: %s | brightness=%d%%\n",
                g_userLight.isOn ? "ON" : "OFF",
                g_userLight.brightnessPercent);

  g_server.send(200, "application/json", "{\"ok\":true}");
}

void handleBrightness() {
  int value = g_server.arg("value").toInt();

  if (value < 0) value = 0;
  if (value > 100) value = 100;

  g_userLight.brightnessPercent = static_cast<uint8_t>(value);
  applyUserLight();
  saveUserLightPrefs();

  Serial.printf("[LED] Brightness set to %d%%\n", g_userLight.brightnessPercent);

  g_server.send(200, "application/json", "{\"ok\":true}");
}

void setupRoutes() {
  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/settings", HTTP_GET, handleSettings);
  g_server.on("/api/state", HTTP_GET, handleApiState);
  g_server.on("/api/history", HTTP_GET, handleApiHistory);
  g_server.on("/api/coreiot/config", HTTP_GET, handleApiCoreiotConfigGet);
  g_server.on("/api/coreiot/config", HTTP_POST, handleApiCoreiotConfigPost);
  g_server.on("/api/scan", HTTP_GET, handleApiScan);
  g_server.on("/api/saved", HTTP_GET, handleApiSaved);
  g_server.on("/connect", HTTP_POST, handleConnect);
  g_server.on("/forget", HTTP_POST, handleForget);
  g_server.on("/api/light/toggle", HTTP_POST, handleToggleLight);
  g_server.on("/api/light/brightness", HTTP_POST, handleBrightness);
  g_server.onNotFound([]() {
    if (g_server.uri() == "/favicon.ico") {
      g_server.send(204, "text/plain", "");
      return;
    }
    g_server.send(404, "application/json", "{\"ok\":false,\"reason\":\"not_found\"}");
  });
}

void processQueues() {
  if (g_ctx == nullptr) return;

  sensor_data_t incoming{};
  if (g_ctx->webQueue != nullptr) {
    while (xQueueReceive(g_ctx->webQueue, &incoming, 0) == pdTRUE) {
      g_latestSensor = incoming;
      if (!isnan(incoming.temperature) && !isnan(incoming.humidity)) {
        pushHistory(incoming.temperature, incoming.humidity);
      }
    }
  }

  if (g_ctx->tinyResultQueue != nullptr) {
    tinyml_result_t tiny{};
    if (xQueuePeek(g_ctx->tinyResultQueue, &tiny, 0) == pdTRUE) {
      g_latestTiny = tiny;
      g_hasTinyResult = true;
    }
  }
}

void processWifiState() {
  wl_status_t currentStatus = WiFi.status();
  if (currentStatus != g_lastWifiStatus) {
    Serial.printf("[Web] WiFi status changed: %d -> %d | mode=%d\n",
                  g_lastWifiStatus,
                  currentStatus,
                  WiFi.getMode());
    g_lastWifiStatus = currentStatus;
  }

  if (g_isStaConnecting && WiFi.status() == WL_CONNECTED) {
    g_isStaConnecting = false;
    g_isApMode = false;
    g_staIp = WiFi.localIP().toString();
    app_set_wifi_connected(g_ctx, true);
    notifyInternetReady();

    Serial.printf("[Web] STA connected | SSID=%s | IP=%s | RSSI=%d\n",
                  WiFi.SSID().c_str(),
                  g_staIp.c_str(),
                  WiFi.RSSI());
    Serial.printf("[Web] Open STA page at: http://%s/\n", g_staIp.c_str());

    trySyncNtp(true);
  }

  if (!g_isApMode && !g_isStaConnecting && currentStatus != WL_CONNECTED) {
    app_set_wifi_connected(g_ctx, false);
    g_ntpSynced = false;
  }

  if (g_isStaConnecting && (millis() - g_staConnectStartMs >= STA_CONNECT_TIMEOUT)) {
    Serial.printf("[Web] STA connect timeout -> back to AP | last status=%d\n", WiFi.status());
    startApMode();
  }

  if (!g_isApMode && !g_isStaConnecting && currentStatus == WL_CONNECTED) {
    trySyncNtp(false);
  }
}

void processBootButton() {
  static int lastReading = HIGH;
  static int stableState = HIGH;
  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 50;

  int reading = digitalRead(BOOT_PIN);

  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != stableState) {
      stableState = reading;
      if (stableState == LOW) {
        Serial.println("[Web] BOOT pressed -> AP mode");
        startApMode();
      }
    }
  }

  lastReading = reading;
}

} // namespace

void main_server_task(void *pvParameters) {
  g_ctx = static_cast<app_context_t *>(pvParameters);
  if (g_ctx == nullptr || g_ctx->webQueue == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  pinMode(BOOT_PIN, INPUT_PULLUP);

  initUserLight();
  loadCoreiotPrefsAndApply();
  setupRoutes();

  String savedSsid, savedPassword;
  if (loadMostRecentWifi(savedSsid, savedPassword)) {
    startStaConnect(savedSsid, savedPassword);
  } else {
    startApMode();
  }

  while (true) {
    processQueues();
    processWifiState();
    processBootButton();
    g_server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
