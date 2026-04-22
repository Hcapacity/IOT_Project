#include "temp_humi_monitor.h"
#include <WiFi.h>
#include <math.h>

static DHT20 g_dht20;
static LiquidCrystal_I2C g_lcd(0x21, 16, 2);

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
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

static const char *wifi_quality_text(int32_t rssi) {
  if (rssi >= -55) return "EXCELLENT";
  if (rssi >= -67) return "GOOD";
  if (rssi >= -75) return "FAIR";
  if (rssi >= -85) return "WEAK";
  return "BAD";
}

static const char *web_mode_text() {
  wifi_mode_t mode = WiFi.getMode();

  if (mode == WIFI_AP || mode == WIFI_AP_STA) {
    return "AP";
  }
  return "STA";
}

static bool is_ap_mode_active() {
  wifi_mode_t mode = WiFi.getMode();
  return (mode == WIFI_AP || mode == WIFI_AP_STA);
}

static const char *tinyml_weather_text(const tinyml_result_t &tiny, bool hasTinyResult) {
  if (!hasTinyResult) return "WAIT";
  return tiny.isRain ? "RAIN" : "SUNNY";
}

static int tinyml_probability_percent(const tinyml_result_t &tiny, bool hasTinyResult) {
  if (!hasTinyResult) return -1;
  int p = (int)roundf(tiny.rainProbability * 100.0f);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

static void render_sensor_screen(const sensor_data_t &data) {
  char line1[17];
  char line2[17];

  if (isnan(data.temperature) || isnan(data.humidity)) {
    snprintf(line1, sizeof(line1), "Sensor Error");
    snprintf(line2, sizeof(line2), "Mode:%s", web_mode_text());
  } else {
    snprintf(line1, sizeof(line1), "T:%4.1fC H:%4.1f%%", data.temperature, data.humidity);
    snprintf(line2, sizeof(line2), "Mode:%s", web_mode_text());
  }

  lcd_print_padded(0, 0, line1);
  lcd_print_padded(0, 1, line2);
}

static void render_wifi_screen() {
  char line1[17];
  char line2[17];

  wl_status_t st = WiFi.status();
  int32_t rssi = (st == WL_CONNECTED) ? WiFi.RSSI() : -100;
  int quality = (st == WL_CONNECTED) ? wifi_quality_percent(rssi) : 0;

  snprintf(line1, sizeof(line1), "WebMode:%-8s", web_mode_text());

  if (st == WL_CONNECTED) {
    snprintf(line2, sizeof(line2), "WiFi:%3d%% %-4s", quality, wifi_quality_text(rssi));
  } else {
    snprintf(line2, sizeof(line2), "WiFi:NOT LINKED");
  }

  lcd_print_padded(0, 0, line1);
  lcd_print_padded(0, 1, line2);
}

static void render_tinyml_screen(const tinyml_result_t &tiny, bool hasTinyResult) {
  char line1[17];
  char line2[17];

  const char *weather = tinyml_weather_text(tiny, hasTinyResult);
  int prob = tinyml_probability_percent(tiny, hasTinyResult);

  if (prob >= 0) {
    snprintf(line1, sizeof(line1), "Fcst:%s %3d%%", weather, prob);
  } else {
    snprintf(line1, sizeof(line1), "Fcst:%s", weather);
  }

  snprintf(line2, sizeof(line2), "Mode:%s", web_mode_text());

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
      app_set_latest_sensor(ctx, data.temperature, data.humidity);

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
      xQueueOverwrite(ctx->coreQueue, &data);
      xQueueOverwrite(ctx->tinyMLQueue, &data);

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

  sensor_data_t lastSensor{};
  lastSensor.temperature = NAN;
  lastSensor.humidity = NAN;
  lastSensor.timestamp = 0;

  tinyml_result_t lastTiny{};
  lastTiny.rainProbability = 0.0f;
  lastTiny.isRain = false;
  lastTiny.sensorTimestamp = 0;
  lastTiny.inferTimestamp = 0;
  bool hasTinyResult = false;

  const lcd_view_mode_t lcdViewMode = LCD_VIEW_SENSOR;
  lcd_content_mode_t lastRenderedContent = app_get_lcd_content_mode(ctx);
  int lastRssi = -999;
  bool lastApMode = is_ap_mode_active();

  while (true) {
    bool needRender = false;

    sensor_data_t incoming{};
    if (xQueueReceive(ctx->lcdQueue, &incoming, pdMS_TO_TICKS(250)) == pdTRUE) {
      lastSensor = incoming;
      needRender = true;
    }

    if (ctx->tinyResultQueue != nullptr) {
      tinyml_result_t tiny{};
      if (xQueuePeek(ctx->tinyResultQueue, &tiny, 0) == pdTRUE) {
        bool changed =
          (!hasTinyResult) ||
          (tiny.isRain != lastTiny.isRain) ||
          (fabsf(tiny.rainProbability - lastTiny.rainProbability) >= 0.01f) ||
          (tiny.inferTimestamp != lastTiny.inferTimestamp);

        lastTiny = tiny;
        hasTinyResult = true;

        if (changed) {
          needRender = true;
        }
      }
    }

    int currentRssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
    bool wifiChanged = abs(currentRssi - lastRssi) >= 3;
    lcd_content_mode_t lcdContentMode = app_get_lcd_content_mode(ctx);
    bool contentChanged = (lastRenderedContent != lcdContentMode);
    bool apModeNow = is_ap_mode_active();
    bool apModeChanged = (apModeNow != lastApMode);

    if (wifiChanged || contentChanged || apModeChanged) {
      needRender = true;
    }

    if (needRender) {
      if (xSemaphoreTake(ctx->i2cMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        if (lcdViewMode == LCD_VIEW_WIFI) {
          render_wifi_screen();
        } else {
          // AP mode luôn ưu tiên màn sensor, bỏ qua CoreIOT toggle
          if (apModeNow) {
            render_sensor_screen(lastSensor);
          } else {
            // STA mode: chỉ khi CoreIOT bật thì mới show TinyML
            if (lcdContentMode == LCD_CONTENT_TINYML) {
              render_tinyml_screen(lastTiny, hasTinyResult);
            } else {
              render_sensor_screen(lastSensor);
            }
          }
        }

        xSemaphoreGive(ctx->i2cMutex);
      }

      lastRenderedContent = lcdContentMode;
      lastRssi = currentRssi;
      lastApMode = apModeNow;
    }
  }
}
