#ifndef __COREIOT_H__
#define __COREIOT_H__

#include <Arduino.h>
#include <PubSubClient.h>
#include "global.h"

void coreiot_task(void *pvParameters);

// Hook để đổi mode LCD từ CoreIOT button
void coreiot_set_lcd_forecast_mode(bool enabled);

#endif