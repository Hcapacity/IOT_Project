#ifndef __TEMP_HUMI_MONITOR__
#define __TEMP_HUMI_MONITOR__

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include "LiquidCrystal_I2C.h"
#include "DHT20.h"
#include "global.h"

void sensor_task(void *pvParameters);
void lcd_task(void *pvParameters);

#endif
