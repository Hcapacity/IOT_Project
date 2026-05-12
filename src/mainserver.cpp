#include "mainserver.h"

#include <ArduinoJson.h>
#include <math.h>

#include "app_time_utils.h"
#include "coreiot.h"
#include "web_embedded_pages.h"

namespace {

// ===== Runtime state =====

WebServer g_webServer(80);
Preferences g_preferences;
app_context_t *g_appContext = nullptr;

sensor_data_t g_latestSensorData = {NAN, NAN, 0};
tinyml_result_t g_latestTinyMlResult = {0.0f, false, 0, 0};
bool g_hasTinyMlResult = false;

bool g_isApModeActive = false;
bool g_isStaConnecting = false;
bool g_hasServerStarted = false;

unsigned long g_staConnectStartMs = 0;
wl_status_t g_lastWifiStatus = WL_IDLE_STATUS;

String g_targetStaSsid;
String g_staIpAddress;

constexpr int32_t kNtpGmtOffsetSec = 7 * 3600;
constexpr int32_t kNtpDaylightOffsetSec = 0;
static const char *kNtpServer = "pool.ntp.org";
static const unsigned long kNtpRetryIntervalMs = 30000UL;

bool g_hasSyncedNtp = false;
unsigned long g_lastNtpAttemptMs = 0;

static const char *kCoreIotEnabledPrefKey = "ciot_en";
static const char *kCoreIotHostPrefKey = "ciot_host";
static const char *kCoreIotUserPrefKey = "ciot_user";
static const char *kCoreIotPassPrefKey = "ciot_pass";

constexpr int kHistorySize = 20;
float g_temperatureHistory[kHistorySize] = {0};
float g_humidityHistory[kHistorySize] = {0};
int g_historyItemCount = 0;
int g_historyHeadIndex = 0;
uint32_t g_historyVersion = 0;
uint32_t g_cachedHistoryVersion = UINT32_MAX;
String g_cachedHistoryJson;

String g_cachedWifiScanJson = "[]";
unsigned long g_lastScanStartMs = 0;
unsigned long g_lastScanFinishMs = 0;
bool g_isWifiScanInProgress = false;

static const unsigned long kWifiScanRefreshMs = 15000UL;
static const unsigned long kWifiScanTimeoutMs = 12000UL;
const IPAddress kApIpAddress(192, 168, 4, 1);
const IPAddress kApGateway(192, 168, 4, 1);
const IPAddress kApSubnet(255, 255, 255, 0);

struct SavedWifiCredentials {
  String ssid;
  String password;
};

struct UserLightState {
  bool isOn;
  uint8_t brightnessPercent;
};

UserLightState g_userLightState = {false, 50};

// ===== Small helpers =====

String escapeJsonString(const String &text) {
  String escapedText;
  escapedText.reserve(text.length() + 8);

  for (size_t index = 0; index < text.length(); ++index) {
    const char currentChar = text[index];

    switch (currentChar) {
      case '\"': escapedText += "\\\""; break;
      case '\\': escapedText += "\\\\"; break;
      case '\n': escapedText += "\\n"; break;
      case '\r': escapedText += "\\r"; break;
      case '\t': escapedText += "\\t"; break;
      default: escapedText += currentChar; break;
    }
  }

  return escapedText;
}

String escapeHtmlText(const String &text) {
  String escapedText;
  escapedText.reserve(text.length() + 8);

  for (size_t index = 0; index < text.length(); ++index) {
    const char currentChar = text[index];

    switch (currentChar) {
      case '&': escapedText += "&amp;"; break;
      case '<': escapedText += "&lt;"; break;
      case '>': escapedText += "&gt;"; break;
      case '\"': escapedText += "&quot;"; break;
      case '\'': escapedText += "&#39;"; break;
      default: escapedText += currentChar; break;
    }
  }

  return escapedText;
}

String getWifiStatusText() {
  if (g_isStaConnecting) return "STA CONNECTING";
  if (g_isApModeActive) return "AP MODE";
  if (WiFi.status() == WL_CONNECTED) return "STA CONNECTED";
  return "DISCONNECTED";
}

String getApIpText() {
  const wifi_mode_t wifiMode = WiFi.getMode();
  if (wifiMode == WIFI_AP || wifiMode == WIFI_AP_STA) {
    return WiFi.softAPIP().toString();
  }
  return "-";
}

String getStaIpText() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return "-";
}

