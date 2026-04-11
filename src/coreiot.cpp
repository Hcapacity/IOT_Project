#include "coreiot.h"
#include "app_time_utils.h"

// ----------- CONFIGURE THESE! -----------
const char* coreIOT_Server = "app.coreiot.io";  
const char* coreIOT_Token = "h61qp4bfrpbsp7iy5x2c";   // Device Access Token
const int   mqttPort = 1883;
constexpr int32_t kGmtOffsetSec = 7 * 3600;
constexpr int32_t kDaylightOffsetSec = 0;
// ----------------------------------------

WiFiClient espClient;
PubSubClient client(espClient);

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


void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect (username=token, password=empty)
    if (client.connect("ESP32Client", coreIOT_Token, NULL)) {
      Serial.println("connected to CoreIOT Server!");
      client.subscribe("v1/devices/me/rpc/request/+");
      Serial.println("Subscribed to v1/devices/me/rpc/request/+");

    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}


void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.println("] ");

  // Allocate a temporary buffer for the message
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  Serial.print("Payload: ");
  Serial.println(message);

  // Parse JSON
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }

  const char* method = doc["method"] | "";
  JsonVariant params = doc["params"];

  // Accept a few common RPC method names used across the project/UI.
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
      // TODO: bật LED / relay / action tương ứng
    } else {
      Serial.println("Device turned OFF.");
      // TODO: tắt LED / relay / action tương ứng
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

  client.setServer(coreIOT_Server, mqttPort);
  client.setCallback(callback);
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
    if (!client.connected()) {
      reconnect();
    }
    client.loop();

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

        StaticJsonDocument<192> doc;
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

        char payload[192];
        size_t n = serializeJson(doc, payload, sizeof(payload));

        bool ok = client.publish("v1/devices/me/telemetry", payload, n);
        Serial.print("[CoreIoT] ");
        Serial.println(ok ? "Publish OK" : "Publish FAIL");
        Serial.println(payload);
        } else {
          Serial.println("[CoreIoT] Sensor invalid, skip publish");
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}