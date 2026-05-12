#include "temp_humi_monitor.h"

#include <WiFi.h>
#include <math.h>

namespace {

static DHT20 g_dht20;
static LiquidCrystal_I2C g_lcd(0x21, 16, 2);

static bool g_isLcdInitialized = false;
static bool g_isDhtInitialized = false;

constexpr uint8_t kLcdColumnCount = 16;
constexpr int32_t kDisconnectedRssi = -100;
constexpr int32_t kWifiRssiRenderThreshold = 3;
constexpr float kTinyMlProbabilityRenderThreshold = 0.01f;
constexpr TickType_t kSensorPeriodTicks = pdMS_TO_TICKS(2000);
constexpr TickType_t kI2cAccessTimeoutTicks = pdMS_TO_TICKS(300);
constexpr TickType_t kLcdQueueWaitTicks = pdMS_TO_TICKS(250);

void printPaddedLcdLine(uint8_t column, uint8_t row, const char *text) {
  char lineBuffer[kLcdColumnCount + 1];
  snprintf(lineBuffer, sizeof(lineBuffer), "%-16.16s", text);
  g_lcd.setCursor(column, row);
  g_lcd.print(lineBuffer);
}

int getWifiQualityPercent(int32_t rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

const char *getWifiQualityText(int32_t rssi) {
  if (rssi >= -55) return "EXCELLENT";
  if (rssi >= -67) return "GOOD";
  if (rssi >= -75) return "FAIR";
  if (rssi >= -85) return "WEAK";
  return "BAD";
}

bool isApModeActive() {
  const wifi_mode_t wifiMode = WiFi.getMode();
  return wifiMode == WIFI_AP || wifiMode == WIFI_AP_STA;
}

const char *getWebModeText() {
  return isApModeActive() ? "AP" : "STA";
}

const char *getTinyMlWeatherText(const tinyml_result_t &tinyMlResult, bool hasTinyMlResult) {
  if (!hasTinyMlResult) return "WAIT";
  return tinyMlResult.isRain ? "RAIN" : "SUNNY";
}

int getTinyMlProbabilityPercent(const tinyml_result_t &tinyMlResult, bool hasTinyMlResult) {
  if (!hasTinyMlResult) return -1;

  int probabilityPercent = static_cast<int>(roundf(tinyMlResult.rainProbability * 100.0f));
  if (probabilityPercent < 0) probabilityPercent = 0;
  if (probabilityPercent > 100) probabilityPercent = 100;
  return probabilityPercent;
}

void renderSensorScreen(const sensor_data_t &sensorData) {
  char firstLine[kLcdColumnCount + 1];
  char secondLine[kLcdColumnCount + 1];

  if (isnan(sensorData.temperature) || isnan(sensorData.humidity)) {
    snprintf(firstLine, sizeof(firstLine), "Sensor Error");
    snprintf(secondLine, sizeof(secondLine), "Mode:%s", getWebModeText());
  } else {
    snprintf(firstLine, sizeof(firstLine), "T:%4.1fC H:%4.1f%%", sensorData.temperature, sensorData.humidity);
    snprintf(secondLine, sizeof(secondLine), "Mode:%s", getWebModeText());
  }

  printPaddedLcdLine(0, 0, firstLine);
  printPaddedLcdLine(0, 1, secondLine);
}

void renderWifiScreen() {
  char firstLine[kLcdColumnCount + 1];
  char secondLine[kLcdColumnCount + 1];

  const wl_status_t wifiStatus = WiFi.status();
  const int32_t currentRssi = (wifiStatus == WL_CONNECTED) ? WiFi.RSSI() : kDisconnectedRssi;
  const int qualityPercent = (wifiStatus == WL_CONNECTED) ? getWifiQualityPercent(currentRssi) : 0;

  snprintf(firstLine, sizeof(firstLine), "WebMode:%-8s", getWebModeText());

  if (wifiStatus == WL_CONNECTED) {
    snprintf(secondLine, sizeof(secondLine), "WiFi:%3d%% %-4s", qualityPercent, getWifiQualityText(currentRssi));
  } else {
    snprintf(secondLine, sizeof(secondLine), "WiFi:NOT LINKED");
  }

  printPaddedLcdLine(0, 0, firstLine);
  printPaddedLcdLine(0, 1, secondLine);
}

void renderTinyMlScreen(const tinyml_result_t &tinyMlResult, bool hasTinyMlResult) {
  char firstLine[kLcdColumnCount + 1];
  char secondLine[kLcdColumnCount + 1];

  const char *weatherText = getTinyMlWeatherText(tinyMlResult, hasTinyMlResult);
  const int probabilityPercent = getTinyMlProbabilityPercent(tinyMlResult, hasTinyMlResult);

  if (probabilityPercent >= 0) {
    snprintf(firstLine, sizeof(firstLine), "Fcst:%s %3d%%", weatherText, probabilityPercent);
  } else {
    snprintf(firstLine, sizeof(firstLine), "Fcst:%s", weatherText);
  }

  snprintf(secondLine, sizeof(secondLine), "Mode:%s", getWebModeText());

  printPaddedLcdLine(0, 0, firstLine);
  printPaddedLcdLine(0, 1, secondLine);
}

void initializeDhtIfNeeded(app_context_t *appContext) {
  if (xSemaphoreTake(appContext->i2cMutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  if (!g_isDhtInitialized) {
    g_dht20.begin();
    g_isDhtInitialized = true;
    Serial.println("[Sensor] DHT20 initialized.");
  }

  xSemaphoreGive(appContext->i2cMutex);
}

void initializeLcdIfNeeded(app_context_t *appContext) {
  if (xSemaphoreTake(appContext->i2cMutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  if (!g_isLcdInitialized) {
    g_lcd.begin();
    g_lcd.backlight();
    g_lcd.clear();
    printPaddedLcdLine(0, 0, "System Booting");
    printPaddedLcdLine(0, 1, "Wait sensor...");
    g_isLcdInitialized = true;
    Serial.println("[LCD] LCD initialized.");
  }

  xSemaphoreGive(appContext->i2cMutex);
}

sensor_data_t readSensorSnapshot(app_context_t *appContext) {
  sensor_data_t sensorSnapshot{};
  sensorSnapshot.temperature = NAN;
  sensorSnapshot.humidity = NAN;
  sensorSnapshot.timestamp = xTaskGetTickCount();

  if (xSemaphoreTake(appContext->i2cMutex, kI2cAccessTimeoutTicks) != pdTRUE) {
    return sensorSnapshot;
  }

  g_dht20.read();
  sensorSnapshot.temperature = g_dht20.getTemperature();
  sensorSnapshot.humidity = g_dht20.getHumidity();
  xSemaphoreGive(appContext->i2cMutex);

  return sensorSnapshot;
}

void publishValidSensorSnapshot(app_context_t *appContext, const sensor_data_t &sensorSnapshot) {
  app_set_latest_sensor(appContext, sensorSnapshot.temperature, sensorSnapshot.humidity);

  led_command_t ledCommand{};
  ledCommand.type = LED_CMD_SENSOR_UPDATE;
  ledCommand.temperature = sensorSnapshot.temperature;
  xQueueSend(appContext->ledQueue, &ledCommand, 0);

  neo_command_t neoCommand{};
  neoCommand.type = NEO_CMD_SENSOR_UPDATE;
  neoCommand.humidity = sensorSnapshot.humidity;
  xQueueOverwrite(appContext->neoQueue, &neoCommand);

  xQueueOverwrite(appContext->lcdQueue, &sensorSnapshot);
  xQueueOverwrite(appContext->webQueue, &sensorSnapshot);
  xQueueOverwrite(appContext->coreQueue, &sensorSnapshot);
  xQueueOverwrite(appContext->tinyMLQueue, &sensorSnapshot);
}

bool hasValidSensorValues(const sensor_data_t &sensorSnapshot) {
  return !isnan(sensorSnapshot.temperature) && !isnan(sensorSnapshot.humidity);
}

bool hasTinyMlResultChanged(
  const tinyml_result_t &currentTinyMlResult,
  const tinyml_result_t &previousTinyMlResult,
  bool hadTinyMlResultBefore
) {
  return !hadTinyMlResultBefore ||
         currentTinyMlResult.isRain != previousTinyMlResult.isRain ||
         fabsf(currentTinyMlResult.rainProbability - previousTinyMlResult.rainProbability) >= kTinyMlProbabilityRenderThreshold ||
         currentTinyMlResult.inferTimestamp != previousTinyMlResult.inferTimestamp;
}

void renderCurrentLcdScreen(
  app_context_t *appContext,
  const sensor_data_t &latestSensorSnapshot,
  const tinyml_result_t &latestTinyMlResult,
  bool hasTinyMlResult,
  lcd_view_mode_t lcdViewMode,
  lcd_content_mode_t lcdContentMode,
  bool apModeIsActive
) {
  if (xSemaphoreTake(appContext->i2cMutex, kI2cAccessTimeoutTicks) != pdTRUE) {
    return;
  }

  if (lcdViewMode == LCD_VIEW_WIFI) {
    renderWifiScreen();
    xSemaphoreGive(appContext->i2cMutex);
    return;
  }

  // AP mode intentionally ignores the remote LCD content toggle.
  // If this condition is removed, the setup screen may stop showing live sensor values exactly when users need them most.
  if (apModeIsActive) {
    renderSensorScreen(latestSensorSnapshot);
    xSemaphoreGive(appContext->i2cMutex);
    return;
  }

  if (lcdContentMode == LCD_CONTENT_TINYML) {
    renderTinyMlScreen(latestTinyMlResult, hasTinyMlResult);
  } else {
    renderSensorScreen(latestSensorSnapshot);
  }

  xSemaphoreGive(appContext->i2cMutex);
}

}  // namespace

void sensor_task(void *pvParameters) {
  app_context_t *appContext = static_cast<app_context_t *>(pvParameters);
  if (appContext == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  initializeDhtIfNeeded(appContext);

  TickType_t lastWakeTick = xTaskGetTickCount();

  while (true) {
    const sensor_data_t sensorSnapshot = readSensorSnapshot(appContext);

    if (hasValidSensorValues(sensorSnapshot)) {
      publishValidSensorSnapshot(appContext, sensorSnapshot);
      Serial.printf("[Sensor] T=%.2f C, H=%.2f %%\n", sensorSnapshot.temperature, sensorSnapshot.humidity);
    } else {
      Serial.println("[Sensor] Failed to read DHT20.");
      xQueueOverwrite(appContext->lcdQueue, &sensorSnapshot);
      xQueueOverwrite(appContext->webQueue, &sensorSnapshot);
    }

    vTaskDelayUntil(&lastWakeTick, kSensorPeriodTicks);
  }
}

void lcd_task(void *pvParameters) {
  app_context_t *appContext = static_cast<app_context_t *>(pvParameters);
  if (appContext == nullptr || appContext->lcdQueue == nullptr || appContext->i2cMutex == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  initializeLcdIfNeeded(appContext);

  sensor_data_t latestSensorSnapshot{};
  latestSensorSnapshot.temperature = NAN;
  latestSensorSnapshot.humidity = NAN;
  latestSensorSnapshot.timestamp = 0;

  tinyml_result_t latestTinyMlResult{};
  latestTinyMlResult.rainProbability = 0.0f;
  latestTinyMlResult.isRain = false;
  latestTinyMlResult.sensorTimestamp = 0;
  latestTinyMlResult.inferTimestamp = 0;

  bool hasTinyMlResult = false;

  const lcd_view_mode_t lcdViewMode = LCD_VIEW_SENSOR;
  lcd_content_mode_t lastRenderedContentMode = app_get_lcd_content_mode(appContext);
  int32_t lastRenderedRssi = -999;
  bool wasApModeActive = isApModeActive();

  while (true) {
    bool shouldRender = false;

    sensor_data_t queuedSensorSnapshot{};
    if (xQueueReceive(appContext->lcdQueue, &queuedSensorSnapshot, kLcdQueueWaitTicks) == pdTRUE) {
      latestSensorSnapshot = queuedSensorSnapshot;
      shouldRender = true;
    }

    if (appContext->tinyResultQueue != nullptr) {
      tinyml_result_t queuedTinyMlResult{};
      if (xQueuePeek(appContext->tinyResultQueue, &queuedTinyMlResult, 0) == pdTRUE) {
        const bool tinyMlResultChanged = hasTinyMlResultChanged(
          queuedTinyMlResult,
          latestTinyMlResult,
          hasTinyMlResult
        );

        latestTinyMlResult = queuedTinyMlResult;
        hasTinyMlResult = true;

        if (tinyMlResultChanged) {
          shouldRender = true;
        }
      }
    }

    const int32_t currentRssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : kDisconnectedRssi;
    const bool wifiStrengthChanged = abs(currentRssi - lastRenderedRssi) >= kWifiRssiRenderThreshold;

    const lcd_content_mode_t currentContentMode = app_get_lcd_content_mode(appContext);
    const bool contentModeChanged = currentContentMode != lastRenderedContentMode;

    const bool apModeIsActive = isApModeActive();
    const bool apModeChanged = apModeIsActive != wasApModeActive;

    if (wifiStrengthChanged || contentModeChanged || apModeChanged) {
      shouldRender = true;
    }

    if (!shouldRender) {
      continue;
    }

    renderCurrentLcdScreen(
      appContext,
      latestSensorSnapshot,
      latestTinyMlResult,
      hasTinyMlResult,
      lcdViewMode,
      currentContentMode,
      apModeIsActive
    );

    lastRenderedContentMode = currentContentMode;
    lastRenderedRssi = currentRssi;
    wasApModeActive = apModeIsActive;
  }
}
