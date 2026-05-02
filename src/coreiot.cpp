#include "coreiot.h"
#include "app_time_utils.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>

// ----------- CONFIGURE THESE! -----------
static const char *kDefaultBrokerHost = "127.0.0.1";
static const char *kDefaultMqttUsername = "";
static const char *kDefaultMqttPassword = "";
static const char *kDeviceId = "ESP32_S3_1";
static const int mqttPort = 1883;

constexpr int32_t kGmtOffsetSec   = 7 * 3600;
constexpr int32_t kDaylightOffsetSec = 0;

// RPC topic from CoreIOT
static const char *kRpcTopicSub = "v1/devices/me/rpc/request/+";
// Telemetry topic
static const char *kTelemetryTopic = "v1/devices/me/telemetry";
// Optional RPC response topic prefix
static const char *kRpcResponsePrefix = "v1/devices/me/rpc/response/";
// ----------------------------------------

namespace {

WiFiClient espClient;
PubSubClient client(espClient);

// giữ pointer tới app context để callback MQTT cũng đổi được state LCD
app_context_t *g_ctx = nullptr;

// trạng thái remote control cho LCD từ CoreIOT
bool g_remoteLcdForecastEnabled = false;
bool g_coreiotPublishEnabled = true;
char g_mqttHost[64] = {0};
char g_mqttUsername[80] = {0};
char g_mqttPassword[80] = {0};

// ----------------------------------------------------
// Utility
// ----------------------------------------------------
static float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

static const char *rain_status_short(float p01) {
  if (p01 < 0.2f) return "Dry";
  if (p01 < 0.4f) return "Low";
  if (p01 < 0.6f) return "Maybe";
  if (p01 < 0.8f) return "Rain";
  return "Storm";
}

static String boolText(bool value) {
  return value ? "true" : "false";
}

static bool parseBoolFlexible(JsonVariant v, bool &out) {
  if (v.is<bool>()) {
    out = v.as<bool>();
    return true;
  }

  if (v.is<int>()) {
    out = (v.as<int>() != 0);
    return true;
  }

  if (v.is<const char *>()) {
    String s = v.as<const char *>();
    s.trim();
    s.toLowerCase();

    if (s == "on" || s == "true" || s == "1" || s == "yes") {
      out = true;
      return true;
    }
    if (s == "off" || s == "false" || s == "0" || s == "no") {
      out = false;
      return true;
    }
  }

  return false;
}

static bool extractRpcBool(JsonVariant params, bool &out) {
  // Case 1: params trực tiếp là bool / number / string
  if (parseBoolFlexible(params, out)) {
    return true;
  }

  // Case 2: params là object, thử nhiều field name khác nhau
  if (params.is<JsonObject>()) {
    JsonObject obj = params.as<JsonObject>();

    const char *keys[] = {
      "value",
      "enabled",
      "state",
      "on",
      "lcdForecast",
      "forecast",
      "showForecast",
      "displayForecast",
      "mode"
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      if (obj.containsKey(keys[i])) {
        if (parseBoolFlexible(obj[keys[i]], out)) {
          return true;
        }
      }
    }

    // mode = "tinyml" / "sensor"
    if (obj.containsKey("mode") && obj["mode"].is<const char *>()) {
      String mode = obj["mode"].as<const char *>();
      mode.trim();
      mode.toLowerCase();

      if (mode == "tinyml" || mode == "forecast" || mode == "weather") {
        out = true;
        return true;
      }
      if (mode == "sensor" || mode == "normal") {
        out = false;
        return true;
      }
    }
  }

  return false;
}

static String extractRequestIdFromTopic(const char *topic) {
  String t(topic);
  int lastSlash = t.lastIndexOf('/');
  if (lastSlash < 0 || lastSlash >= (int)t.length() - 1) {
    return "";
  }
  return t.substring(lastSlash + 1);
}

static void publishRpcResponse(const char *topic, bool success, bool enabled) {
  String requestId = extractRequestIdFromTopic(topic);
  if (requestId.isEmpty()) {
    return;
  }

  StaticJsonDocument<128> doc;
  doc["success"] = success;
  doc["lcdForecast"] = enabled;

  char payload[128];
  size_t n = serializeJson(doc, payload, sizeof(payload));

  String responseTopic = String(kRpcResponsePrefix) + requestId;
  bool ok = client.publish(responseTopic.c_str(), payload, n);

  Serial.print("[CoreIoT] RPC response -> ");
  Serial.print(responseTopic);
  Serial.print(" | ");
  Serial.println(ok ? "OK" : "FAIL");
}

static void applyRemoteLcdForecastMode(bool enabled) {
  g_remoteLcdForecastEnabled = enabled;

  // Chỉ đổi state content mode.
  // lcd_task sẽ tự quyết định:
  // - AP -> luôn render sensor
  // - STA + enabled -> render tinyml
  // - STA + disabled -> render sensor
  if (enabled) {
    app_set_lcd_content_mode(g_ctx, LCD_CONTENT_TINYML);
    Serial.println("[CoreIoT] LCD forecast mode = ON");
  } else {
    app_set_lcd_content_mode(g_ctx, LCD_CONTENT_SENSOR);
    Serial.println("[CoreIoT] LCD forecast mode = OFF");
  }
}

static void log_mqtt_config() {
  const size_t pwdLen = strlen(g_mqttPassword);
  Serial.print("[CoreIoT] MQTT config | host=");
  Serial.print(g_mqttHost[0] != '\0' ? g_mqttHost : "(empty)");
  Serial.print(" | username=");
  Serial.print(g_mqttUsername[0] != '\0' ? g_mqttUsername : "(empty)");
  Serial.print(" | password_len=");
  Serial.print(pwdLen);
  Serial.print(" | publish=");
  Serial.println(g_coreiotPublishEnabled ? "on" : "off");
}

// ----------------------------------------------------
// MQTT connect / callback
// ----------------------------------------------------
static void reconnect() {
  static uint32_t retryMs = 3000;
  static uint32_t nextAttemptMs = 0;
  constexpr uint32_t kRetryBaseMs = 3000;
  constexpr uint32_t kRetryMaxMs = 30000;

  if (client.connected()) {
    app_set_coreiot_mqtt_connected(g_ctx, true);
    app_set_coreiot_retry_sec(g_ctx, 0);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    retryMs = kRetryBaseMs;
    nextAttemptMs = 0;
    app_set_coreiot_mqtt_connected(g_ctx, false);
    app_set_coreiot_retry_sec(g_ctx, 0);
    return;
  }

  if (!g_coreiotPublishEnabled) {
    app_set_coreiot_mqtt_connected(g_ctx, false);
    app_set_coreiot_retry_sec(g_ctx, 0);
    return;
  }

  const uint32_t nowMs = millis();
  if (nowMs < nextAttemptMs) {
    const uint32_t remainMs = nextAttemptMs - nowMs;
    const uint16_t remainSec = static_cast<uint16_t>((remainMs + 999U) / 1000U);
    app_set_coreiot_mqtt_connected(g_ctx, false);
    app_set_coreiot_retry_sec(g_ctx, remainSec);
    return;
  }

  Serial.print("[CoreIoT] Attempting MQTT connection... ");
  log_mqtt_config();

  if (client.connect("ESP32Client", g_mqttUsername, g_mqttPassword)) {
    Serial.println("connected");
    bool subOk = client.subscribe(kRpcTopicSub);
    Serial.print("[CoreIoT] Subscribe ");
    Serial.print(kRpcTopicSub);
    Serial.print(" -> ");
    Serial.println(subOk ? "OK" : "FAIL");

    // vẫn giữ khoảng nghỉ tối thiểu 3s giữa các lần attempt.
    // nếu broker vừa ngắt lại ngay, task cũng không spam connect liên tục.
    retryMs = kRetryBaseMs;
    nextAttemptMs = nowMs + kRetryBaseMs;
    app_set_coreiot_mqtt_connected(g_ctx, true);
    app_set_coreiot_retry_sec(g_ctx, 0);
  } else {
    Serial.print("failed, rc=");
    Serial.print(client.state());
    Serial.print(", next retry in ");
    Serial.print(retryMs / 1000);
    Serial.println("s");

    nextAttemptMs = nowMs + retryMs;
    app_set_coreiot_mqtt_connected(g_ctx, false);
    app_set_coreiot_retry_sec(g_ctx, static_cast<uint16_t>((retryMs + 999U) / 1000U));
    if (retryMs < kRetryMaxMs) {
      retryMs = (retryMs * 2 > kRetryMaxMs) ? kRetryMaxMs : (retryMs * 2);
    }
  }
}

static void callback(char *topic, byte *payload, unsigned int length) {
  Serial.print("[CoreIoT] Message arrived [");
  Serial.print(topic);
  Serial.println("]");

  if (length == 0) {
    Serial.println("[CoreIoT] Empty payload");
    return;
  }

  char message[256];
  unsigned int copyLen = (length < sizeof(message) - 1) ? length : (sizeof(message) - 1);
  memcpy(message, payload, copyLen);
  message[copyLen] = '\0';

  Serial.print("[CoreIoT] Payload: ");
  Serial.println(message);

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.print("[CoreIoT] deserializeJson() failed: ");
    Serial.println(error.c_str());
    publishRpcResponse(topic, false, g_remoteLcdForecastEnabled);
    return;
  }

