#include "telemetry/logger.h"

#if defined(TELEMETRY_UPLOAD) && TELEMETRY_UPLOAD

#include <Arduino.h>
#include <LittleFS.h>
#include <time.h>

#include "telemetry/line_protocol.h"

namespace telemetry {
namespace {

// Segment sizing: the stock 4 MB partition table gives LittleFS ~1.4 MB. Two
// 512 KB segments (~1 MB) leave comfortable filesystem headroom. At ~50 B/min
// each segment holds ~7 days, so the pair spans ~2 weeks — matching the sink's
// 14-day out-of-order window.
constexpr size_t kSegBytes = 512 * 1024;
const char* kSegA = "/tlog.a";
const char* kSegB = "/tlog.b";
const char* kMeta = "/tlog.meta";

// Below this epoch the RTC is clearly unset (2023-11-14); above it, treat the
// clock as SNTP-synced and stamp absolute time.
constexpr uint32_t kMinValidEpoch = 1700000000u;

// LittleFS-backed FileStore. Device-only; the host tests use an in-memory fake.
class LittleFsStore : public FileStore {
 public:
  bool append(const char* path, const uint8_t* data, size_t len) override {
    File f = LittleFS.open(path, "a");
    if (!f) return false;
    size_t n = f.write(data, len);
    f.close();
    return n == len;
  }
  size_t size(const char* path) override {
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t s = f.size();
    f.close();
    return s;
  }
  size_t read(const char* path, size_t offset, uint8_t* buf,
              size_t len) override {
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    if (!f.seek(offset)) {
      f.close();
      return 0;
    }
    int n = f.read(buf, len);
    f.close();
    return n < 0 ? 0 : (size_t)n;
  }
  bool writeAll(const char* path, const uint8_t* data, size_t len) override {
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    bool ok = (len == 0) || (f.write(data, len) == len);
    f.close();
    return ok;
  }
};

LittleFsStore g_store;

}  // namespace

bool Logger::begin() {
  // Mount, formatting on first use / corruption so a fresh board just works.
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    Serial.println("[telemetry] LittleFS mount failed; logging disabled");
    ready_ = false;
    return false;
  }
  static RingLog ring(g_store, kSegA, kSegB, kMeta, kSegBytes);
  ring.begin();
  ring_ = &ring;
  ready_ = true;
  Serial.printf("[telemetry] logging ready (pending %u B, dropped %u)\n",
                (unsigned)ring_->pendingBytes(),
                (unsigned)ring_->droppedRecords());
  return true;
}

bool Logger::log(const Sample& s) {
  if (!ready_ || !ring_ || !s.any()) return false;

  time_t now = time(nullptr);
  bool synced = (uint32_t)now >= kMinValidEpoch;
  bool absolute = synced;
  uint32_t ts = synced ? (uint32_t)now : (uint32_t)(millis() / 1000);

  // A full record (flag + ts + all seven fields + newline) is well under 160 B.
  char rec[160];
  int n = encodeRecord(s, absolute, ts, rec, sizeof(rec));
  if (n < 0) return false;
  return ring_->append((const uint8_t*)rec, (size_t)n);
}

uint32_t Logger::droppedRecords() const {
  return ready_ && ring_ ? ring_->droppedRecords() : 0;
}

size_t Logger::pendingBytes() {
  return ready_ && ring_ ? ring_->pendingBytes() : 0;
}

}  // namespace telemetry

#endif  // TELEMETRY_UPLOAD
