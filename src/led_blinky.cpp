#include "led_blinky.h"

static void led_apply(bool level) {
  digitalWrite(LED_GPIO, level ? HIGH : LOW);
}

static void play_wifi_success_pattern() {
  for (int i = 0; i < 3; ++i) {
    led_apply(true);
    vTaskDelay(pdMS_TO_TICKS(180));
    led_apply(false);
    vTaskDelay(pdMS_TO_TICKS(80));
    led_apply(true);
    vTaskDelay(pdMS_TO_TICKS(180));
    led_apply(false);
    vTaskDelay(pdMS_TO_TICKS(220));
  }
}

static void play_mqtt_success_pattern() {
  for (int i = 0; i < 5; ++i) {
    led_apply(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    led_apply(false);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static TickType_t led_half_period_ticks(led_mode_t mode) {
  switch (mode) {
    case LED_MODE_TEMP_COLD:
      return pdMS_TO_TICKS(5000);   
    case LED_MODE_TEMP_NORMAL:
      return pdMS_TO_TICKS(2000);   
    case LED_MODE_TEMP_HOT:
      return pdMS_TO_TICKS(500);   
    case LED_MODE_BOOT:
      return pdMS_TO_TICKS(300);
    case LED_MODE_ERROR:
    default:
      return pdMS_TO_TICKS(300);
  }
}

void led_manager_task(void *pvParameters) {
  app_context_t *ctx = static_cast<app_context_t *>(pvParameters);
  if (ctx == nullptr || ctx->ledQueue == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  pinMode(LED_GPIO, OUTPUT);

  bool ledLevel = false;
  bool enabled = true;
  bool errorActive = false;
  led_mode_t currentMode = LED_MODE_BOOT;
  led_mode_t baseMode = LED_MODE_TEMP_NORMAL;
  led_command_t cmd{};

  led_apply(false);

  while (true) {
    TickType_t waitTicks = enabled ? led_half_period_ticks(currentMode) : pdMS_TO_TICKS(200);

    if (xQueueReceive(ctx->ledQueue, &cmd, waitTicks) == pdTRUE) {
      switch (cmd.type) {
        case LED_CMD_SENSOR_UPDATE:
          baseMode = classify_temperature_mode(cmd.temperature);
          if (!errorActive) {
            currentMode = baseMode;
          }
          break;

        case LED_CMD_WIFI_CONNECTED:
          if (enabled && !errorActive) {
            play_wifi_success_pattern();
            currentMode = baseMode;
            ledLevel = false;
            led_apply(false);
          }
          break;

        case LED_CMD_MQTT_CONNECTED:
          if (enabled && !errorActive) {
            play_mqtt_success_pattern();
            currentMode = baseMode;
            ledLevel = false;
            led_apply(false);
          }
          break;

        case LED_CMD_SET_ENABLE:
          enabled = cmd.enabled;
          if (!enabled) {
            ledLevel = false;
            led_apply(false);
          } else if (!errorActive) {
            currentMode = baseMode;
          }
          break;

        case LED_CMD_ERROR_ON:
          errorActive = true;
          currentMode = LED_MODE_ERROR;
          ledLevel = false;
          led_apply(false);
          break;

        case LED_CMD_ERROR_CLEAR:
          errorActive = false;
          currentMode = baseMode;
          break;
      }
      continue;
    }

    if (!enabled) {
      led_apply(false);
      ledLevel = false;
      continue;
    }

    if (currentMode == LED_MODE_ERROR) {
      led_apply(false);
      ledLevel = false;
      continue;
    }

    ledLevel = !ledLevel;
    led_apply(ledLevel);
  }
}
