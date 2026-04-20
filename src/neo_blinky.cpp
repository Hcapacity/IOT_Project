#include "neo_blinky.h"
#include <math.h>

static uint32_t humidity_to_color(Adafruit_NeoPixel &strip, float humidity) {
  // Giới hạn về 0..100
  humidity = constrain(humidity, 0.0f, 100.0f);

  // Đổi theo từng 1%
  int h = (int)roundf(humidity); // 0..100

  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  if (h <= 50) {
    // 0..50%: đỏ -> xanh lá
    float t = h / 50.0f;
    r = (uint8_t)roundf(255.0f * (1.0f - t));
    g = (uint8_t)roundf(255.0f * t);
    b = 0;
  } else {
    // 51..100%: xanh lá -> xanh dương
    float t = (h - 50) / 50.0f;
    r = 0;
    g = (uint8_t)roundf(255.0f * (1.0f - t));
    b = (uint8_t)roundf(255.0f * t);
  }

  return strip.Color(r, g, b);
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

        if (!enabled) {
          strip.clear();
          strip.show();
        }
        continue;
      }

      if (cmd.type == NEO_CMD_SENSOR_UPDATE) {
        if (!enabled) {
          strip.clear();
          strip.show();
          continue;
        }

        strip.setPixelColor(0, humidity_to_color(strip, cmd.humidity));
        strip.show();
      }
    }
  }
}
