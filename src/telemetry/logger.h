// Device-side telemetry facade: owns a LittleFS-backed ring log and turns the
// per-cycle Sample into a stored record. Only compiled when TELEMETRY_UPLOAD is
// enabled — with the flag off this header is empty and the ring logger, codec,
// and LittleFS pull in nothing (linker-stripped), exactly like ECOFLOW_GATT.
//
// T1 scope: logging to flash only. Timestamps are absolute epoch once the RTC
// looks set (SNTP lands in T3 with the uploader) and boot-relative before then;
// relative records are resolved to real time at upload, so no in-place rewrite
// is needed. The uploader (backlog drain over WiFi/HTTPS) is T3.
// See plans/2026-07-24-01-telemetry-logging-upload.md.
#pragma once

#include "config.h"

#if defined(TELEMETRY_UPLOAD) && TELEMETRY_UPLOAD

#include <stddef.h>
#include <stdint.h>

#include "telemetry/ring_log.h"
#include "telemetry/sample.h"

namespace telemetry {

class Logger {
 public:
  // Mount LittleFS and load/initialise the ring. Returns false if the flash
  // filesystem can't be mounted (logging then no-ops rather than crashing).
  bool begin();

  // Append one sample as a record. No-op if !ready or the sample is empty.
  // Chooses an absolute or boot-relative timestamp based on whether the clock
  // has been set. Returns false on I/O error.
  bool log(const Sample& s);

  bool ready() const { return ready_; }
  uint32_t droppedRecords() const;  // 0 if !ready
  size_t pendingBytes();            // 0 if !ready

  // The backing ring, for the uploader's drain loop (readBatch/commit). Null
  // until begin() succeeds.
  RingLog* ring() { return ring_; }

 private:
  bool ready_ = false;
  RingLog* ring_ = nullptr;  // constructed in begin() once FS is up
};

}  // namespace telemetry

#endif  // TELEMETRY_UPLOAD
