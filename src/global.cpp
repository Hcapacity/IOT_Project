#include "global.h"

namespace {

bool lockSharedState(app_context_t *ctx) {
  if (ctx == nullptr || ctx->stateMutex == nullptr) {
    return false;
  }
  return xSemaphoreTake(ctx->stateMutex, portMAX_DELAY) == pdTRUE;
}

void unlockSharedState(app_context_t *ctx) {
  if (ctx != nullptr && ctx->stateMutex != nullptr) {
    xSemaphoreGive(ctx->stateMutex);
  }
}

} // namespace

void init_app_shared_state(app_context_t *ctx) {
  if (ctx == nullptr) {
    return;
  }

  ctx->sharedState.latestTemperature = NAN;
  ctx->sharedState.latestHumidity = NAN;
  ctx->sharedState.wifiConnected = false;
  ctx->sharedState.lcdContentMode = LCD_CONTENT_SENSOR;
  ctx->sharedState.coreiotMqttConnected = false;
  ctx->sharedState.coreiotRetrySec = 0;
}

void app_set_latest_sensor(app_context_t *ctx, float temperature, float humidity) {
  if (!lockSharedState(ctx)) {
    return;
  }

  ctx->sharedState.latestTemperature = temperature;
  ctx->sharedState.latestHumidity = humidity;
  unlockSharedState(ctx);
}

void app_set_wifi_connected(app_context_t *ctx, bool connected) {
  if (!lockSharedState(ctx)) {
    return;
  }

  ctx->sharedState.wifiConnected = connected;
  unlockSharedState(ctx);
}

bool app_get_wifi_connected(app_context_t *ctx) {
  if (!lockSharedState(ctx)) {
    return false;
  }

  const bool connected = ctx->sharedState.wifiConnected;
  unlockSharedState(ctx);
  return connected;
}

void app_set_lcd_content_mode(app_context_t *ctx, lcd_content_mode_t mode) {
  if (!lockSharedState(ctx)) {
    return;
  }

  ctx->sharedState.lcdContentMode = mode;
  unlockSharedState(ctx);
}

lcd_content_mode_t app_get_lcd_content_mode(app_context_t *ctx) {
  if (!lockSharedState(ctx)) {
    return LCD_CONTENT_SENSOR;
  }

  const lcd_content_mode_t mode = ctx->sharedState.lcdContentMode;
  unlockSharedState(ctx);
  return mode;
}

void app_set_coreiot_mqtt_connected(app_context_t *ctx, bool connected) {
  if (!lockSharedState(ctx)) {
    return;
  }

  ctx->sharedState.coreiotMqttConnected = connected;
  unlockSharedState(ctx);
}

bool app_get_coreiot_mqtt_connected(app_context_t *ctx) {
  if (!lockSharedState(ctx)) {
    return false;
  }

  const bool connected = ctx->sharedState.coreiotMqttConnected;
  unlockSharedState(ctx);
  return connected;
}

void app_set_coreiot_retry_sec(app_context_t *ctx, uint16_t retrySec) {
  if (!lockSharedState(ctx)) {
    return;
  }

  ctx->sharedState.coreiotRetrySec = retrySec;
  unlockSharedState(ctx);
}

uint16_t app_get_coreiot_retry_sec(app_context_t *ctx) {
  if (!lockSharedState(ctx)) {
    return 0;
  }

  const uint16_t retrySec = ctx->sharedState.coreiotRetrySec;
  unlockSharedState(ctx);
  return retrySec;
}

led_mode_t classify_temperature_mode(float temperature) {
  if (temperature < 10.0f) {
    return LED_MODE_TEMP_COLD;
  }
  if (temperature < 40.0f) {
    return LED_MODE_TEMP_NORMAL;
  }
  return LED_MODE_TEMP_HOT;
}

const char *classify_environment_status(float temperature, float humidity) {
  if (temperature >= 35.0f || humidity >= 85.0f) {
    return "CRITICAL";
  }
  if ((temperature >= 30.0f && temperature < 35.0f) || humidity >= 70.0f) {
    return "WARNING";
  }
  return "NORMAL";
}
