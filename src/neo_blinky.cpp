#include "neo_blinky.h"

// NeoPixel control task
static uint32_t humidity_to_color(Adafruit_NeoPixel &strip, float humidity) {
  if (humidity < 30.0f) {
    return strip.Color(255, 0, 0);   // dry
  }
  else if (humidity < 70.0f) {
    return strip.Color(0, 255, 0);   // normal
  }
  else {
    return strip.Color(0, 0, 255);   // humid
  }
}

void neo_pixel_task(void *pvParameters) {
  app_context_t *ctx = static_cast<app_context_t *>(pvParameters);
  if (ctx == nullptr || ctx->neoQueue == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  Adafruit_NeoPixel strip(LED_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);
  strip.begin();
  strip.clear();
  strip.show();

  bool enabled = true;
  neo_command_t cmd{};

  while (true) {
    if (xQueueReceive(ctx->neoQueue, &cmd, portMAX_DELAY) == pdTRUE) {
      if (cmd.type == NEO_CMD_SET_ENABLE) {
        enabled = cmd.enabled;
      }

      if (!enabled) {
        strip.clear();
        strip.show();
        continue;
      }

      float humidity = cmd.humidity;
      strip.setPixelColor(0, humidity_to_color(strip, humidity));
      strip.show();
    }
  }
}
