#include "devices/ecoflow_build.h"
#if ECOFLOW_ENABLE_GATT_TU

#include "devices/ecoflow_session.h"

#include <string.h>

#include "devices/ecoflow_crypto.h"

namespace {
// rfcomm-variant GATT characteristics used by the River 2 series.
const NimBLEUUID kWriteChar("00000002-0000-1000-8000-00805f9b34fb");
const NimBLEUUID kNotifyChar("00000003-0000-1000-8000-00805f9b34fb");

// Manufacturer-data layout for matching (same as the passive monitor).
constexpr uint16_t kCompanyId = 0xB5B5;
constexpr size_t kSerialOffset = 3;
constexpr size_t kSerialLen = 16;

// Inner-packet addressing (from the reference handshake).
constexpr uint8_t kAddrApp = 0x21;   // us
constexpr uint8_t kAddrDev = 0x35;   // the device
constexpr uint8_t kCmdSetAuth = 0x35;
constexpr uint8_t kCmdIdAuthStatus = 0x89;
constexpr uint8_t kCmdIdAuth = 0x86;
constexpr uint8_t kCmdSetTime = 0x01;  // device RTC request
constexpr uint8_t kCmdIdTime = 0x52;

constexpr uint32_t kHandshakeTimeoutMs = 8000;
constexpr uint16_t kWattsIdleThreshold = 5;  // below this either way = idle
}  // namespace

void EcoflowSession::begin(const char* serial, const char* userId) {
  serial_ = serial;
  userId_ = userId;
}

bool EcoflowSession::matches(const NimBLEAdvertisedDevice* dev) const {
  if (!dev->haveManufacturerData()) return false;
  const std::string md = dev->getManufacturerData();
  const uint8_t* p = (const uint8_t*)md.data();
  if (md.size() < kSerialOffset + kSerialLen) return false;
  if ((uint16_t)(p[0] | (p[1] << 8)) != kCompanyId) return false;
  for (size_t i = 0; i < kSerialLen; i++)
    if ((char)p[kSerialOffset + i] != serial_[i]) return false;
  return true;
}

void EcoflowSession::onDisconnect(NimBLEClient*, int reason) {
  Serial.printf("[ecoflow] disconnected (reason=%d)\n", reason);
  writeChar_ = nullptr;
  notifyChar_ = nullptr;
  state_ = State::kDisconnected;
  haveCipher_ = false;
  buf_.clear();
}

bool EcoflowSession::discoverChars() {
  writeChar_ = nullptr;
  notifyChar_ = nullptr;
  for (NimBLERemoteService* svc : client_->getServices(true)) {
    for (NimBLERemoteCharacteristic* chr : svc->getCharacteristics(true)) {
      const NimBLEUUID& u = chr->getUUID();
      if (u == kWriteChar) writeChar_ = chr;
      if (u == kNotifyChar) notifyChar_ = chr;
    }
  }
  return writeChar_ && notifyChar_;
}

bool EcoflowSession::connectTo(const NimBLEAdvertisedDevice* dev) {
  if (connected()) return true;
  Serial.printf("[ecoflow] found, RSSI=%d dBm; connecting...\n", dev->getRSSI());

  if (!client_) {
    client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(this, false);
    client_->setConnectTimeout(4 * 1000);
    client_->setConnectionParams(24, 48, 0, 500);
  }
  if (!client_->connect(dev)) {
    Serial.println("[ecoflow] connect failed");
    return false;
  }

  bool discovered = false;
  for (int attempt = 0; attempt < 3 && connected(); attempt++) {
    delay(300);
    if (discoverChars()) { discovered = true; break; }
  }
  if (!discovered) {
    Serial.println("[ecoflow] rfcomm characteristics not found");
    if (connected()) client_->disconnect();
    return false;
  }

  notifyChar_->subscribe(
      true, [this](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool) {
        this->onNotify(d, l);
      });

  startHandshake();
  return true;
}

// ---------------- framing ----------------