int getWifiQualityPercent(int32_t rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

String getTinyMlLabel() {
  if (!g_hasTinyMlResult) return "COLLECTING";
  return g_latestTinyMlResult.isRain ? "RAIN" : "SUNNY";
}

String getTinyMlProbabilityText() {
  if (!g_hasTinyMlResult) return "--";
  return String(g_latestTinyMlResult.rainProbability * 100.0f, 1);
}

// ===== History helpers =====

void appendSensorHistory(float temperature, float humidity) {
  g_temperatureHistory[g_historyHeadIndex] = temperature;
  g_humidityHistory[g_historyHeadIndex] = humidity;
  g_historyHeadIndex = (g_historyHeadIndex + 1) % kHistorySize;

  if (g_historyItemCount < kHistorySize) {
    g_historyItemCount++;
  }

  g_historyVersion++;
}

String buildHistoryArrayJson(const float *historyBuffer) {
  String json = "[";

  for (int historyIndex = 0; historyIndex < g_historyItemCount; ++historyIndex) {
    const int bufferIndex =
      (g_historyHeadIndex - g_historyItemCount + historyIndex + kHistorySize) % kHistorySize;

    if (historyIndex > 0) {
      json += ",";
    }

    json += String(historyBuffer[bufferIndex], 2);
  }

  json += "]";
  return json;
}

String buildHistoryJson() {
  if (g_cachedHistoryVersion == g_historyVersion && !g_cachedHistoryJson.isEmpty()) {
    return g_cachedHistoryJson;
  }

  String json = "{";
  json += "\"historyVersion\":" + String(g_historyVersion) + ",";
  json += "\"tempHistory\":" + buildHistoryArrayJson(g_temperatureHistory) + ",";
  json += "\"humHistory\":" + buildHistoryArrayJson(g_humidityHistory);
  json += "}";

  g_cachedHistoryJson = json;
  g_cachedHistoryVersion = g_historyVersion;
  return g_cachedHistoryJson;
}

// ===== User light helpers =====

uint32_t convertBrightnessPercentToDuty(uint8_t brightnessPercent) {
  const uint32_t maxDuty = (1u << USER_LED_PWM_RESOLUTION) - 1u;
  return static_cast<uint32_t>((brightnessPercent * maxDuty) / 100u);
}

void applyUserLightState() {
  const uint32_t duty =
    g_userLightState.isOn ? convertBrightnessPercentToDuty(g_userLightState.brightnessPercent) : 0;

  ledcWrite(USER_LED_PWM_CHANNEL, duty);
}

void loadUserLightPreferences() {
  g_preferences.begin(WIFI_STORE_NAMESPACE, true);
  const bool savedLightState = g_preferences.getBool("usr_led_on", false);
  int savedBrightnessPercent = g_preferences.getInt("usr_led_pct", 50);
  g_preferences.end();

  if (savedBrightnessPercent < 0) savedBrightnessPercent = 0;
  if (savedBrightnessPercent > 100) savedBrightnessPercent = 100;

  g_userLightState.isOn = savedLightState;
  g_userLightState.brightnessPercent = static_cast<uint8_t>(savedBrightnessPercent);
}

void saveUserLightPreferences() {
  g_preferences.begin(WIFI_STORE_NAMESPACE, false);
  g_preferences.putBool("usr_led_on", g_userLightState.isOn);
  g_preferences.putInt("usr_led_pct", g_userLightState.brightnessPercent);
  g_preferences.end();
}

void initializeUserLight() {
  pinMode(USER_LED_GPIO, OUTPUT);
  ledcSetup(USER_LED_PWM_CHANNEL, USER_LED_PWM_FREQ, USER_LED_PWM_RESOLUTION);
  ledcAttachPin(USER_LED_GPIO, USER_LED_PWM_CHANNEL);
  loadUserLightPreferences();
  applyUserLightState();
}

// ===== Preferences helpers =====

int getSavedWifiCount() {
  g_preferences.begin(WIFI_STORE_NAMESPACE, true);
  int savedWifiCount = g_preferences.getInt("count", 0);
  g_preferences.end();

  if (savedWifiCount < 0) savedWifiCount = 0;
  if (savedWifiCount > WIFI_MAX_SAVED) savedWifiCount = WIFI_MAX_SAVED;
  return savedWifiCount;
}

SavedWifiCredentials readSavedWifiAt(int index) {
  SavedWifiCredentials savedWifi;

  g_preferences.begin(WIFI_STORE_NAMESPACE, true);
  savedWifi.ssid = g_preferences.getString(("ssid" + String(index)).c_str(), "");
  savedWifi.password = g_preferences.getString(("pass" + String(index)).c_str(), "");
  g_preferences.end();

  return savedWifi;
}

void writeSavedWifiAt(int index, const SavedWifiCredentials &savedWifi) {
  g_preferences.begin(WIFI_STORE_NAMESPACE, false);
  g_preferences.putString(("ssid" + String(index)).c_str(), savedWifi.ssid);
  g_preferences.putString(("pass" + String(index)).c_str(), savedWifi.password);
  g_preferences.end();
}

void setSavedWifiCount(int count) {
  g_preferences.begin(WIFI_STORE_NAMESPACE, false);
  g_preferences.putInt("count", count);
  g_preferences.end();
}

void clearSavedWifiPreferences() {
  g_preferences.begin(WIFI_STORE_NAMESPACE, false);
  g_preferences.clear();
  g_preferences.end();
}

bool loadMostRecentWifi(String &ssid, String &password) {
  const int savedWifiCount = getSavedWifiCount();
  if (savedWifiCount <= 0) {
    return false;
  }

  const SavedWifiCredentials latestSavedWifi = readSavedWifiAt(0);
  ssid = latestSavedWifi.ssid;
  password = latestSavedWifi.password;
  return !ssid.isEmpty();
}

void saveRecentWifiCredentials(const String &ssid, const String &password) {
  if (ssid.isEmpty()) {
    return;
  }

  SavedWifiCredentials savedWifiList[WIFI_MAX_SAVED];
  const int savedWifiCount = getSavedWifiCount();

  for (int index = 0; index < savedWifiCount; ++index) {
    savedWifiList[index] = readSavedWifiAt(index);
  }

  SavedWifiCredentials reorderedWifiList[WIFI_MAX_SAVED];
  int reorderedCount = 0;
  reorderedWifiList[reorderedCount++] = {ssid, password};

  for (int index = 0; index < savedWifiCount && reorderedCount < WIFI_MAX_SAVED; ++index) {
    if (savedWifiList[index].ssid != ssid && !savedWifiList[index].ssid.isEmpty()) {
      reorderedWifiList[reorderedCount++] = savedWifiList[index];
    }
  }

  for (int index = 0; index < reorderedCount; ++index) {
    writeSavedWifiAt(index, reorderedWifiList[index]);
  }

  g_preferences.begin(WIFI_STORE_NAMESPACE, false);
  for (int index = reorderedCount; index < WIFI_MAX_SAVED; ++index) {
    const String ssidKey = "ssid" + String(index);
    const String passwordKey = "pass" + String(index);

    if (g_preferences.isKey(ssidKey.c_str())) {
      g_preferences.remove(ssidKey.c_str());
    }

    if (g_preferences.isKey(passwordKey.c_str())) {
      g_preferences.remove(passwordKey.c_str());
    }
  }
  g_preferences.end();

  setSavedWifiCount(reorderedCount);
}

void loadCoreIotPreferencesAndApply() {
  g_preferences.begin(WIFI_STORE_NAMESPACE, true);
  const bool isCoreIotEnabled = g_preferences.getBool(kCoreIotEnabledPrefKey, true);
  String brokerHost = g_preferences.getString(kCoreIotHostPrefKey, "");
  String username = g_preferences.getString(kCoreIotUserPrefKey, "");
  String password = g_preferences.getString(kCoreIotPassPrefKey, "");
  g_preferences.end();

  coreiot_set_publish_enabled(isCoreIotEnabled);

  brokerHost.trim();
  username.trim();

  if (!brokerHost.isEmpty()) {
    coreiot_set_broker_host(brokerHost.c_str());
  }

  if (!username.isEmpty()) {
    coreiot_set_credentials(username.c_str(), password.c_str());
  }
}

void saveCoreIotEnabledPreference(bool isEnabled) {
  g_preferences.begin(WIFI_STORE_NAMESPACE, false);
  g_preferences.putBool(kCoreIotEnabledPrefKey, isEnabled);
  g_preferences.end();
}

void saveCoreIotConnectionPreferences(const String &host, const String &username, const String &password) {
  g_preferences.begin(WIFI_STORE_NAMESPACE, false);
  g_preferences.putString(kCoreIotHostPrefKey, host);
  g_preferences.putString(kCoreIotUserPrefKey, username);
  g_preferences.putString(kCoreIotPassPrefKey, password);
  g_preferences.end();
}

String buildSavedWifiJson() {
  const int savedWifiCount = getSavedWifiCount();
  String json = "[";

  for (int index = 0; index < savedWifiCount; ++index) {
    const SavedWifiCredentials savedWifi = readSavedWifiAt(index);

    if (index > 0) {
      json += ",";
    }

    json += "{";
    json += "\"ssid\":\"" + escapeJsonString(savedWifi.ssid) + "\",";
    json += "\"pass\":\"" + escapeJsonString(savedWifi.password) + "\"";
    json += "}";
  }

  json += "]";
  return json;
}

// ===== Wi-Fi scan helpers =====

String buildWifiScanJsonFromResults(int networkCount) {
  String json = "[";

  for (int networkIndex = 0; networkIndex < networkCount; ++networkIndex) {
    if (networkIndex > 0) {
      json += ",";
    }

    String encryptionText;
    const wifi_auth_mode_t authMode = WiFi.encryptionType(networkIndex);

    switch (authMode) {
      case WIFI_AUTH_OPEN: encryptionText = "OPEN"; break;
      case WIFI_AUTH_WEP: encryptionText = "WEP"; break;
      case WIFI_AUTH_WPA_PSK: encryptionText = "WPA"; break;
      case WIFI_AUTH_WPA2_PSK: encryptionText = "WPA2"; break;
      case WIFI_AUTH_WPA_WPA2_PSK: encryptionText = "WPA/WPA2"; break;
      case WIFI_AUTH_WPA2_ENTERPRISE: encryptionText = "WPA2-ENT"; break;
      case WIFI_AUTH_WPA3_PSK: encryptionText = "WPA3"; break;
      case WIFI_AUTH_WPA2_WPA3_PSK: encryptionText = "WPA2/WPA3"; break;
      default: encryptionText = "UNKNOWN"; break;
    }

    json += "{";
    json += "\"ssid\":\"" + escapeJsonString(WiFi.SSID(networkIndex)) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(networkIndex)) + ",";
    json += "\"quality\":" + String(getWifiQualityPercent(WiFi.RSSI(networkIndex))) + ",";
    json += "\"enc\":\"" + encryptionText + "\"";
    json += "}";
  }

  json += "]";
  return json;
}

