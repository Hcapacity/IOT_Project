#include "global.h"
float glob_temperature = 0;
float glob_humidity = 0;


// String ssid = "ESP32-YOUR NETWORK HERE!!!";
// String password = "12345678";
// String wifi_ssid = "abcde";
// String wifi_password = "123456789";
boolean isWifiConnected = false;
SemaphoreHandle_t xBinarySemaphoreInternet = xSemaphoreCreateBinary();

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