void EcoflowSession::onNotify(const uint8_t* data, size_t len) {
  // Verified on hardware: the device sends one complete EncPacket per
  // notification (scan for 0x5A5A, length at offset 4-5). Reassembly still
  // handles fragmentation defensively.
  buf_.insert(buf_.end(), data, data + len);
  if (buf_.size() > 1024) buf_.clear();  // runaway guard

  while (true) {
    // Find the 0x5A5A prefix; drop any preamble noise before it.
    size_t start = 0;
    bool found = false;
    for (; start + 1 < buf_.size(); start++) {
      if (buf_[start] == 0x5A && buf_[start + 1] == 0x5A) { found = true; break; }
    }
    if (!found) {
      if (!buf_.empty() && buf_.back() != 0x5A) buf_.clear();
      return;
    }
    if (start > 0) buf_.erase(buf_.begin(), buf_.begin() + start);

    if (buf_.size() < 6) return;  // need the header + length
    uint16_t lenField = (uint16_t)(buf_[4] | (buf_[5] << 8));
    size_t total = (size_t)lenField + 6;  // header(6) + payload + crc(2)
    if (buf_.size() < total) return;      // wait for the rest

    // Copy the complete frame out (alloc here, in the callback task) and hand it
    // to poll() — do NOT decode/handle here, since handlers write to the link.
    std::vector<uint8_t> frame(buf_.begin(), buf_.begin() + total);
    buf_.erase(buf_.begin(), buf_.begin() + total);
    portENTER_CRITICAL(&notifyMux_);
    pendingFrames_.push_back(std::move(frame));  // O(1) move under lock
    portEXIT_CRITICAL(&notifyMux_);
  }
}

// Drain frames buffered by the notify callback and process them in the main-loop
// context (where blocking BLE writes are safe). Called from poll().
void EcoflowSession::processPending() {
  std::vector<std::vector<uint8_t>> local;
  portENTER_CRITICAL(&notifyMux_);
  local.swap(pendingFrames_);  // O(1) pointer swap; no alloc under lock
  portEXIT_CRITICAL(&notifyMux_);

  for (auto& raw : local) {
    ecoflow::EncFrame frame;
    if (ecoflow::decodeEncPacket(raw.data(), raw.size(), frame)) {
      handleEncFrame(frame);
    } else {
      Serial.println("[ecoflow] bad EncPacket (CRC/parse)");
    }
  }
}

void EcoflowSession::handleEncFrame(const ecoflow::EncFrame& frame) {
  // Handshake pubkey/key-info responses arrive as plaintext command frames;
  // everything post-cipher (auth result, telemetry) arrives as encrypted data.
  if (frame.type == ecoflow::FrameType::kCommand) {
    if (state_ == State::kAwaitPubKey) {
      onPubKey(frame.payload.data(), frame.payload.size());
    } else if (state_ == State::kAwaitKeyInfo) {
      onKeyInfo(frame.payload.data(), frame.payload.size());
    }
    return;
  }

  // Data frame: AES-CBC decrypt whole blocks, then decode the inner packet.
  // (The inner packet carries its own length + CRC, so trailing PKCS7 padding
  // bytes are simply not examined — no unpadding needed here.)
  if (!haveCipher_ || frame.payload.size() < 16) return;
  size_t whole = frame.payload.size() - (frame.payload.size() % 16);
  std::vector<uint8_t> plain(whole);
  ecoflow::aes128CbcDecrypt(aesKey_, iv_, frame.payload.data(), whole, plain.data());

  ecoflow::Packet pkt;
  if (ecoflow::decodePacketV2(plain.data(), plain.size(), pkt)) {
    handleInnerPacket(pkt);
  } else {
    Serial.println("[ecoflow] inner packet decode failed (post-decrypt)");
  }
}

void EcoflowSession::sendCommand(const uint8_t* payload, size_t len) {
  if (!writeChar_) return;
  auto frame = ecoflow::encodeEncPacket(ecoflow::FrameType::kCommand, payload, len);
  writeChar_->writeValue(frame.data(), frame.size(), true);
}