void startWifiScanAsync() {
  if (g_isWifiScanInProgress) {
    return;
  }

  const int scanState = WiFi.scanComplete();
  if (scanState == WIFI_SCAN_RUNNING) {
    g_isWifiScanInProgress = true;
    return;
  }

  if (scanState >= 0) {
    WiFi.scanDelete();
  }

  const int startResult = WiFi.scanNetworks(true, true);
  if (startResult == WIFI_SCAN_RUNNING) {
    g_isWifiScanInProgress = true;
    g_lastScanStartMs = millis();
    Serial.println("[Web] Async Wi-Fi scan started");
  } else {
    g_isWifiScanInProgress = false;
    Serial.printf("[Web] Failed to start async Wi-Fi scan, rc=%d\n", startResult);
  }
}

void updateWifiScanCache() {
  const int scanState = WiFi.scanComplete();

  if (scanState == WIFI_SCAN_RUNNING) {
    g_isWifiScanInProgress = true;
    return;
  }

  if (scanState == WIFI_SCAN_FAILED) {
    if (g_isWifiScanInProgress) {
      Serial.println("[Web] Async Wi-Fi scan failed");
    }
    g_isWifiScanInProgress = false;
    return;
  }

  if (scanState >= 0) {
    g_cachedWifiScanJson = buildWifiScanJsonFromResults(scanState);
    g_lastScanFinishMs = millis();
    g_isWifiScanInProgress = false;
    WiFi.scanDelete();
    Serial.printf("[Web] Async Wi-Fi scan cached %d network(s)\n", scanState);
  }
}

