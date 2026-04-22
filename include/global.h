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

// ===== Sensor payload =====
typedef struct {
  float temperature;
  float humidity;
  TickType_t timestamp;
} sensor_data_t;

// ===== TinyML result =====
typedef struct {
  float rainProbability;      // 0.0 -> 1.0
  bool isRain;
  TickType_t sensorTimestamp;
  TickType_t inferTimestamp;
} tinyml_result_t;

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

// ===== LCD view mode =====
typedef enum {
  LCD_VIEW_SENSOR = 0,
  LCD_VIEW_WIFI = 1
} lcd_view_mode_t;

// ===== NEW: LCD content mode controlled by CoreIOT =====
typedef enum {
  LCD_CONTENT_SENSOR = 0,   // normal: temp/humi + ESP mode
  LCD_CONTENT_TINYML = 1    // tinyml forecast + ESP mode
} lcd_content_mode_t;

typedef struct {
  float latestTemperature;
  float latestHumidity;
  bool wifiConnected;
  lcd_content_mode_t lcdContentMode;
} app_shared_state_t;

// ===== Application context =====
typedef struct {
  QueueHandle_t ledQueue;
  QueueHandle_t neoQueue;
  QueueHandle_t lcdQueue;
  QueueHandle_t webQueue;
  QueueHandle_t coreQueue;
  QueueHandle_t tinyMLQueue;
  QueueHandle_t tinyResultQueue;
  SemaphoreHandle_t i2cMutex;
  SemaphoreHandle_t stateMutex;
  SemaphoreHandle_t internetSemaphore;
  app_shared_state_t sharedState;
} app_context_t;

void init_app_shared_state(app_context_t *ctx);
void app_set_latest_sensor(app_context_t *ctx, float temperature, float humidity);
void app_set_wifi_connected(app_context_t *ctx, bool connected);
bool app_get_wifi_connected(app_context_t *ctx);
void app_set_lcd_content_mode(app_context_t *ctx, lcd_content_mode_t mode);
lcd_content_mode_t app_get_lcd_content_mode(app_context_t *ctx);

led_mode_t classify_temperature_mode(float temperature);
const char *classify_environment_status(float temperature, float humidity);

#endif
