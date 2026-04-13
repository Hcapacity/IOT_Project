#include "coreiot.h"
#include "app_time_utils.h"
#include <Preferences.h>
#include <math.h>
#include <string.h>

// Default fallback values (used when no config saved yet)
static const char* kDefaultCoreIotServer = "app.coreiot.io";
static const char* kDefaultAccessToken = "h61qp4bfrpbsp7iy5x2c";
static const char* kDefaultClientId = "ESP32Client";
static const uint16_t kDefaultMqttPort = 1883;

// Time config
constexpr int32_t kGmtOffsetSec = 7 * 3600;
constexpr int32_t kDaylightOffsetSec = 0;

// NVS namespaces and keys
static const char* kMqttCfgNs = "coreiot_cfg";
static const char* kBufNs = "coreiot_buf";

// Auth modes for MQTT
// 0: Access token only -> username=accessToken, password=""
// 1: User token + Access token -> username=userToken, password=accessToken
static const uint8_t kAuthAccessTokenOnly = 0;
static const uint8_t kAuthUserAndAccessToken = 1;

// Offline buffer settings
static const int kOfflineMaxItems = 40;
static const int kFlushPerLoop = 3;

WiFiClient espClient;
PubSubClient client(espClient);

struct MqttRuntimeConfig {
  String host;
  uint16_t port;
  uint8_t authMode;
  String clientId;
  String userToken;
  String accessToken;
};

static MqttRuntimeConfig g_cfg;
static uint32_t g_lastReconnectAttemptMs = 0;
static uint32_t g_lastCfgPollMs = 0;

static float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

static const char* rain_status_short(float p01) {
  if (p01 < 0.2f) return "Dry";
  if (p01 < 0.4f) return "Low";
  if (p01 < 0.6f) return "Maybe";
  if (p01 < 0.8f) return "Rain";
  return "Storm";
}

static bool sameConfig(const MqttRuntimeConfig& a, const MqttRuntimeConfig& b) {
  return a.host == b.host &&
         a.port == b.port &&
         a.authMode == b.authMode &&
         a.clientId == b.clientId &&
         a.userToken == b.userToken &&
         a.accessToken == b.accessToken;
}

static MqttRuntimeConfig loadMqttConfigFromNvs() {
  Preferences prefs;
  MqttRuntimeConfig cfg;

  if (!prefs.begin(kMqttCfgNs, true)) {
    cfg.host = kDefaultCoreIotServer;
    cfg.port = kDefaultMqttPort;
    cfg.authMode = kAuthAccessTokenOnly;
    cfg.clientId = kDefaultClientId;
    cfg.userToken = "";
    cfg.accessToken = kDefaultAccessToken;
    return cfg;
  }

  cfg.host = prefs.getString("host", kDefaultCoreIotServer);
  cfg.port = static_cast<uint16_t>(prefs.getUShort("port", kDefaultMqttPort));
  cfg.authMode = static_cast<uint8_t>(prefs.getUChar("auth", kAuthAccessTokenOnly));
  cfg.clientId = prefs.getString("cid", kDefaultClientId);
  cfg.userToken = prefs.getString("user", "");
  cfg.accessToken = prefs.getString("atok", kDefaultAccessToken);
  prefs.end();

  if (cfg.host.isEmpty()) cfg.host = kDefaultCoreIotServer;
  if (cfg.port == 0) cfg.port = kDefaultMqttPort;
  if (cfg.clientId.isEmpty()) cfg.clientId = kDefaultClientId;
  if (cfg.authMode != kAuthAccessTokenOnly && cfg.authMode != kAuthUserAndAccessToken) {
    cfg.authMode = kAuthAccessTokenOnly;
  }
  return cfg;
}

static void applyConfigToClient(const MqttRuntimeConfig& cfg) {
  client.setServer(cfg.host.c_str(), cfg.port);
}

static bool connectMqttWithConfig(const MqttRuntimeConfig& cfg) {
  if (cfg.accessToken.isEmpty()) {
    Serial.println("[CoreIoT] MQTT access token empty, cannot connect.");
    return false;
  }

  Serial.print("[CoreIoT] Connecting MQTT to ");
  Serial.print(cfg.host);
  Serial.print(":");
  Serial.print(cfg.port);
  Serial.print(" mode=");
  Serial.println(cfg.authMode == kAuthAccessTokenOnly ? "ACCESS_TOKEN" : "USER+TOKEN");

  bool ok = false;
  if (cfg.authMode == kAuthUserAndAccessToken) {
    ok = client.connect(cfg.clientId.c_str(), cfg.userToken.c_str(), cfg.accessToken.c_str());
  } else {
    ok = client.connect(cfg.clientId.c_str(), cfg.accessToken.c_str(), "");
  }

  if (ok) {
    Serial.println("[CoreIoT] MQTT connected.");
    client.subscribe("v1/devices/me/rpc/request/+");
    Serial.println("[CoreIoT] Subscribed RPC topic.");
  } else {
    Serial.print("[CoreIoT] MQTT connect failed, rc=");
    Serial.println(client.state());
  }

  return ok;
}