void EcoflowSession::sendData(const ecoflow::Packet& pkt) {
  if (!writeChar_ || !haveCipher_) return;
  auto inner = ecoflow::encodePacketV2(pkt);
  auto enc = ecoflow::aes128CbcEncryptPkcs7(aesKey_, iv_, inner.data(), inner.size());
  auto frame = ecoflow::encodeEncPacket(ecoflow::FrameType::kData, enc.data(), enc.size());
  writeChar_->writeValue(frame.data(), frame.size(), true);
}

void EcoflowSession::ackPacket(const ecoflow::Packet& in) {
  // LIVE-TUNE: the reference echoes received packets with src<->dst swapped,
  // otherwise the device only streams the bare minimum. V2 has no dsrc/ddst.
  ecoflow::Packet ack = in;
  ack.src = in.dst;
  ack.dst = in.src;
  ack.seq = seq_++;
  sendData(ack);
}

// ---------------- handshake ----------------

void EcoflowSession::startHandshake() {
  if (!ecoflow::ecdhMakeKey(privKey_, pubKey_)) {
    Serial.println("[ecoflow] ECDH keygen failed");
    return;
  }
  uint8_t payload[2 + 40];
  payload[0] = 0x01;
  payload[1] = 0x00;
  memcpy(payload + 2, pubKey_, 40);
  sendCommand(payload, sizeof(payload));
  state_ = State::kAwaitPubKey;
  stateSinceMs_ = millis();
  Serial.println("[ecoflow] handshake: sent public key");
}

void EcoflowSession::onPubKey(const uint8_t* p, size_t n) {
  // [0]=type [1]=status [2]=ecdh_type [3..]=device pubkey (40 for secp160r1).
  if (n < 3 + 40) { Serial.printf("[ecoflow] short pubkey resp (%u)\n", (unsigned)n); return; }
  uint8_t shared[20];
  if (!ecoflow::ecdhSharedSecret(privKey_, p + 3, shared)) {
    Serial.println("[ecoflow] ECDH shared-secret failed");
    return;
  }
  ecoflow::deriveIvFromShared(shared, iv_);
  memcpy(aesKey_, shared, 16);  // initial cipher = shared[:16]
  haveCipher_ = true;
  requestKeyInfo();
}

void EcoflowSession::requestKeyInfo() {
  const uint8_t payload[1] = {0x02};
  sendCommand(payload, sizeof(payload));
  state_ = State::kAwaitKeyInfo;
  stateSinceMs_ = millis();
  Serial.println("[ecoflow] handshake: requested key info");
}

void EcoflowSession::onKeyInfo(const uint8_t* p, size_t n) {
  // [0]=0x02, then AES-CBC body encrypted with the INITIAL cipher. Decrypt whole
  // blocks; sRand = plain[0:16], seed = plain[16:18].
  if (n < 1 + 32) { Serial.printf("[ecoflow] short key-info (%u)\n", (unsigned)n); return; }
  size_t bodyLen = n - 1;
  size_t whole = bodyLen - (bodyLen % 16);
  std::vector<uint8_t> plain(whole);
  ecoflow::aes128CbcDecrypt(aesKey_, iv_, p + 1, whole, plain.data());
  if (plain.size() < 18) { Serial.println("[ecoflow] key-info body too short"); return; }

  uint8_t sessionKey[16];
  ecoflow::genSessionKey(&plain[16], &plain[0], sessionKey);  // seed, srand
  memcpy(aesKey_, sessionKey, 16);  // swap to the session key (IV unchanged)
  Serial.println("[ecoflow] handshake: derived session key");

  // Probe auth status, then authenticate.
  ecoflow::Packet status;
  status.seq = seq_++;
  status.src = kAddrApp;
  status.dst = kAddrDev;
  status.cmdSet = kCmdSetAuth;
  status.cmdId = kCmdIdAuthStatus;
  sendData(status);

  authenticate();
}

void EcoflowSession::authenticate() {
  uint8_t auth[32];
  ecoflow::buildAuthPayload(userId_.c_str(), serial_.c_str(), auth);
  ecoflow::Packet pkt;
  pkt.seq = seq_++;
  pkt.src = kAddrApp;
  pkt.dst = kAddrDev;
  pkt.cmdSet = kCmdSetAuth;
  pkt.cmdId = kCmdIdAuth;
  pkt.payload.assign(auth, auth + 32);
  sendData(pkt);
  state_ = State::kAwaitAuth;
  stateSinceMs_ = millis();
  Serial.println("[ecoflow] handshake: sent auth");
}

