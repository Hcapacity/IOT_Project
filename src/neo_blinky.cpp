#include "neo_blinky.h"

#include <math.h>

namespace {

uint32_t mapHumidityToColor(Adafruit_NeoPixel &strip, float humidityPercent) {
  humidityPercent = constrain(humidityPercent, 0.0f, 100.0f);
  const int roundedHumidityPercent = static_cast<int>(roundf(humidityPercent));

  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;

  if (roundedHumidityPercent <= 50) {
    const float blendRatio = roundedHumidityPercent / 50.0f;
    red = static_cast<uint8_t>(roundf(255.0f * (1.0f - blendRatio)));
    green = static_cast<uint8_t>(roundf(255.0f * blendRatio));
  } else {
    const float blendRatio = (roundedHumidityPercent - 50) / 50.0f;
    green = static_cast<uint8_t>(roundf(255.0f * (1.0f - blendRatio)));
    blue = static_cast<uint8_t>(roundf(255.0f * blendRatio));
  }

  return strip.Color(red, green, blue);
}

void clearStrip(Adafruit_NeoPixel &strip) {
  strip.clear();
  strip.show();
}

void applyHumidityColor(Adafruit_NeoPixel &strip, float humidityPercent) {
  strip.setPixelColor(0, mapHumidityToColor(strip, humidityPercent));
  strip.show();
}

}  // namespace

void neo_pixel_task(void *pvParameters) {
  app_context_t *appContext = static_cast<app_context_t *>(pvParameters);
  if (appContext == nullptr || appContext->neoQueue == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  Adafruit_NeoPixel statusStrip(LED_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);
  statusStrip.begin();
  clearStrip(statusStrip);

  bool isStripEnabled = true;
  neo_command_t incomingCommand{};

  while (true) {
    if (xQueueReceive(appContext->neoQueue, &incomingCommand, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (incomingCommand.type == NEO_CMD_SET_ENABLE) {
      isStripEnabled = incomingCommand.enabled;

      if (!isStripEnabled) {
        clearStrip(statusStrip);
      }
      continue;
    }

    if (incomingCommand.type == NEO_CMD_SENSOR_UPDATE) {
      if (!isStripEnabled) {
        // Keep the LED physically off while disabled.
        // If we only skip updates here, the previous color would remain latched on the NeoPixel.
        clearStrip(statusStrip);
        continue;
      }

      applyHumidityColor(statusStrip, incomingCommand.humidity);
    }
  }
}