static void reconnectMqttNonBlocking() {
  if (WiFi.status() != WL_CONNECTED) return;

  const uint32_t nowMs = millis();
  if (nowMs - g_lastReconnectAttemptMs < 5000UL) return;
  g_lastReconnectAttemptMs = nowMs;

  applyConfigToClient(g_cfg);
  connectMqttWithConfig(g_cfg);
}

static void refreshConfigIfChanged() {
  const uint32_t nowMs = millis();
  if (nowMs - g_lastCfgPollMs < 3000UL) return;
  g_lastCfgPollMs = nowMs;

  MqttRuntimeConfig latest = loadMqttConfigFromNvs();
  if (!sameConfig(latest, g_cfg)) {
    Serial.println("[CoreIoT] MQTT config changed in NVS, applying...");
    g_cfg = latest;
    applyConfigToClient(g_cfg);

    if (client.connected()) {
      client.disconnect();
    }
  }
}

static bool bufferReadMeta(Preferences& prefs, int& head, int& count) {
  head = prefs.getInt("head", 0);
  count = prefs.getInt("count", 0);

  if (head < 0 || head >= kOfflineMaxItems) head = 0;
  if (count < 0) count = 0;
  if (count > kOfflineMaxItems) count = kOfflineMaxItems;
  return true;
}

static void bufferWriteMeta(Preferences& prefs, int head, int count) {
  prefs.putInt("head", head);
  prefs.putInt("count", count);
}

static void slotKey(int idx, char* out, size_t outLen) {
  snprintf(out, outLen, "p%02d", idx);
}

static bool offlinePush(const char* payload) {
  if (payload == nullptr || payload[0] == '\0') return false;

  Preferences prefs;
  if (!prefs.begin(kBufNs, false)) return false;

  int head = 0, count = 0;
  bufferReadMeta(prefs, head, count);

  char key[8];
  slotKey(head, key, sizeof(key));
  prefs.putString(key, payload);

  head = (head + 1) % kOfflineMaxItems;
  if (count < kOfflineMaxItems) {
    count++;
  }

  bufferWriteMeta(prefs, head, count);
  prefs.end();
  return true;
}

static bool offlinePeekOldest(String& outPayload) {
  outPayload = "";
  Preferences prefs;
  if (!prefs.begin(kBufNs, true)) return false;

  int head = 0, count = 0;
  bufferReadMeta(prefs, head, count);
  if (count <= 0) {
    prefs.end();
    return false;
  }

  int oldest = (head - count + kOfflineMaxItems) % kOfflineMaxItems;
  char key[8];
  slotKey(oldest, key, sizeof(key));
  outPayload = prefs.getString(key, "");
  prefs.end();

  return !outPayload.isEmpty();
}

static bool offlinePopOldest() {
  Preferences prefs;
  if (!prefs.begin(kBufNs, false)) return false;

  int head = 0, count = 0;
  bufferReadMeta(prefs, head, count);
  if (count <= 0) {
    prefs.end();
    return false;
  }

  int oldest = (head - count + kOfflineMaxItems) % kOfflineMaxItems;
  char key[8];
  slotKey(oldest, key, sizeof(key));
  prefs.remove(key);

  count--;
  if (count < 0) count = 0;
  bufferWriteMeta(prefs, head, count);
  prefs.end();
  return true;
}

static int offlineCount() {
  Preferences prefs;
  if (!prefs.begin(kBufNs, true)) return 0;
  int count = prefs.getInt("count", 0);
  prefs.end();
  if (count < 0) count = 0;
  if (count > kOfflineMaxItems) count = kOfflineMaxItems;
  return count;
}

static void flushOfflineIfConnected() {
  if (WiFi.status() != WL_CONNECTED || !client.connected()) return;

  for (int i = 0; i < kFlushPerLoop; ++i) {
    String p;
    if (!offlinePeekOldest(p)) break;

    bool ok = client.publish("v1/devices/me/telemetry", p.c_str(), p.length());
    if (!ok) {
      Serial.println("[CoreIoT] Flush buffered payload failed, keep for retry.");
      break;
    }

    offlinePopOldest();
    Serial.print("[CoreIoT] Flushed 1 buffered payload. Remaining=");
    Serial.println(offlineCount());
  }
}