  const char *method = doc["method"] | "";
  JsonVariant params = doc["params"];

  String methodStr = String(method);
  methodStr.trim();
  methodStr.toLowerCase();

  // chấp nhận nhiều tên method để đỡ phụ thuộc widget UI
  bool isLcdMethod =
      (methodStr == "setlcdforecast") ||
      (methodStr == "setforecastmode") ||
      (methodStr == "setdisplaymode") ||
      (methodStr == "settinymldisplay") ||
      (methodStr == "setweatherview") ||
      (methodStr == "setvalue") ||          // fallback, repo cũ đang dùng tên này
      (methodStr == "setvalueled") ||       // fallback
      (methodStr == "setstateled");         // fallback

  if (!isLcdMethod) {
    Serial.print("[CoreIoT] Unknown method: ");
    Serial.println(method);
    publishRpcResponse(topic, false, g_remoteLcdForecastEnabled);
    return;
  }

  bool enabled = false;
  if (!extractRpcBool(params, enabled)) {
    Serial.println("[CoreIoT] Cannot parse boolean from params");
    publishRpcResponse(topic, false, g_remoteLcdForecastEnabled);
    return;
  }

  applyRemoteLcdForecastMode(enabled);
  publishRpcResponse(topic, true, enabled);
}

static bool setup_coreiot(app_context_t *ctx) {
  if (ctx == nullptr || ctx->internetSemaphore == nullptr) {
    Serial.println("[CoreIoT] Invalid app context");
    return false;
  }

  Serial.println("[CoreIoT] Waiting internet...");
  xSemaphoreTake(ctx->internetSemaphore, portMAX_DELAY);
  Serial.println("[CoreIoT] Internet ready");

  if (g_mqttHost[0] == '\0') {
    strncpy(g_mqttHost, kDefaultBrokerHost, sizeof(g_mqttHost) - 1);
    g_mqttHost[sizeof(g_mqttHost) - 1] = '\0';
  }
  if (g_mqttUsername[0] == '\0') {
    strncpy(g_mqttUsername, kDefaultMqttUsername, sizeof(g_mqttUsername) - 1);
    g_mqttUsername[sizeof(g_mqttUsername) - 1] = '\0';
  }
  if (g_mqttPassword[0] == '\0') {
    strncpy(g_mqttPassword, kDefaultMqttPassword, sizeof(g_mqttPassword) - 1);
    g_mqttPassword[sizeof(g_mqttPassword) - 1] = '\0';
  }

  client.setServer(g_mqttHost, mqttPort);
  client.setCallback(callback);
  client.setBufferSize(512);
  app_set_coreiot_mqtt_connected(ctx, false);
  app_set_coreiot_retry_sec(ctx, 0);

  return true;
}

