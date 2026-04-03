#include "temp_humi_monitor.h"

static DHT20 g_dht20;
static LiquidCrystal_I2C g_lcd(0x21, 16, 2); 
// Nếu không hiện, test lại bằng I2C scanner.
// Rất có thể phải đổi thành 0x27 hoặc 0x3F.

static bool g_lcdInitialized = false;
static bool g_dhtInitialized = false;

static void lcd_print_padded(uint8_t col, uint8_t row, const char *text) {
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "%-16.16s", text);
  g_lcd.setCursor(col, row);
  g_lcd.print(buffer);
}

void sensor_task(void *pvParameters) {
  app_context_t *ctx = static_cast<app_context_t *>(pvParameters);
  if (ctx == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  if (xSemaphoreTake(ctx->i2cMutex, portMAX_DELAY) == pdTRUE) {
    if (!g_dhtInitialized) {
      g_dht20.begin();
      g_dhtInitialized = true;
      Serial.println("[Sensor] DHT20 initialized.");
    }
    xSemaphoreGive(ctx->i2cMutex);
  }

  TickType_t lastWakeTime = xTaskGetTickCount();

  while (true) {
    sensor_data_t data{};
    data.temperature = NAN;
    data.humidity = NAN;
    data.timestamp = xTaskGetTickCount();

    if (xSemaphoreTake(ctx->i2cMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
      g_dht20.read();
      data.temperature = g_dht20.getTemperature();
      data.humidity = g_dht20.getHumidity();
      xSemaphoreGive(ctx->i2cMutex);
    }

    if (!isnan(data.temperature) && !isnan(data.humidity)) {
      led_command_t ledCmd{};
      ledCmd.type = LED_CMD_SENSOR_UPDATE;
      ledCmd.temperature = data.temperature;
      xQueueSend(ctx->ledQueue, &ledCmd, 0);

      neo_command_t neoCmd{};
      neoCmd.type = NEO_CMD_SENSOR_UPDATE;
      neoCmd.humidity = data.humidity;
      xQueueOverwrite(ctx->neoQueue, &neoCmd);

      xQueueOverwrite(ctx->lcdQueue, &data);
      xQueueOverwrite(ctx->webQueue, &data);

      Serial.printf("[Sensor] T=%.2f C, H=%.2f %%\n", data.temperature, data.humidity);
    } else {
      Serial.println("[Sensor] Failed to read DHT20.");
      // vẫn gửi xuống LCD để LCD hiện lỗi rõ ràng
      xQueueOverwrite(ctx->lcdQueue, &data);
      xQueueOverwrite(ctx->webQueue, &data);
    }

    vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(2000));
  }
}

void lcd_task(void *pvParameters) {
  app_context_t *ctx = static_cast<app_context_t *>(pvParameters);
  if (ctx == nullptr || ctx->lcdQueue == nullptr || ctx->i2cMutex == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  if (xSemaphoreTake(ctx->i2cMutex, portMAX_DELAY) == pdTRUE) {
    if (!g_lcdInitialized) {
      g_lcd.begin();
      g_lcd.backlight();
      g_lcd.clear();

      lcd_print_padded(0, 0, "System Booting");
      lcd_print_padded(0, 1, "Wait sensor...");
      g_lcdInitialized = true;

      Serial.println("[LCD] LCD initialized.");
    }
    xSemaphoreGive(ctx->i2cMutex);
  }

  sensor_data_t data{};

  while (true) {
    if (xQueueReceive(ctx->lcdQueue, &data, portMAX_DELAY) == pdTRUE) {
      if (xSemaphoreTake(ctx->i2cMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        char line1[17];
        char line2[17];

        if (isnan(data.temperature) || isnan(data.humidity)) {
          snprintf(line1, sizeof(line1), "Sensor Error");
          snprintf(line2, sizeof(line2), "Check DHT20");
        } else {
          const char *status = classify_environment_status(data.temperature, data.humidity);

          // Dòng 1: Temp + Hum
          // ví dụ: "T:28.1C H:65.2"
          snprintf(line1, sizeof(line1), "T:%4.1fC H:%4.1f", data.temperature, data.humidity);

          // Dòng 2: Status
          // ví dụ: "Status:NORMAL"
          snprintf(line2, sizeof(line2), "Status:%s", status);
        }

        lcd_print_padded(0, 0, line1);
        lcd_print_padded(0, 1, line2);

        xSemaphoreGive(ctx->i2cMutex);
      }
    }
  }
}