void processWifiScan() {
  updateWifiScanCache();

  if (!g_isApModeActive) {
    if (g_isWifiScanInProgress && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
      g_isWifiScanInProgress = false;
    }
    return;
  }

  const unsigned long nowMs = millis();

  if (g_isWifiScanInProgress && (nowMs - g_lastScanStartMs >= kWifiScanTimeoutMs)) {
    // Warning: a stuck scan blocks further refreshes in AP mode.
    // Resetting the scan state here prevents the setup page from appearing "dead" forever.
    Serial.println("[Web] Async Wi-Fi scan timeout, restarting");
    WiFi.scanDelete();
    g_isWifiScanInProgress = false;
  }

  const bool isCacheExpired =
    g_lastScanFinishMs == 0 || (nowMs - g_lastScanFinishMs >= kWifiScanRefreshMs);

  if (isCacheExpired && !g_isWifiScanInProgress) {
    startWifiScanAsync();
  }
}

String buildWifiScanJson() {
  updateWifiScanCache();

  if (g_isApModeActive && !g_isWifiScanInProgress && g_cachedWifiScanJson == "[]") {
    startWifiScanAsync();
  }

  return g_cachedWifiScanJson;
}

// ===== Embedded page helpers =====

void prepareCommonResponseHeaders() {
  // Warning: keeping HTTP connections open for too long can exhaust the small
  // socket pool on ESP32, especially when desktop browsers open parallel probes.
  g_webServer.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  g_webServer.sendHeader("Pragma", "no-cache");
  g_webServer.sendHeader("Expires", "0");
  g_webServer.sendHeader("Connection", "close");
}