static void buildTelemetryPayload(
    const sensor_data_t &data,
    const tinyml_result_t *mlResult,
    bool hasMlResult,
    char *outBuffer,
    size_t outSize) {

  AppDateTime dt{};
  const bool hasEpoch = appTimeNow(kGmtOffsetSec, kDaylightOffsetSec, dt);

  StaticJsonDocument<256> doc;
  doc["temperature"] = data.temperature;
  doc["humidity"] = data.humidity;
  doc["ts"] = hasEpoch
                ? dt.epochSec
                : static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);

  float p01 = 0.0f;
  const char *pred = "NO_DATA";

  if (hasMlResult && mlResult != nullptr) {
    p01 = clamp01(mlResult->rainProbability);
    pred = mlResult->isRain ? "RAIN" : "SUNNY";
  }

  doc["rain_prob"] = p01;
  doc["rain_prob_pct"] = p01 * 100.0f;
  doc["rain_pred"] = pred;
  doc["rain_status_short"] = rain_status_short(p01);

  doc["device_id"] = kDeviceId;

  // publish thêm state LCD hiện tại cho dễ debug
  doc["lcd_forecast_mode"] = g_remoteLcdForecastEnabled;
  doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);

  serializeJson(doc, outBuffer, outSize);
}

} // namespace

