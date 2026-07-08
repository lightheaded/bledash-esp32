#include "devices/alpicool.h"

namespace {
// Characteristic UUIDs (16-bit, expand against the Bluetooth base UUID).
const NimBLEUUID kWriteChar((uint16_t)0x1235);
const NimBLEUUID kNotifyChar((uint16_t)0x1236);

// Literal command packets (see docs/protocols/alpicool.md).
const uint8_t kBind[] = {0xFE, 0xFE, 0x03, 0x00, 0x01, 0xFF};
const uint8_t kQuery[] = {0xFE, 0xFE, 0x03, 0x01, 0x02, 0x00};

constexpr uint8_t kCmdQuery = 0x01;

int8_t toSigned(uint8_t b) { return b > 127 ? (int)b - 256 : (int)b; }
}  // namespace

void AlpicoolDriver::begin(const char* mac, uint32_t queryIntervalMs) {
  targetMac_ = mac;
  targetMac_.toLowerCase();
  queryIntervalMs_ = queryIntervalMs;
}

void AlpicoolDriver::onConnect(NimBLEClient*) {
  Serial.println("[alpicool] connected");
}

void AlpicoolDriver::onDisconnect(NimBLEClient*, int reason) {
  Serial.printf("[alpicool] disconnected (reason=%d)\n", reason);
  writeChar_ = nullptr;
  notifyChar_ = nullptr;
  bound_ = false;
  buf_.clear();
}

bool AlpicoolDriver::discoverChars() {
  writeChar_ = nullptr;
  notifyChar_ = nullptr;
  // The write/notify chars may live under service 0x1234 or 0xFFF0 depending on
  // firmware, so search all services by characteristic UUID rather than assuming.
  const std::vector<NimBLERemoteService*>& services = client_->getServices(true);
  Serial.printf("[alpicool] discovered %u services\n", (unsigned)services.size());
  for (NimBLERemoteService* svc : services) {
    for (NimBLERemoteCharacteristic* chr : svc->getCharacteristics(true)) {
      const NimBLEUUID& u = chr->getUUID();
      if (u == kWriteChar) writeChar_ = chr;
      if (u == kNotifyChar) notifyChar_ = chr;
    }
  }
  if (!writeChar_ || !notifyChar_) {
    Serial.println("[alpicool] write/notify characteristics not found yet");
    return false;
  }
  return true;
}

bool AlpicoolDriver::ensureConnected() {
  if (connected()) return true;

  // Confirm the fridge is advertising before attempting to connect. If it isn't,
  // it's out of range or its single connection is held elsewhere (e.g. HA) —
  // don't burn a long connect timeout.
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(3000, false);

  const NimBLEAdvertisedDevice* found = nullptr;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = results.getDevice(i);
    String a = d->getAddress().toString().c_str();
    a.toLowerCase();
    if (a == targetMac_) {
      found = d;
      break;
    }
  }
  scan->clearResults();

  wasAdvertising_ = (found != nullptr);
  if (!found) {
    Serial.println("[alpicool] not advertising (out of range or held elsewhere)");
    return false;
  }
  Serial.printf("[alpicool] found, RSSI=%d dBm\n", found->getRSSI());

  if (!client_) {
    client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(this, false);
    client_->setConnectTimeout(10 * 1000);
    // Relaxed params with a long (5 s) supervision timeout so brief packet loss
    // over a marginal link doesn't drop the connection. Args: min/max interval
    // (×1.25 ms), latency, supervision timeout (×10 ms).
    client_->setConnectionParams(24, 48, 0, 500);
  }

  Serial.println("[alpicool] connecting...");
  if (!client_->connect(found)) {
    Serial.println("[alpicool] connect failed");
    return false;
  }
  // Let the link settle before service discovery; retry a few times in case a
  // weak link needs a moment.
  bool discovered = false;
  for (int attempt = 0; attempt < 3 && connected(); attempt++) {
    delay(300);
    if (discoverChars()) {
      discovered = true;
      break;
    }
  }
  if (!discovered) {
    if (connected()) client_->disconnect();
    return false;
  }

  notifyChar_->subscribe(
      true, [this](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool) {
        this->onNotify(d, l);
      });

  // BIND once after connecting, then let the periodic QUERY drive updates.
  sendPacket(kBind, sizeof(kBind));
  bound_ = true;
  lastQueryMs_ = 0;  // force an immediate QUERY
  return true;
}

void AlpicoolDriver::sendPacket(const uint8_t* data, size_t len) {
  if (!writeChar_) return;
  // Prefer write-with-response if supported, else write-without-response.
  bool response = writeChar_->canWrite();
  writeChar_->writeValue(data, len, response);
}

void AlpicoolDriver::onNotify(const uint8_t* data, size_t len) {
  buf_.insert(buf_.end(), data, data + len);

  // Extract complete FE FE frames. Frame = [FE FE][len][cmd][payload][cksum:2];
  // len counts everything after the 2-byte header, so total = 3 + len.
  while (true) {
    // Find header.
    size_t start = 0;
    bool haveHeader = false;
    for (; start + 1 < buf_.size(); start++) {
      if (buf_[start] == 0xFE && buf_[start + 1] == 0xFE) {
        haveHeader = true;
        break;
      }
    }
    if (!haveHeader) {
      buf_.clear();  // no header in buffer; discard noise
      return;
    }
    if (start > 0) buf_.erase(buf_.begin(), buf_.begin() + start);  // drop preamble

    if (buf_.size() < 3) return;  // need the length byte
    size_t total = 3 + buf_[2];
    if (buf_.size() < total) return;  // wait for the rest of the frame

    handleFrame(buf_.data(), total);
    buf_.erase(buf_.begin(), buf_.begin() + total);
  }
}

void AlpicoolDriver::handleFrame(const uint8_t* frame, size_t total) {
  uint8_t cmd = frame[3];
  if (cmd != kCmdQuery) return;  // ignore echoes of BIND/SET etc.
  // Payload is everything after cmd (the trailing 2 checksum bytes are past the
  // fields we read, so they're harmless to include).
  const uint8_t* payload = frame + 4;
  size_t payloadLen = total - 4;
  parseStatus(payload, payloadLen);
}

void AlpicoolDriver::parseStatus(const uint8_t* p, size_t len) {
  if (len < 15) {
    Serial.printf("[alpicool] short status payload (%u bytes)\n", (unsigned)len);
    return;
  }
  reading_.poweredOn = p[1] != 0;
  reading_.batProtect = p[3];
  reading_.targetTemp = toSigned(p[4]);
  reading_.actualTemp = toSigned(p[14]);
  reading_.valid = true;
  reading_.lastUpdateMs = millis();

  Serial.printf("[alpicool] status: on=%d target=%d°C actual=%d°C batProt=%d\n",
                reading_.poweredOn, reading_.targetTemp, reading_.actualTemp,
                reading_.batProtect);
}

void AlpicoolDriver::loop() {
  uint32_t now = millis();

  if (!connected()) {
    // Retry connecting on a short backoff (independent of the slower poll
    // cadence) so a dropped link recovers quickly.
    const uint32_t kReconnectMs = 5000;
    if (lastConnAttemptMs_ != 0 && now - lastConnAttemptMs_ < kReconnectMs) return;
    lastConnAttemptMs_ = now;
    ensureConnected();
    return;
  }

  if (now - lastQueryMs_ >= queryIntervalMs_ || lastQueryMs_ == 0) {
    lastQueryMs_ = now;
    sendPacket(kQuery, sizeof(kQuery));
  }
}