void sendEmbeddedPage(const char *htmlPage, int statusCode = 200) {
  prepareCommonResponseHeaders();
  g_webServer.send_P(statusCode, "text/html; charset=utf-8", htmlPage);
}

void sendTextResponse(int statusCode, const char *contentType, const String &payload) {
  prepareCommonResponseHeaders();
  g_webServer.send(statusCode, contentType, payload);
}

void sendTextResponse(int statusCode, const char *contentType, const char *payload) {
  prepareCommonResponseHeaders();
  g_webServer.send(statusCode, contentType, payload);
}

void sendJsonResponse(int statusCode, const String &payload) {
  sendTextResponse(statusCode, "application/json", payload);
}

void sendJsonResponse(int statusCode, const char *payload) {
  sendTextResponse(statusCode, "application/json", payload);
}

void redirectToRoot() {
  prepareCommonResponseHeaders();
  g_webServer.sendHeader("Location", "/", true);
  g_webServer.send(302, "text/plain", "Redirect");
}

String buildConnectPage(const String &ssid) {
  String html = FPSTR(kConnectStatusPage);
  html.replace("{{SSID}}", escapeHtmlText(ssid));
  return html;
}

// ===== Wi-Fi / time flow helpers =====

void ensureServerStarted() {
  if (!g_hasServerStarted) {
    g_webServer.begin();
    g_hasServerStarted = true;
  }
}

void notifyInternetReady() {
  if (g_appContext == nullptr || g_appContext->internetSemaphore == nullptr) {
    Serial.println("[Web] Cannot signal internet semaphore: invalid context");
    return;
  }

  const BaseType_t giveResult = xSemaphoreGive(g_appContext->internetSemaphore);
  Serial.printf("[Web] internetSemaphore -> %s\n", giveResult == pdTRUE ? "GIVE OK" : "GIVE FAIL");
}

void trySyncNtp(bool forceNow) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  const unsigned long nowMs = millis();
  if (!forceNow && g_hasSyncedNtp) {
    return;
  }

  if (!forceNow && (nowMs - g_lastNtpAttemptMs < kNtpRetryIntervalMs)) {
    return;
  }

  g_lastNtpAttemptMs = nowMs;

  const uint32_t epochBeforeSync = static_cast<uint32_t>(time(nullptr));
  Serial.printf("[Time] NTP try | force=%d | epoch_before=%lu\n",
                forceNow ? 1 : 0,
                static_cast<unsigned long>(epochBeforeSync));

  const bool syncResult = appTimeSyncNtp(
    kNtpServer,
    kNtpGmtOffsetSec,
    kNtpDaylightOffsetSec,
    1704067200U,
    15000U,
    250U
  );

  g_hasSyncedNtp = syncResult;

  const uint32_t epochAfterSync = static_cast<uint32_t>(time(nullptr));
  Serial.printf("[Time] NTP sync -> %s | epoch_after=%lu\n",
                syncResult ? "OK" : "FAIL",
                static_cast<unsigned long>(epochAfterSync));
}

void startApMode() {
  WiFi.scanDelete();
  g_isWifiScanInProgress = false;

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  delay(200);

  WiFi.mode(WIFI_AP);
  delay(100);
  WiFi.softAPConfig(kApIpAddress, kApGateway, kApSubnet);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  g_isApModeActive = true;
  g_isStaConnecting = false;
  g_staConnectStartMs = 0;
  g_staIpAddress = "";
  g_targetStaSsid = "";

  app_set_wifi_connected(g_appContext, false);
  g_lastWifiStatus = WiFi.status();

  ensureServerStarted();
  startWifiScanAsync();

  Serial.printf("[Web] AP mode started | mode=%d | status=%d | AP IP=%s\n",
                WiFi.getMode(),
                WiFi.status(),
                WiFi.softAPIP().toString().c_str());
}

