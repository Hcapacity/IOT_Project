#include <Arduino.h>
#include <Wire.h>

#include "global.h"
#include "led_blinky.h"
#include "neo_blinky.h"
#include "temp_humi_monitor.h"
#include "mainserver.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  app_context_t *app = new app_context_t();
  if (app == nullptr) {
    Serial.println("[Main] Failed to allocate app context.");
    return;
  }

  app->ledQueue = xQueueCreate(8, sizeof(led_command_t));
  app->neoQueue = xQueueCreate(1, sizeof(neo_command_t));
  app->lcdQueue = xQueueCreate(1, sizeof(sensor_data_t));
  app->webQueue = xQueueCreate(1, sizeof(sensor_data_t));
  app->i2cMutex = xSemaphoreCreateMutex();
  app->internetSemaphore = xSemaphoreCreateBinary();

  if (app->ledQueue == nullptr || app->neoQueue == nullptr || app->lcdQueue == nullptr ||
      app->webQueue == nullptr || app->i2cMutex == nullptr || app->internetSemaphore == nullptr) {
    Serial.println("[Main] Failed to create queues/semaphores.");
    return;
  }

  xTaskCreate(sensor_task,       "SensorTask",      4096, app, 3, nullptr);
  xTaskCreate(led_manager_task,  "LedManagerTask",  3072, app, 2, nullptr);
  xTaskCreate(neo_pixel_task,    "NeoPixelTask",    3072, app, 2, nullptr);
  xTaskCreate(lcd_task,          "LcdTask",         4096, app, 2, nullptr);
  xTaskCreate(main_server_task,  "WebServerTask",   8192, app, 2, nullptr);

  Serial.println("[Main] RTOS tasks created successfully.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
