#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  _available = _sdkRtc.begin();
  if (_available) {
    LOG_INF("CLK", "External RTC (DS3231) found");
    return;
  }
  // Fall back to the ESP32 internal RTC backed by the system clock (SNTP).
  // Time is meaningful only after the first NTP sync; getTime() returns false
  // until then by checking against MIN_VALID_TIME.
  _useInternalRtc = true;
  _available = true;
  LOG_INF("CLK", "No external RTC; using internal RTC (requires NTP sync)");
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();

  if (_useInternalRtc) {
    // Throttle system-clock reads the same as the external chip.
    if (_hasCachedTime && _lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
      hour = _cachedHour;
      minute = _cachedMinute;
      return true;
    }
    const time_t t = time(nullptr);
    if (t < MIN_VALID_TIME) return false;  // Clock not yet synced
    struct tm ti;
    gmtime_r(&t, &ti);
    _cachedHour = static_cast<uint8_t>(ti.tm_hour);
    _cachedMinute = static_cast<uint8_t>(ti.tm_min);
    _lastPollMs = now;
    _hasCachedTime = true;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      if (_useInternalRtc) {
        // The SNTP stack already updated the system clock via settimeofday().
        // Just refresh our cached values.
        _cachedHour = static_cast<uint8_t>(timeinfo.tm_hour);
        _cachedMinute = static_cast<uint8_t>(timeinfo.tm_min);
        _hasCachedTime = true;
        _lastPollMs = 0;  // Force a fresh read next getTime() call
        LOG_INF("CLK", "System clock set to %04d-%02d-%02d %02d:%02u:%02d UTC", timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _hasCachedTime = true;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