void startStaConnect(const String &ssid, const String &password) {
  WiFi.scanDelete();
  g_isWifiScanInProgress = false;

  WiFi.disconnect(true, true);
  delay(100);

  // Keep AP alive while STA is connecting so the setup page does not disappear
  // mid-request on phones/laptops that are still attached to the ESP32 AP.
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  WiFi.softAPConfig(kApIpAddress, kApGateway, kApSubnet);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(100);
  WiFi.begin(ssid.c_str(), password.c_str());

  g_isApModeActive = true;
  g_isStaConnecting = true;
  g_staConnectStartMs = millis();
  g_targetStaSsid = ssid;
  g_staIpAddress = "";

  app_set_wifi_connected(g_appContext, false);
  g_lastWifiStatus = WiFi.status();

  ensureServerStarted();

  Serial.printf("[Web] STA connect start | SSID=%s | mode=%d | status=%d\n",
                ssid.c_str(),
                WiFi.getMode(),
                WiFi.status());
}

// ===== API payload helpers =====

String buildStateJson() {
  StaticJsonDocument<416> stateDocument;
  stateDocument["wifiStatus"] = getWifiStatusText();
  stateDocument["apSsid"] = AP_SSID;
  stateDocument["apIp"] = getApIpText();
  stateDocument["staIp"] = getStaIpText();
  stateDocument["staSsid"] = g_targetStaSsid;
  stateDocument["temperatureText"] =
    isnan(g_latestSensorData.temperature) ? "--" : String(g_latestSensorData.temperature, 1);
  stateDocument["humidityText"] =
    isnan(g_latestSensorData.humidity) ? "--" : String(g_latestSensorData.humidity, 1);
  stateDocument["tinyLabel"] = getTinyMlLabel();
  stateDocument["tinyProbability"] = getTinyMlProbabilityText();
  stateDocument["userLightOn"] = g_userLightState.isOn;
  stateDocument["userLightBrightness"] = g_userLightState.brightnessPercent;
  stateDocument["coreiotMqtt"] = app_get_coreiot_mqtt_connected(g_appContext);
  stateDocument["coreiotRetrySec"] = app_get_coreiot_retry_sec(g_appContext);
  stateDocument["historyVersion"] = g_historyVersion;

  String json;
  json.reserve(352);
  serializeJson(stateDocument, json);
  return json;
}

// ===== HTTP handlers =====

void handleApiHistory() {
  sendJsonResponse(200, buildHistoryJson());
}

void handleRootPage() {
  sendEmbeddedPage((g_isApModeActive || g_isStaConnecting) ? kApDashboardPage : kStaDashboardPage);
}

void handleSettingsPage() {
  if (!g_isApModeActive && !g_isStaConnecting) {
    redirectToRoot();
    return;
  }

  sendEmbeddedPage(kSettingsPage);
}

void handleApiState() {
  sendJsonResponse(200, buildStateJson());
}

void handleApiScan() {
  sendJsonResponse(200, buildWifiScanJson());
}

void handleApiSaved() {
  sendJsonResponse(200, buildSavedWifiJson());
}

void handleApiCoreIotConfigGet() {
  char host[64] = {0};
  char username[80] = {0};
  char password[80] = {0};

  coreiot_get_broker_host(host, sizeof(host));
  coreiot_get_credentials(username, sizeof(username), password, sizeof(password));

  StaticJsonDocument<320> configDocument;
  configDocument["enabled"] = coreiot_get_publish_enabled();
  configDocument["host"] = host;
  configDocument["username"] = username;
  configDocument["password"] = password;

  String payload;
  serializeJson(configDocument, payload);
  sendJsonResponse(200, payload);
}

void handleApiCoreIotConfigPost() {
  bool isCoreIotEnabled = coreiot_get_publish_enabled();

  if (g_webServer.hasArg("enabled")) {
    String enabledArg = g_webServer.arg("enabled");
    enabledArg.trim();
    enabledArg.toLowerCase();
    isCoreIotEnabled =
      enabledArg == "1" || enabledArg == "true" || enabledArg == "on" || enabledArg == "yes";
  }

  String host;
  String username;
  String password;

  if (g_webServer.hasArg("host")) {
    host = g_webServer.arg("host");
    host.trim();
  }

  if (g_webServer.hasArg("username")) {
    username = g_webServer.arg("username");
    username.trim();
  }

  if (g_webServer.hasArg("password")) {
    password = g_webServer.arg("password");
  }

  if (!host.isEmpty() && !coreiot_set_broker_host(host.c_str())) {
    sendJsonResponse(400, "{\"ok\":false,\"reason\":\"invalid_host\"}");
    return;
  }

  if (!username.isEmpty()) {
    if (!coreiot_set_credentials(username.c_str(), password.c_str())) {
      sendJsonResponse(400, "{\"ok\":false,\"reason\":\"invalid_credentials\"}");
      return;
    }
  }

  coreiot_set_publish_enabled(isCoreIotEnabled);
  saveCoreIotConnectionPreferences(host, username, password);
  saveCoreIotEnabledPreference(isCoreIotEnabled);

  sendJsonResponse(200, "{\"ok\":true}");
}

