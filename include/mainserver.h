#ifndef __MAIN_SERVER_H__
#define __MAIN_SERVER_H__

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "global.h"

#define AP_SSID "Hoang_ESP32_Config"
#define AP_PASSWORD "0367994254"

#define STA_CONNECT_TIMEOUT 10000UL
#define WIFI_STORE_NAMESPACE "wifi_cfg"
#define WIFI_MAX_SAVED 5

#define USER_LED_GPIO 5
#define USER_LED_PWM_CHANNEL 1
#define USER_LED_PWM_FREQ 5000
#define USER_LED_PWM_RESOLUTION 8

void main_server_task(void *pvParameters);

#endif