#ifndef APP_TIME_UTILS_H
#define APP_TIME_UTILS_H

#include <Arduino.h>

struct AppDateTime {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint32_t epochSec;
};

bool appTimeIsEpochValid(uint32_t epochSec, uint32_t minEpochSec = 1704067200U);

bool appTimeNow(
  int32_t gmtOffsetSec,
  int32_t daylightOffsetSec,
  AppDateTime &out,
  uint32_t minEpochSec = 1704067200U
);

bool appTimeSyncNtp(
  const char *ntpServer,
  int32_t gmtOffsetSec,
  int32_t daylightOffsetSec,
  uint32_t minEpochSec = 1704067200U,
  uint32_t timeoutMs = 30000U,
  uint32_t pollMs = 500U
);

void appTimeFormatIso(const AppDateTime &dt, char *out, size_t outSize);

#endif
