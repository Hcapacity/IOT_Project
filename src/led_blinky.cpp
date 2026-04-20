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
        default:
            // fallback an toàn
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

    while (true) {
        TickType_t waitTicks = enabled
                               ? led_half_period_ticks(currentMode)
                               : pdMS_TO_TICKS(200);

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

        ledLevel = !ledLevel;
        led_apply(ledLevel);
    }
}