void handleConnectRequest() {
  String ssid = g_webServer.arg("ssid");
  String password = g_webServer.arg("password");
  bool shouldSaveCredentials = true;
  ssid.trim();

  if (g_webServer.hasArg("save")) {
    String saveArg = g_webServer.arg("save");
    saveArg.trim();
    saveArg.toLowerCase();
    shouldSaveCredentials =
      saveArg == "1" || saveArg == "true" || saveArg == "on" || saveArg == "yes";
  }

  if (ssid.isEmpty()) {
    sendTextResponse(400, "text/plain", "SSID is required.");
    return;
  }

  // Only persist credentials when the user explicitly chooses the save flow.
  if (shouldSaveCredentials) {
    saveRecentWifiCredentials(ssid, password);
  }

  startStaConnect(ssid, password);
  sendTextResponse(200, "text/html; charset=utf-8", buildConnectPage(ssid));
}

void handleForgetRequest() {
  clearSavedWifiPreferences();
  sendJsonResponse(200, "{\"ok\":true}");
}

void handleToggleLightRequest() {
  g_userLightState.isOn = !g_userLightState.isOn;
  applyUserLightState();
  saveUserLightPreferences();

  Serial.printf("[LED] Toggle: %s | brightness=%d%%\n",
                g_userLightState.isOn ? "ON" : "OFF",
                g_userLightState.brightnessPercent);

  sendJsonResponse(200, "{\"ok\":true}");
}

void handleBrightnessRequest() {
  int brightnessPercent = g_webServer.arg("value").toInt();

  if (brightnessPercent < 0) brightnessPercent = 0;
  if (brightnessPercent > 100) brightnessPercent = 100;

  g_userLightState.brightnessPercent = static_cast<uint8_t>(brightnessPercent);
  applyUserLightState();
  saveUserLightPreferences();

  Serial.printf("[LED] Brightness set to %d%%\n", g_userLightState.brightnessPercent);
  sendJsonResponse(200, "{\"ok\":true}");
}

void handleCaptivePortalProbe() {
  redirectToRoot();
}

void setupRoutes() {
  g_webServer.on("/", HTTP_GET, handleRootPage);
  g_webServer.on("/settings", HTTP_GET, handleSettingsPage);
  g_webServer.on("/generate_204", HTTP_GET, handleCaptivePortalProbe);
  g_webServer.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortalProbe);
  g_webServer.on("/connecttest.txt", HTTP_GET, handleCaptivePortalProbe);
  g_webServer.on("/ncsi.txt", HTTP_GET, handleCaptivePortalProbe);
  g_webServer.on("/fwlink", HTTP_GET, handleCaptivePortalProbe);
  g_webServer.on("/api/state", HTTP_GET, handleApiState);
  g_webServer.on("/api/history", HTTP_GET, handleApiHistory);
  g_webServer.on("/api/coreiot/config", HTTP_GET, handleApiCoreIotConfigGet);
  g_webServer.on("/api/coreiot/config", HTTP_POST, handleApiCoreIotConfigPost);
  g_webServer.on("/api/scan", HTTP_GET, handleApiScan);
  g_webServer.on("/api/saved", HTTP_GET, handleApiSaved);
  g_webServer.on("/connect", HTTP_POST, handleConnectRequest);
  g_webServer.on("/forget", HTTP_POST, handleForgetRequest);
  g_webServer.on("/api/light/toggle", HTTP_POST, handleToggleLightRequest);
  g_webServer.on("/api/light/brightness", HTTP_POST, handleBrightnessRequest);
  g_webServer.onNotFound([]() {
    if (g_webServer.uri() == "/favicon.ico") {
      sendTextResponse(204, "text/plain", "");
      return;
    }

    if (g_isApModeActive && g_webServer.method() == HTTP_GET) {
      redirectToRoot();
      return;
    }

    sendJsonResponse(404, "{\"ok\":false,\"reason\":\"not_found\"}");
  });
}

