#include "coreiot.h"

// ----------- CONFIGURE THESE! -----------
const char* coreIOT_Server = "app.coreiot.io";  
const char* coreIOT_Token = "h61qp4bfrpbsp7iy5x2c";   // Device Access Token
const int   mqttPort = 1883;
// ----------------------------------------

WiFiClient espClient;
PubSubClient client(espClient);


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
      delay(5000);
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

  const char* method = doc["method"];
  if (strcmp(method, "setStateLED") == 0) {
    // Check params type (could be boolean, int, or string according to your RPC)
    // Example: {"method": "setValueLED", "params": "ON"}
    const char* params = doc["params"];

    if (strcmp(params, "ON") == 0) {
      Serial.println("Device turned ON.");
      //TODO

    } else {   
      Serial.println("Device turned OFF.");
      //TODO

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


void setup_coreiot(){

  //Serial.print("Connecting to WiFi...");
  //WiFi.begin(wifi_ssid, wifi_password);
  //while (WiFi.status() != WL_CONNECTED) {
  
  // while (isWifiConnected == false) {
  //   delay(500);
  //   Serial.print(".");
  // }

  while(1){
    if (xSemaphoreTake(xBinarySemaphoreInternet, portMAX_DELAY)) {
      break;
    }
    delay(500);
    Serial.print(".");
  }


  Serial.println(" Connected!");

  client.setServer(coreIOT_Server, mqttPort);
  client.setCallback(callback);

}

void coreiot_task(void *pvParameters) {
  app_context_t *ctx = static_cast<app_context_t *>(pvParameters);
  if (ctx == nullptr || ctx->webQueue == nullptr) {
    Serial.println("[CoreIoT] Invalid context or webQueue");
    vTaskDelete(nullptr);
    return;
  }

  if (!setup_coreiot(ctx)) {
    vTaskDelete(nullptr);
    return;
  }

  sensor_data_t data{};
  data.temperature = NAN;
  data.humidity = NAN;

  while (1) {
    if (!client.connected()) {
      reconnect();
    }
    client.loop();

    // Đọc mẫu mới nhất từ queue (không pop)
    if (xQueuePeek(ctx->webQueue, &data, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (!isnan(data.temperature) && !isnan(data.humidity)) {
        StaticJsonDocument<128> doc;
        doc["temperature"] = data.temperature;
        doc["humidity"] = data.humidity;
        doc["ts"] = (uint32_t)(data.timestamp * portTICK_PERIOD_MS);

        char payload[128];
        size_t n = serializeJson(doc, payload, sizeof(payload));

        bool ok = client.publish("v1/devices/me/telemetry", payload, n);
        Serial.print("[CoreIoT] ");
        Serial.println(ok ? "Publish OK" : "Publish FAIL");
        Serial.println(payload);
      } else {
        Serial.println("[CoreIoT] Sensor invalid, skip publish");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}