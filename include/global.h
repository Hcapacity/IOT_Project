#ifndef __GLOBAL_H__
#define __GLOBAL_H__

#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define I2C_SDA_PIN 11
#define I2C_SCL_PIN 12
#define BOOT_PIN 0

extern float glob_temperature;
extern float glob_humidity;


// extern String ssid;
// extern String password;
// extern String wifi_ssid;
// extern String wifi_password;
// extern boolean isWifiConnected;


extern boolean isWifiConnected;
extern SemaphoreHandle_t xBinarySemaphoreInternet;

// ===== Sensor payload =====
typedef struct {
  float temperature;
  float humidity;
  TickType_t timestamp;
} sensor_data_t;

// ===== LED manager =====
typedef enum {
  LED_MODE_BOOT = 0,
  LED_MODE_TEMP_COLD,
  LED_MODE_TEMP_NORMAL,
  LED_MODE_TEMP_HOT,
  LED_MODE_ERROR
} led_mode_t;

typedef enum {
  LED_CMD_SENSOR_UPDATE = 0,
  LED_CMD_WIFI_CONNECTED,
  LED_CMD_MQTT_CONNECTED,
  LED_CMD_SET_ENABLE,
  LED_CMD_ERROR_ON,
  LED_CMD_ERROR_CLEAR
} led_cmd_type_t;

typedef struct {
  led_cmd_type_t type;
  float temperature;
  bool enabled;
} led_command_t;

// ===== NeoPixel manager =====
typedef enum {
  NEO_CMD_SENSOR_UPDATE = 0,
  NEO_CMD_SET_ENABLE
} neo_cmd_type_t;

typedef struct {
  neo_cmd_type_t type;
  float humidity;
  bool enabled;
} neo_command_t;

// ===== Application context =====
typedef struct {
  QueueHandle_t ledQueue;
  QueueHandle_t neoQueue;
  QueueHandle_t lcdQueue;
  QueueHandle_t webQueue;
  QueueHandle_t coreQueue;
  SemaphoreHandle_t i2cMutex;
  SemaphoreHandle_t internetSemaphore;
} app_context_t;

led_mode_t classify_temperature_mode(float temperature);
const char *classify_environment_status(float temperature, float humidity);

#endif