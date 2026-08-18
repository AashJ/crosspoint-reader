#include "TrustedTime.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_sntp.h>

#include <ctime>

namespace trustedtime {

namespace {

// Below this the clock was obviously never set (2025-01-01 UTC).
constexpr int64_t MIN_VALID_EPOCH = 1735689600LL;
// NVS wear guard: only rewrite the floor when it moved by at least this much.
constexpr int64_t MIN_ADVANCE_SECS = 60;

constexpr const char* PREFS_NAMESPACE = "cptime";
constexpr const char* PREFS_KEY = "floor";

int64_t readFloor() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/true)) return 0;
  const int64_t value = prefs.getLong64(PREFS_KEY, 0);
  prefs.end();
  return value;
}

void writeFloor(const int64_t value) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false)) return;
  prefs.putLong64(PREFS_KEY, value);
  prefs.end();
}

// SNTP sync callback (lwIP task context; Preferences/NVS is mutex-guarded).
void onTimeSynced(struct timeval*) { note(); }

void configureSntp() {
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
}

}  // namespace

void init() {
  sntp_set_time_sync_notification_cb(&onTimeSynced);
  const int64_t floor = readFloor();
  if (floor < MIN_VALID_EPOCH) return;
  if (static_cast<int64_t>(time(nullptr)) < floor) {
    // Cold boot reset the clock; resume from the floor so time keeps moving
    // forward across power cycles instead of restarting at epoch 0.
    timeval tv = {static_cast<time_t>(floor), 0};
    settimeofday(&tv, nullptr);
    LOG_DBG("TIME", "Clock restored to persisted floor");
  }
}

void note() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  if (now < MIN_VALID_EPOCH) return;
  if (now - readFloor() >= MIN_ADVANCE_SECS) writeFloor(now);
}

void startSync() {
  if (esp_sntp_enabled()) return;  // running; the sync callback handles the rest
  configureSntp();
}

bool syncNow(const uint32_t timeoutMs) {
  // SNTP cannot be reconfigured while running.
  if (esp_sntp_enabled()) esp_sntp_stop();
  configureSntp();
  const unsigned long deadline = millis() + timeoutMs;
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED &&
         static_cast<long>(deadline - millis()) > 0) {
    delay(100);
  }
  const bool synced = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
  if (synced) note();
  return synced;
}

int64_t trustedNow() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  return now >= MIN_VALID_EPOCH ? now : 0;
}

}  // namespace trustedtime