// ===== Background processing helpers =====

void processIncomingQueues() {
  if (g_appContext == nullptr) {
    return;
  }

  sensor_data_t incomingSensorData{};

  if (g_appContext->webQueue != nullptr) {
    while (xQueueReceive(g_appContext->webQueue, &incomingSensorData, 0) == pdTRUE) {
      g_latestSensorData = incomingSensorData;

      if (!isnan(incomingSensorData.temperature) && !isnan(incomingSensorData.humidity)) {
        appendSensorHistory(incomingSensorData.temperature, incomingSensorData.humidity);
      }
    }
  }

  if (g_appContext->tinyResultQueue != nullptr) {
    tinyml_result_t queuedTinyMlResult{};
    if (xQueuePeek(g_appContext->tinyResultQueue, &queuedTinyMlResult, 0) == pdTRUE) {
      g_latestTinyMlResult = queuedTinyMlResult;
      g_hasTinyMlResult = true;
    }
  }
}

void processWifiState() {
  const wl_status_t currentWifiStatus = WiFi.status();

  if (currentWifiStatus != g_lastWifiStatus) {
    Serial.printf("[Web] WiFi status changed: %d -> %d | mode=%d\n",
                  g_lastWifiStatus,
                  currentWifiStatus,
                  WiFi.getMode());
    g_lastWifiStatus = currentWifiStatus;
  }

  if (g_isStaConnecting && currentWifiStatus == WL_CONNECTED) {
    g_isStaConnecting = false;
    g_staIpAddress = WiFi.localIP().toString();

    app_set_wifi_connected(g_appContext, true);
    notifyInternetReady();

    // Close the temporary AP after STA succeeds so the board returns to the
    // same final behavior as before: one active infrastructure connection.
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_isApModeActive = false;

    Serial.printf("[Web] STA connected | SSID=%s | IP=%s | RSSI=%d\n",
                  WiFi.SSID().c_str(),
                  g_staIpAddress.c_str(),
                  WiFi.RSSI());
    Serial.printf("[Web] Open STA page at: http://%s/\n", g_staIpAddress.c_str());

    trySyncNtp(true);
  }

  if (!g_isApModeActive && !g_isStaConnecting && currentWifiStatus != WL_CONNECTED) {
    app_set_wifi_connected(g_appContext, false);
    g_hasSyncedNtp = false;
  }

  if (g_isStaConnecting && (millis() - g_staConnectStartMs >= STA_CONNECT_TIMEOUT)) {
    // Warning: removing this fallback can trap the board in a failed STA attempt
    // with no setup UI reachable from the phone/laptop anymore.
    Serial.printf("[Web] STA connect timeout -> back to AP | last status=%d\n", WiFi.status());
    startApMode();
  }

  if (!g_isApModeActive && !g_isStaConnecting && currentWifiStatus == WL_CONNECTED) {
    trySyncNtp(false);
  }
}

void processBootButton() {
  static int lastRawReading = HIGH;
  static int stableReading = HIGH;
  static unsigned long lastDebounceStartMs = 0;
  const unsigned long debounceDelayMs = 50;

  const int currentReading = digitalRead(BOOT_PIN);

  if (currentReading != lastRawReading) {
    lastDebounceStartMs = millis();
  }

  if ((millis() - lastDebounceStartMs) > debounceDelayMs) {
    if (currentReading != stableReading) {
      stableReading = currentReading;

      if (stableReading == LOW) {
        Serial.println("[Web] BOOT pressed -> AP mode");
        startApMode();
      }
    }
  }

  lastRawReading = currentReading;
}

}  // namespace

void main_server_task(void *pvParameters) {
  g_appContext = static_cast<app_context_t *>(pvParameters);
  if (g_appContext == nullptr || g_appContext->webQueue == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  pinMode(BOOT_PIN, INPUT_PULLUP);
  Serial.println("[Web] Embedded web pages mode enabled (LittleFS not required)");

  initializeUserLight();
  loadCoreIotPreferencesAndApply();
  setupRoutes();

  String savedSsid;
  String savedPassword;

  if (loadMostRecentWifi(savedSsid, savedPassword)) {
    startStaConnect(savedSsid, savedPassword);
  } else {
    startApMode();
  }

  while (true) {
    processIncomingQueues();
    processWifiState();
    processWifiScan();
    processBootButton();
    g_webServer.handleClient();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
