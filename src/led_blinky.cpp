#include "led_blinky.h"

namespace {

void applyLedOutput(bool isOn) {
  digitalWrite(LED_GPIO, isOn ? HIGH : LOW);
}

TickType_t getBlinkHalfPeriodTicks(led_mode_t mode) {
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

void turnLedOff(bool &ledIsOn) {
  ledIsOn = false;
  applyLedOutput(false);
}

void startPulsePattern(int &remainingToggles, TickType_t &pulseIntervalTicks, int toggleCount, TickType_t intervalTicks) {
  remainingToggles = toggleCount;
  pulseIntervalTicks = intervalTicks;
}

TickType_t getQueueWaitTicks(bool isEnabled, led_mode_t mode, int remainingPulseToggles, TickType_t pulseIntervalTicks) {
  if (remainingPulseToggles > 0) {
    return pulseIntervalTicks;
  }

  if (isEnabled) {
    return getBlinkHalfPeriodTicks(mode);
  }

  return pdMS_TO_TICKS(200);
}

void handleLedCommand(
  const led_command_t &command,
  bool &isEnabled,
  bool &ledIsOn,
  led_mode_t &currentMode,
  int &remainingPulseToggles,
  TickType_t &pulseIntervalTicks
) {
  switch (command.type) {
    case LED_CMD_SENSOR_UPDATE:
      currentMode = classify_temperature_mode(command.temperature);
      break;

    case LED_CMD_SET_ENABLE:
      isEnabled = command.enabled;
      if (!isEnabled) {
        turnLedOff(ledIsOn);
      }
      break;

    case LED_CMD_WIFI_CONNECTED:
      startPulsePattern(remainingPulseToggles, pulseIntervalTicks, 6, pdMS_TO_TICKS(80));
      break;

    case LED_CMD_ERROR_ON:
      startPulsePattern(remainingPulseToggles, pulseIntervalTicks, 12, pdMS_TO_TICKS(60));
      break;

    case LED_CMD_ERROR_CLEAR:
      remainingPulseToggles = 0;
      turnLedOff(ledIsOn);
      break;

    default:
      break;
  }
}

void processPulseStep(bool &ledIsOn, int &remainingPulseToggles) {
  ledIsOn = !ledIsOn;
  applyLedOutput(ledIsOn);
  remainingPulseToggles--;

  if (remainingPulseToggles == 0) {
    turnLedOff(ledIsOn);
  }
}

void processNormalBlinkStep(bool &ledIsOn) {
  ledIsOn = !ledIsOn;
  applyLedOutput(ledIsOn);
}

}  // namespace

void led_manager_task(void *pvParameters) {
  app_context_t *appContext = static_cast<app_context_t *>(pvParameters);
  if (appContext == nullptr || appContext->ledQueue == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  pinMode(LED_GPIO, OUTPUT);

  bool ledIsOn = false;
  bool isBlinkEnabled = true;
  led_mode_t currentBlinkMode = LED_MODE_BOOT;
  led_command_t incomingCommand{};

  int remainingPulseToggles = 0;
  TickType_t pulseIntervalTicks = pdMS_TO_TICKS(100);

  turnLedOff(ledIsOn);

  while (true) {
    TickType_t waitTicks = getQueueWaitTicks(
      isBlinkEnabled,
      currentBlinkMode,
      remainingPulseToggles,
      pulseIntervalTicks
    );

    if (xQueueReceive(appContext->ledQueue, &incomingCommand, waitTicks) == pdTRUE) {
      handleLedCommand(
        incomingCommand,
        isBlinkEnabled,
        ledIsOn,
        currentBlinkMode,
        remainingPulseToggles,
        pulseIntervalTicks
      );
      continue;
    }

    if (!isBlinkEnabled) {
      turnLedOff(ledIsOn);
      continue;
    }

    if (remainingPulseToggles > 0) {
      // Pulse patterns are used as short status notifications.
      // If this branch is mixed with the normal blink path, Wi-Fi/error feedback becomes hard to read.
      processPulseStep(ledIsOn, remainingPulseToggles);
      continue;
    }

    processNormalBlinkStep(ledIsOn);
  }
}