static bool publishOrBuffer(const char* payload, size_t n) {
  if (payload == nullptr || n == 0) return false;

  if (WiFi.status() == WL_CONNECTED && client.connected()) {
    bool ok = client.publish("v1/devices/me/telemetry", payload, n);
    if (ok) return true;
  }

  bool saved = offlinePush(payload);
  if (saved) {
    Serial.print("[CoreIoT] Stored offline payload. Queue=");
    Serial.println(offlineCount());
  } else {
    Serial.println("[CoreIoT] Failed to store offline payload.");
  }
  return false;
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.println("] ");

  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  Serial.print("Payload: ");
  Serial.println(message);

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }

  const char* method = doc["method"] | "";
  JsonVariant params = doc["params"];

  if (strcmp(method, "setValue") == 0 ||
      strcmp(method, "setValueLED") == 0 ||
      strcmp(method, "setStateLED") == 0) {
    bool turnOn = false;

    if (params.is<const char*>()) {
      const char* paramsText = params.as<const char*>();
      turnOn = (strcmp(paramsText, "ON") == 0) || (strcmp(paramsText, "1") == 0) || (strcmp(paramsText, "true") == 0);
    } else if (params.is<bool>()) {
      turnOn = params.as<bool>();
    } else if (params.is<int>() || params.is<long>()) {
      turnOn = params.as<int>() != 0;
    }

    if (turnOn) {
      Serial.println("Device turned ON.");
    } else {
      Serial.println("Device turned OFF.");
    }
  } else {
    Serial.print("Unknown method: ");
    Serial.println(method);
  }
}

static bool setup_coreiot(app_context_t *ctx) {
  if (ctx == nullptr || ctx->internetSemaphore == nullptr) {
    Serial.println("[CoreIoT] Invalid app context");
    return false;
  }

  Serial.println("[CoreIoT] Waiting internet...");
  xSemaphoreTake(ctx->internetSemaphore, portMAX_DELAY);
  Serial.println("[CoreIoT] Internet ready");

  g_cfg = loadMqttConfigFromNvs();
  applyConfigToClient(g_cfg);
  client.setCallback(callback);

  Serial.print("[CoreIoT] Boot config host=");
  Serial.print(g_cfg.host);
  Serial.print(" port=");
  Serial.print(g_cfg.port);
  Serial.print(" authMode=");
  Serial.println(g_cfg.authMode == kAuthAccessTokenOnly ? "ACCESS_TOKEN" : "USER+TOKEN");

  return true;
}

void coreiot_task(void *pvParameters) {
  app_context_t *ctx = static_cast<app_context_t *>(pvParameters);
  if (ctx == nullptr || ctx->coreQueue == nullptr || ctx->tinyResultQueue == nullptr) {
    Serial.println("[CoreIoT] Invalid context or queues");
    vTaskDelete(nullptr);
    return;
  }

  if (!setup_coreiot(ctx)) {
    vTaskDelete(nullptr);
    return;
  }

  sensor_data_t data{};
  tinyml_result_t mlResult{};
  bool hasMlResult = false;
  data.temperature = NAN;
  data.humidity = NAN;
  TickType_t lastPublishTick = xTaskGetTickCount();

  while (1) {
    refreshConfigIfChanged();

    if (!client.connected()) {
      reconnectMqttNonBlocking();
    } else {
      client.loop();
      flushOfflineIfConnected();
    }

    TickType_t now = xTaskGetTickCount();
    if (xQueuePeek(ctx->coreQueue, &data, 0) == pdTRUE) {
      if (xQueuePeek(ctx->tinyResultQueue, &mlResult, 0) == pdTRUE) {
        hasMlResult = true;
      }

      if ((now - lastPublishTick) >= pdMS_TO_TICKS(10000)) {
        lastPublishTick = now;

        if (!isnan(data.temperature) && !isnan(data.humidity)) {
          AppDateTime dt{};
          const bool hasEpoch = appTimeNow(kGmtOffsetSec, kDaylightOffsetSec, dt);

          StaticJsonDocument<224> doc;
          doc["temperature"] = data.temperature;
          doc["humidity"] = data.humidity;
          doc["ts"] = hasEpoch ? dt.epochSec : static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);

          float p01 = 0.0f;
          const char* pred = "NO_DATA";
          if (hasMlResult) {
            p01 = clamp01(mlResult.rainProbability);
            pred = mlResult.isRain ? "RAIN" : "SUNNY";
          }

          doc["rain_prob"] = p01;
          doc["rain_prob_pct"] = p01 * 100.0f;
          doc["rain_pred"] = pred;
          doc["rain_status_short"] = rain_status_short(p01);

          char payload[224];
          size_t n = serializeJson(doc, payload, sizeof(payload));

          bool sent = publishOrBuffer(payload, n);
          Serial.print("[CoreIoT] ");
          Serial.println(sent ? "Publish OK" : "Publish deferred to offline buffer");
          Serial.println(payload);
        } else {
          Serial.println("[CoreIoT] Sensor invalid, skip publish");
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}