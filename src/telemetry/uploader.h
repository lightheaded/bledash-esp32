// Telemetry uploader (T2 WiFi/SNTP + T3 backlog drain). Device-only; compiled
// only when TELEMETRY_UPLOAD is enabled.
//
// Joins WiFi as a station, syncs the clock over SNTP, and drains the ring log to
// the configured line-protocol endpoint over HTTPS with basic auth. The upload
// cursor advances only after a 2xx, so a dropped connection re-sends rather than
// loses. Records logged before this boot's clock sync carry a boot-relative
// timestamp; they are resolved to real time here using this boot's epoch offset
// (bootEpoch = now − seconds-since-boot). See
// plans/2026-07-24-01-telemetry-logging-upload.md.
//
// Coexistence: this runs always-on STA alongside NimBLE (the C3 shares one
// radio). Uploads are blocking POSTs from loop() — a missed BLE scan slice just
// makes readings ~2 min stale, which the poll cadence tolerates. If always-on
// proves unstable against the GATT session, the fallback is upload windows
// (pause BLE, drain, resume) — see the plan's coexistence section.
#pragma once

#include "config.h"

#if defined(TELEMETRY_UPLOAD) && TELEMETRY_UPLOAD

#include <stddef.h>
#include <stdint.h>

#include "telemetry/logger.h"

namespace telemetry {

class Uploader {
 public:
  // Start WiFi association and SNTP. `logger` must outlive the uploader.
  void begin(Logger& logger);

  // Call every loop. Manages WiFi reconnect + clock sync, and drains the
  // backlog on a timer when connected and synced. Cheap when there's nothing to
  // do (returns quickly without touching the radio).
  void poll();

  bool wifiConnected() const;
  bool clockSynced() const { return bootEpochValid_; }

 private:
  // Scan and join the highest-priority configured network currently in range
  // (WIFI_AP_LIST order = priority). No-op if none are visible.
  void tryConnect();

  // Push as much of the backlog as the endpoint accepts, one chunked POST at a
  // time, advancing the cursor on each 2xx. Returns the number of records sent.
  size_t drain();
  // POST one body of wire lines. Returns the HTTP status (<0 on transport
  // error).
  int post(const char* body, size_t len);

  Logger* logger_ = nullptr;
  bool bootEpochValid_ = false;
  uint32_t bootEpoch_ = 0;          // wall epoch of seconds-since-boot 0
  uint32_t lastDrainMs_ = 0;
  uint32_t lastWifiTryMs_ = 0;
  bool sntpStarted_ = false;
};

}  // namespace telemetry

#endif  // TELEMETRY_UPLOAD
