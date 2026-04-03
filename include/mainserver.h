#ifndef __MAIN_SERVER_H__
#define __MAIN_SERVER_H__

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "global.h"

#define BOOT_PIN 0

#define AP_SSID              "Hoang_ESP32_Config"
#define AP_PASSWORD          "0367994254"
#define STA_CONNECT_TIMEOUT  15000UL

#define WIFI_STORE_NAMESPACE "wifi_cfg"
#define WIFI_MAX_SAVED       5

void main_server_task(void *pvParameters);

#endif