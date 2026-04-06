#include "temp_humi_monitor.h"
#include <WiFi.h>

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

static int wifi_quality_percent(int32_t rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50)  return 100;
  return 2 * (rssi + 100);
}

static const char* wifi_quality_text(int32_t rssi) {
  if (rssi >= -55) return "EXCELLENT";
  if (rssi >= -67) return "GOOD";
  if (rssi >= -75) return "FAIR";
  if (rssi >= -85) return "WEAK";
  return "BAD";
}

static const char* wifi_mode_text() {
  wl_status_t st = WiFi.status();
  wifi_mode_t mode = WiFi.getMode();

  if (mode == WIFI_MODE_AP) return "AP MODE";
  if (st == WL_CONNECTED) return "CONNECTED";
  return "DISCONNECTED";
}

static void render_lcd(sensor_data_t data) {
  char line1[17];
  char line2[17];

  if (g_lcdViewMode == LCD_VIEW_WIFI) {
    wl_status_t st = WiFi.status();
    int32_t rssi = (st == WL_CONNECTED) ? WiFi.RSSI() : -100;
    int quality = (st == WL_CONNECTED) ? wifi_quality_percent(rssi) : 0;

    snprintf(line1, sizeof(line1), "WiFi:%-10s", wifi_mode_text());

    if (st == WL_CONNECTED) {
      snprintf(line2, sizeof(line2), "Q:%3d%% %4ddBm", quality, (int)rssi);
    } else {
      snprintf(line2, sizeof(line2), "Q:  0%% NO LINK");
    }
  } else {
    if (isnan(data.temperature) || isnan(data.humidity)) {
      snprintf(line1, sizeof(line1), "Sensor Error");
      snprintf(line2, sizeof(line2), "Check DHT20");
    } else {
      const char *status = classify_environment_status(data.temperature, data.humidity);
      snprintf(line1, sizeof(line1), "T:%4.1fC H:%4.1f", data.temperature, data.humidity);
      snprintf(line2, sizeof(line2), "Status:%s", status);
    }
  }

  lcd_print_padded(0, 0, line1);
  lcd_print_padded(0, 1, line2);
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

  sensor_data_t lastData{};
  lastData.temperature = NAN;
  lastData.humidity = NAN;
  lastData.timestamp = 0;

  lcd_view_mode_t lastMode = g_lcdViewMode;
  int lastRssi = -999;

  while (true) {
    sensor_data_t incoming{};
    bool hasNewData = false;

    if (xQueueReceive(ctx->lcdQueue, &incoming, pdMS_TO_TICKS(500)) == pdTRUE) {
      lastData = incoming;
      hasNewData = true;
    }

    int currentRssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
    bool wifiChanged = abs(currentRssi - lastRssi) >= 3;
    bool modeChanged = (lastMode != g_lcdViewMode);

    if (hasNewData || wifiChanged || modeChanged) {
      if (xSemaphoreTake(ctx->i2cMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        render_lcd(lastData);
        xSemaphoreGive(ctx->i2cMutex);
      }

      lastMode = g_lcdViewMode;
      lastRssi = currentRssi;
    }
  }
}