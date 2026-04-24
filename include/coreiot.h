#ifndef __COREIOT_H__
#define __COREIOT_H__

#include <Arduino.h>
#include <PubSubClient.h>
#include "global.h"

void coreiot_task(void *pvParameters);

// Hook để đổi mode LCD từ CoreIOT button
void coreiot_set_lcd_forecast_mode(bool enabled);

void coreiot_set_publish_enabled(bool enabled);
bool coreiot_get_publish_enabled();
bool coreiot_set_broker_host(const char *host);
void coreiot_get_broker_host(char *outHost, size_t outSize);
bool coreiot_set_credentials(const char *username, const char *password);
void coreiot_get_credentials(char *outUsername, size_t usernameSize, char *outPassword, size_t passwordSize);

#endif