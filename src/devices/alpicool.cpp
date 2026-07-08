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

bool AlpicoolDriver::matches(const NimBLEAdvertisedDevice* dev) const {
  String a = dev->getAddress().toString().c_str();
  a.toLowerCase();
  return a == targetMac_;
}

void AlpicoolDriver::onDisconnect(NimBLEClient*, int reason) {
  Serial.printf("[alpicool] disconnected (reason=%d)\n", reason);
  writeChar_ = nullptr;
  notifyChar_ = nullptr;
  buf_.clear();
}

bool AlpicoolDriver::discoverChars() {
  writeChar_ = nullptr;
  notifyChar_ = nullptr;
  // The write/notify chars may live under service 0x1234 or 0xFFF0 depending on
  // firmware, so search all services by characteristic UUID rather than assuming.
  const std::vector<NimBLERemoteService*>& services = client_->getServices(true);
  for (NimBLERemoteService* svc : services) {
    for (NimBLERemoteCharacteristic* chr : svc->getCharacteristics(true)) {
      const NimBLEUUID& u = chr->getUUID();
      if (u == kWriteChar) writeChar_ = chr;
      if (u == kNotifyChar) notifyChar_ = chr;
    }
  }
  return writeChar_ && notifyChar_;
}

bool AlpicoolDriver::connectTo(const NimBLEAdvertisedDevice* dev) {
  if (connected()) return true;
  Serial.printf("[alpicool] found, RSSI=%d dBm; connecting...\n", dev->getRSSI());

  if (!client_) {
    client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(this, false);
    client_->setConnectTimeout(4 * 1000);  // keep a failed connect from freezing the loop
    // Relaxed params with a long (5 s) supervision timeout so brief packet loss
    // over a marginal link doesn't drop the connection. Args: min/max interval
    // (×1.25 ms), latency, supervision timeout (×10 ms).
    client_->setConnectionParams(24, 48, 0, 500);
  }

  if (!client_->connect(dev)) {
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
    Serial.println("[alpicool] characteristics not found");
    if (connected()) client_->disconnect();
    return false;
  }

  notifyChar_->subscribe(
      true, [this](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool) {
        this->onNotify(d, l);
      });

  // BIND once after connecting, then let poll() drive periodic QUERY.
  sendPacket(kBind, sizeof(kBind));
  lastQueryMs_ = 0;  // force an immediate QUERY on the next poll()
  return true;
}

void AlpicoolDriver::sendPacket(const uint8_t* data, size_t len) {
  if (!writeChar_) return;
  bool response = writeChar_->canWrite();
  writeChar_->writeValue(data, len, response);
}

void AlpicoolDriver::onNotify(const uint8_t* data, size_t len) {
  buf_.insert(buf_.end(), data, data + len);

  // Extract complete FE FE frames. Frame = [FE FE][len][cmd][payload][cksum:2];
  // len counts everything after the 2-byte header, so total = 3 + len.
  while (true) {
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

void AlpicoolDriver::poll() {
  if (!connected()) return;
  uint32_t now = millis();
  if (now - lastQueryMs_ >= queryIntervalMs_ || lastQueryMs_ == 0) {
    lastQueryMs_ = now;
    sendPacket(kQuery, sizeof(kQuery));
  }
}