void EcoflowSession::sendTimeSync() {
  // LIVE-TUNE: the device requests time after auth; without a reply it withholds
  // predictions/config. We have no RTC — send epoch 0 + UTC. Time-to-full does
  // not depend on our clock being correct.
  uint8_t payload[6];
  memset(payload, 0, sizeof(payload));  // unix seconds = 0
  payload[4] = 0;  // tz hours
  payload[5] = 0;  // tz minutes
  ecoflow::Packet pkt;
  pkt.seq = seq_++;
  pkt.src = kAddrApp;
  pkt.dst = kAddrDev;
  pkt.cmdSet = kCmdSetTime;
  pkt.cmdId = kCmdIdTime;
  pkt.payload.assign(payload, payload + sizeof(payload));
  sendData(pkt);
}

// ---------------- telemetry ----------------

void EcoflowSession::handleInnerPacket(const ecoflow::Packet& pkt) {
  // Auth result.
  if (pkt.src == kAddrDev && pkt.cmdSet == kCmdSetAuth && pkt.cmdId == kCmdIdAuth) {
    bool ok = !pkt.payload.empty() && pkt.payload[0] == 0x00;
    Serial.printf("[ecoflow] auth %s\n", ok ? "OK" : "FAILED");
    if (ok) {
      state_ = State::kAuthenticated;
      reading_.authenticated = true;
    } else if (connected()) {
      client_->disconnect();  // bad userId/serial — don't spin
    }
    return;
  }

  if (state_ != State::kAuthenticated) return;

  // Device asking for time.
  if (pkt.src == kAddrDev && pkt.cmdSet == kCmdSetTime && pkt.cmdId == kCmdIdTime) {
    sendTimeSync();
    return;
  }

  // Telemetry heartbeat structs.
  ecoflow::Telemetry t;
  if (ecoflow::applyTelemetry(pkt, t)) {
    if (t.haveSoc) { reading_.soc = t.soc; reading_.haveSoc = true; }
    if (t.haveInputWatts) { reading_.inputWatts = t.inputWatts; reading_.haveInputWatts = true; }
    if (t.haveOutputWatts) { reading_.outputWatts = t.outputWatts; reading_.haveOutputWatts = true; }
    if (t.haveChargeRemainMin) {
      reading_.chargeRemainMin = t.chargeRemainMin;
      reading_.haveChargeRemainMin = true;
    }
    if (t.haveDischargeRemainMin) {
      reading_.dischargeRemainMin = t.dischargeRemainMin;
      reading_.haveDischargeRemainMin = true;
    }

    // Derive charge state from in/out watts.
    if (reading_.haveInputWatts && reading_.inputWatts >= kWattsIdleThreshold) {
      reading_.state = EcoflowRichReading::Charge::kCharging;
    } else if (reading_.haveOutputWatts && reading_.outputWatts >= kWattsIdleThreshold) {
      reading_.state = EcoflowRichReading::Charge::kDischarging;
    } else if (reading_.haveInputWatts || reading_.haveOutputWatts) {
      reading_.state = EcoflowRichReading::Charge::kIdle;
    }
    reading_.lastUpdateMs = millis();

    // LIVE-TUNE: the reference acks received packets to keep the full stream
    // flowing. Enable once confirmed it doesn't cause a feedback loop.
    // ackPacket(pkt);
  }
}

void EcoflowSession::poll() {
  if (!connected()) return;
  processPending();  // handle any frames the notify callback buffered
  // Handshake watchdog: if we stall mid-handshake, drop so the owner retries.
  if (state_ != State::kAuthenticated && state_ != State::kDisconnected) {
    if (millis() - stateSinceMs_ > kHandshakeTimeoutMs) {
      Serial.println("[ecoflow] handshake timeout; disconnecting");
      client_->disconnect();
    }
  }
}

#endif  // ECOFLOW_ENABLE_GATT_TU
