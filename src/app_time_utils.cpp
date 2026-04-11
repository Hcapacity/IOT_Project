#include "app_time_utils.h"

#include <time.h>

namespace {

bool isLeapYear(int year) {
  return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

int daysInYear(int year) {
  return isLeapYear(year) ? 366 : 365;
}

int daysInMonth(int year, int month) {
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) return 29;
  return kDays[month - 1];
}

bool convertEpochToLocalDateTime(
  uint32_t epochSec,
  int32_t gmtOffsetSec,
  int32_t daylightOffsetSec,
  AppDateTime &out
) {
  const int64_t localEpoch =
      static_cast<int64_t>(epochSec) +
      static_cast<int64_t>(gmtOffsetSec) +
      static_cast<int64_t>(daylightOffsetSec);

  int64_t days = localEpoch / 86400;
  int64_t secOfDay = localEpoch % 86400;
  if (secOfDay < 0) {
    secOfDay += 86400;
    days -= 1;
  }

  int year = 1970;
  while (days >= daysInYear(year)) {
    days -= daysInYear(year);
    year++;
  }
  while (days < 0) {
    year--;
    days += daysInYear(year);
  }

  int month = 1;
  while (month <= 12) {
    const int dim = daysInMonth(year, month);
    if (days < dim) break;
    days -= dim;
    month++;
  }

  if (month < 1 || month > 12) return false;

  out.year = static_cast<uint16_t>(year);
  out.month = static_cast<uint8_t>(month);
  out.day = static_cast<uint8_t>(days + 1);
  out.hour = static_cast<uint8_t>(secOfDay / 3600);
  out.minute = static_cast<uint8_t>((secOfDay % 3600) / 60);
  out.second = static_cast<uint8_t>(secOfDay % 60);
  out.epochSec = epochSec;

  return true;
}

}  // namespace

bool appTimeIsEpochValid(uint32_t epochSec, uint32_t minEpochSec) {
  return epochSec >= minEpochSec;
}

bool appTimeNow(
  int32_t gmtOffsetSec,
  int32_t daylightOffsetSec,
  AppDateTime &out,
  uint32_t minEpochSec
) {
  const uint32_t epochSec = static_cast<uint32_t>(time(nullptr));
  if (!appTimeIsEpochValid(epochSec, minEpochSec)) {
    return false;
  }

  return convertEpochToLocalDateTime(epochSec, gmtOffsetSec, daylightOffsetSec, out);
}

bool appTimeSyncNtp(
  const char *ntpServer,
  int32_t gmtOffsetSec,
  int32_t daylightOffsetSec,
  uint32_t minEpochSec,
  uint32_t timeoutMs,
  uint32_t pollMs
) {
  if (ntpServer == nullptr || ntpServer[0] == '\0') {
    return false;
  }

  configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);

  const uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    const uint32_t epochSec = static_cast<uint32_t>(time(nullptr));
    if (appTimeIsEpochValid(epochSec, minEpochSec)) {
      return true;
    }
    delay(pollMs);
  }

  return false;
}

void appTimeFormatIso(const AppDateTime &dt, char *out, size_t outSize) {
  if (out == nullptr || outSize == 0) return;
  snprintf(out, outSize, "%04u-%02u-%02u %02u:%02u:%02u",
           static_cast<unsigned>(dt.year),
           static_cast<unsigned>(dt.month),
           static_cast<unsigned>(dt.day),
           static_cast<unsigned>(dt.hour),
           static_cast<unsigned>(dt.minute),
           static_cast<unsigned>(dt.second));
}