void coreiot_set_publish_enabled(bool enabled) {
  g_coreiotPublishEnabled = enabled;
  if (!enabled && client.connected()) {
    client.disconnect();
  }
}

bool coreiot_get_publish_enabled() {
  return g_coreiotPublishEnabled;
}

bool coreiot_set_broker_host(const char *host) {
  if (host == nullptr || host[0] == '\0') {
    return false;
  }

  strncpy(g_mqttHost, host, sizeof(g_mqttHost) - 1);
  g_mqttHost[sizeof(g_mqttHost) - 1] = '\0';
  client.setServer(g_mqttHost, mqttPort);

  if (client.connected()) {
    client.disconnect();
  }

  return true;
}

void coreiot_get_broker_host(char *outHost, size_t outSize) {
  if (outHost == nullptr || outSize == 0) {
    return;
  }

  strncpy(outHost, g_mqttHost, outSize - 1);
  outHost[outSize - 1] = '\0';
}

bool coreiot_set_credentials(const char *username, const char *password) {
  if (username == nullptr || username[0] == '\0') {
    return false;
  }

  if (password == nullptr) {
    password = "";
  }

  strncpy(g_mqttUsername, username, sizeof(g_mqttUsername) - 1);
  g_mqttUsername[sizeof(g_mqttUsername) - 1] = '\0';

  strncpy(g_mqttPassword, password, sizeof(g_mqttPassword) - 1);
  g_mqttPassword[sizeof(g_mqttPassword) - 1] = '\0';

  if (client.connected()) {
    client.disconnect();
  }

  return true;
}

void coreiot_get_credentials(char *outUsername, size_t usernameSize, char *outPassword, size_t passwordSize) {
  if (outUsername != nullptr && usernameSize > 0) {
    strncpy(outUsername, g_mqttUsername, usernameSize - 1);
    outUsername[usernameSize - 1] = '\0';
  }

  if (outPassword != nullptr && passwordSize > 0) {
    strncpy(outPassword, g_mqttPassword, passwordSize - 1);
    outPassword[passwordSize - 1] = '\0';
  }
}

void coreiot_task(void *pvParameters) {
  app_context_t *ctx = static_cast<app_context_t *>(pvParameters);
  if (ctx == nullptr || ctx->coreQueue == nullptr || ctx->tinyResultQueue == nullptr) {
    Serial.println("[CoreIoT] Invalid context or queues");
    vTaskDelete(nullptr);
    return;
  }

  g_ctx = ctx;

  if (!setup_coreiot(ctx)) {
    vTaskDelete(nullptr);
    return;
  }

  sensor_data_t data{};
  tinyml_result_t mlResult{};
  bool hasMlResult = false;

  data.temperature = NAN;
  data.humidity = NAN;
  data.timestamp = 0;

  TickType_t lastPublishTick = xTaskGetTickCount();

  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      // không có internet thì loop nhẹ, chờ web task nối lại STA
      app_set_coreiot_mqtt_connected(ctx, false);
      app_set_coreiot_retry_sec(ctx, 0);
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    if (!g_coreiotPublishEnabled) {
      if (client.connected()) {
        client.disconnect();
      }
      app_set_coreiot_mqtt_connected(ctx, false);
      app_set_coreiot_retry_sec(ctx, 0);
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }

    if (!client.connected()) {
      reconnect();
    }

    client.loop();

    // cập nhật snapshot sensor mới nhất
    if (xQueuePeek(ctx->coreQueue, &data, 0) == pdTRUE) {
      // ok
    }

    // cập nhật snapshot tinyml mới nhất
    if (xQueuePeek(ctx->tinyResultQueue, &mlResult, 0) == pdTRUE) {
      hasMlResult = true;
    }

    TickType_t now = xTaskGetTickCount();
    if ((now - lastPublishTick) >= pdMS_TO_TICKS(10000)) {
      lastPublishTick = now;

      if (!isnan(data.temperature) && !isnan(data.humidity)) {
        char payload[256];
        buildTelemetryPayload(data, hasMlResult ? &mlResult : nullptr, hasMlResult, payload, sizeof(payload));

        bool ok = client.publish(kTelemetryTopic, payload);
        Serial.print("[CoreIoT] Publish -> ");
        Serial.println(ok ? "OK" : "FAIL");
        Serial.println(payload);
      } else {
        Serial.println("[CoreIoT] Sensor invalid, skip publish");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
