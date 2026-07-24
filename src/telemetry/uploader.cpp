#include "telemetry/uploader.h"

#if defined(TELEMETRY_UPLOAD) && TELEMETRY_UPLOAD

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include <string>
#include <vector>

#include "telemetry/isrg_root_x1.h"
#include "telemetry/line_protocol.h"
#include "telemetry/ring_log.h"

namespace telemetry {
namespace {

constexpr uint32_t kUploadIntervalMs = 60000;  // drain cadence when synced
constexpr uint32_t kWifiRetryMs = 15000;       // reconnect attempt interval
constexpr size_t kMaxLinesPerPost = 300;       // ~16 KB of line protocol
constexpr size_t kMaxBytesPerPost = 16000;
// Matches Logger's threshold: below this epoch the RTC is clearly unset.
constexpr uint32_t kMinValidEpoch = 1700000000u;

const char* caPem() {
#ifdef TELEMETRY_CA_PEM
  return TELEMETRY_CA_PEM;  // user-supplied CA for a non-Let's-Encrypt sink
#else
  return kIsrgRootX1Pem;
#endif
}

}  // namespace

void Uploader::begin(Logger& logger) {
  logger_ = &logger;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // modem sleep — lets BLE and WiFi share the one radio
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[upload] WiFi joining \"%s\"\n", WIFI_SSID);
}

bool Uploader::wifiConnected() const { return WiFi.status() == WL_CONNECTED; }

void Uploader::poll() {
  if (!logger_) return;
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (lastWifiTryMs_ == 0 || now - lastWifiTryMs_ >= kWifiRetryMs) {
      lastWifiTryMs_ = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }

  // Connected: kick off SNTP once, then latch the boot epoch on first sync.
  if (!sntpStarted_) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    sntpStarted_ = true;
    Serial.println("[upload] WiFi up; SNTP started");
  }
  if (!bootEpochValid_) {
    time_t t = time(nullptr);
    if ((uint32_t)t >= kMinValidEpoch) {
      bootEpoch_ = (uint32_t)t - (uint32_t)(now / 1000);
      bootEpochValid_ = true;
      Serial.printf("[upload] clock synced: epoch=%lu bootEpoch=%lu\n",
                    (unsigned long)t, (unsigned long)bootEpoch_);
    }
  }

  if (bootEpochValid_ &&
      (lastDrainMs_ == 0 || now - lastDrainMs_ >= kUploadIntervalMs)) {
    lastDrainMs_ = now;
    if (logger_->pendingBytes() > 0) {
      size_t sent = drain();
      if (sent) {
        Serial.printf("[upload] drained %u records; %u B pending, %u dropped\n",
                      (unsigned)sent, (unsigned)logger_->pendingBytes(),
                      (unsigned)logger_->droppedRecords());
      }
    }
  }
}

size_t Uploader::drain() {
  RingLog* ring = logger_->ring();
  if (!ring) return 0;

  size_t sent = 0;
  char wire[256];
  while (ring->pendingBytes() > 0) {
    std::vector<std::string> lines;
    auto batch = ring->readBatch(kMaxLinesPerPost, kMaxBytesPerPost, lines);
    if (batch.empty()) break;

    std::string body;
    body.reserve(kMaxBytesPerPost + 1024);
    size_t resolved = 0;
    for (const auto& l : lines) {
      Record r;
      if (!parseRecord(l.data(), l.size(), r)) continue;  // corrupt line, skip
      int n = recordToWireLine(r, "bledash", TELEMETRY_DEVICE_TAG, bootEpoch_,
                               bootEpochValid_, wire, sizeof(wire));
      if (n < 0) continue;  // unresolvable/no-fit; skip (committed past below)
      body.append(wire, (size_t)n);
      resolved++;
    }

    // Nothing usable in this batch (e.g. all unresolvable): commit past it so
    // the cursor doesn't wedge on unsendable records.
    if (resolved == 0) {
      ring->commit(batch);
      continue;
    }

    int code = post(body.c_str(), body.size());
    if (code >= 200 && code < 300) {
      ring->commit(batch);
      sent += resolved;
    } else {
      Serial.printf("[upload] POST failed (%d); leaving cursor for retry\n",
                    code);
      break;  // keep the backlog; try again next drain
    }
  }
  return sent;
}

int Uploader::post(const char* body, size_t len) {
  WiFiClientSecure client;
  client.setCACert(caPem());

  HTTPClient http;
  String url = String(TELEMETRY_URL) + "?precision=s";
  if (!http.begin(client, url)) return -1;
  http.setAuthorization(TELEMETRY_USER, TELEMETRY_PASS);
  http.addHeader("Content-Type", "text/plain");

  uint32_t heapBefore = ESP.getFreeHeap();
  int code = http.POST((uint8_t*)body, len);
  Serial.printf("[upload] POST %u B -> %d (free heap %u)\n", (unsigned)len, code,
                (unsigned)heapBefore);
  http.end();
  return code;
}

}  // namespace telemetry

#endif  // TELEMETRY_UPLOAD
