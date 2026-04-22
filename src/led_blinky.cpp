#include "led_blinky.h"

static void led_apply(bool level) {
  digitalWrite(LED_GPIO, level ? HIGH : LOW);
}

static TickType_t led_half_period_ticks(led_mode_t mode) {
  switch (mode) {
    case LED_MODE_TEMP_COLD:
      return pdMS_TO_TICKS(5000);
    case LED_MODE_TEMP_NORMAL:
      return pdMS_TO_TICKS(1000);
    case LED_MODE_TEMP_HOT:
      return pdMS_TO_TICKS(100);
    case LED_MODE_ERROR:
      return pdMS_TO_TICKS(120);
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
  led_apply(false);

  bool ledLevel = false;
  bool enabled = true;
  led_mode_t currentMode = LED_MODE_BOOT;
  led_command_t cmd{};

  int pulseTogglesRemaining = 0;
  TickType_t pulseTicks = pdMS_TO_TICKS(100);

  while (true) {
    TickType_t waitTicks = pdMS_TO_TICKS(200);

    if (pulseTogglesRemaining > 0) {
      waitTicks = pulseTicks;
    } else if (enabled) {
      waitTicks = led_half_period_ticks(currentMode);
    }

    if (xQueueReceive(ctx->ledQueue, &cmd, waitTicks) == pdTRUE) {
      switch (cmd.type) {
        case LED_CMD_SENSOR_UPDATE:
          currentMode = classify_temperature_mode(cmd.temperature);
          break;

        case LED_CMD_SET_ENABLE:
          enabled = cmd.enabled;
          if (!enabled) {
            ledLevel = false;
            led_apply(false);
          }
          break;

        case LED_CMD_WIFI_CONNECTED:
          pulseTogglesRemaining = 6;   // 3 quick blinks
          pulseTicks = pdMS_TO_TICKS(80);
          break;

        case LED_CMD_ERROR_ON:
          pulseTogglesRemaining = 12;  // 6 quick blinks
          pulseTicks = pdMS_TO_TICKS(60);
          break;

        case LED_CMD_ERROR_CLEAR:
          pulseTogglesRemaining = 0;
          ledLevel = false;
          led_apply(false);
          break;

        default:
          break;
      }
      continue;
    }

    if (!enabled) {
      led_apply(false);
      ledLevel = false;
      continue;
    }

    if (pulseTogglesRemaining > 0) {
      ledLevel = !ledLevel;
      led_apply(ledLevel);
      pulseTogglesRemaining--;
      if (pulseTogglesRemaining == 0) {
        ledLevel = false;
        led_apply(false);
      }
      continue;
    }

    ledLevel = !ledLevel;
    led_apply(ledLevel);
  }
}