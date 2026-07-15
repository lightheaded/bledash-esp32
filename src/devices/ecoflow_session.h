// EcoFlow River 2 Max — authenticated GATT session (Tier 2, opt-in).
// Holds a BLE connection, runs the secp160r1 ECDH + key-table handshake to an
// authenticated, AES-CBC-encrypted channel, then decodes the fixed-offset
// heartbeat structs into rich telemetry (watts, charge state, time-to-full).
//
// Only compiled/used when ECOFLOW_GATT is enabled. The owner (main.cpp) drives
// BLE scanning and hands us the EcoFlow's advertisement (matched by serial); we
// don't scan ourselves, mirroring AlpicoolDriver.
//
// STATUS: written against the rabits/ha-ef-ble + ef-ble-reverse reference but
// NOT yet verified against real hardware. The pure codec/crypto it calls
// (ecoflow_proto, ecoflow_crypto) IS host-tested; the live BLE framing details
// (notification reassembly, packet-acking, time-sync) are marked LIVE-TUNE and
// need confirming on the device. Protocol: plans/2026-07-15-01-...md.
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <vector>

#include "devices/ecoflow_proto.h"

struct EcoflowRichReading {
  bool authenticated = false;   // handshake completed at least once
  bool haveSoc = false;
  float soc = 0.0f;             // % state of charge (high-res)
  bool haveInputWatts = false;
  uint16_t inputWatts = 0;      // charge power in
  bool haveOutputWatts = false;
  uint16_t outputWatts = 0;     // load power out
  bool haveChargeRemainMin = false;
  uint32_t chargeRemainMin = 0;  // minutes to full (when charging)
  bool haveDischargeRemainMin = false;
  uint32_t dischargeRemainMin = 0;  // minutes to empty (when discharging)
  uint32_t lastUpdateMs = 0;     // millis() of last telemetry frame

  enum class Charge { kUnknown, kCharging, kDischarging, kIdle };
  Charge state = Charge::kUnknown;
};

class EcoflowSession : public NimBLEClientCallbacks {
 public:
  // serial: the River 2 Max serial to match/authenticate. userId: the bonded
  // EcoFlow account id (both from config.h).
  void begin(const char* serial, const char* userId);

  // True if this advertised device is our EcoFlow (matched by serial in the
  // 0xB5B5 manufacturer data — same match as the passive monitor).
  bool matches(const NimBLEAdvertisedDevice* dev) const;

  // Connect + discover + subscribe + start the handshake. Call with our
  // advertisement (from the owner's scan) when disconnected.
  bool connectTo(const NimBLEAdvertisedDevice* dev);

  // Call frequently: drives handshake timeouts and periodic time re-sync.
  void poll();

  bool connected() const { return client_ && client_->isConnected(); }
  bool authenticated() const { return state_ == State::kAuthenticated; }
  const EcoflowRichReading& reading() const { return reading_; }

 private:
  enum class State {
    kDisconnected,
    kAwaitPubKey,
    kAwaitKeyInfo,
    kAwaitAuth,
    kAuthenticated,
  };

  void onDisconnect(NimBLEClient* c, int reason) override;
  bool discoverChars();

  // --- framing helpers ---
  void onNotify(const uint8_t* data, size_t len);      // reassembles EncPackets
  void handleEncFrame(const ecoflow::EncFrame& frame);  // one outer frame
  void handleInnerPacket(const ecoflow::Packet& pkt);   // one decrypted inner packet
  void sendCommand(const uint8_t* payload, size_t len);       // plaintext EncPacket
  void sendData(const ecoflow::Packet& pkt);                  // encrypted EncPacket
  void ackPacket(const ecoflow::Packet& pkt);                 // echo w/ src<->dst swap

  // --- handshake steps ---
  void startHandshake();          // step 1: send our ECDH public key
  void onPubKey(const uint8_t* p, size_t n);   // step 2
  void requestKeyInfo();          // step 3
  void onKeyInfo(const uint8_t* p, size_t n);  // step 4 -> session key
  void authenticate();            // step 6
  void sendTimeSync();            // answer the device's RTC request

  String serial_;
  String userId_;

  NimBLEClient* client_ = nullptr;
  NimBLERemoteCharacteristic* writeChar_ = nullptr;
  NimBLERemoteCharacteristic* notifyChar_ = nullptr;

  State state_ = State::kDisconnected;
  uint32_t stateSinceMs_ = 0;  // for handshake timeout

  // ECDH / cipher state.
  uint8_t privKey_[21];
  uint8_t pubKey_[40];
  uint8_t iv_[16];       // session IV = MD5(shared secret); constant per session
  uint8_t aesKey_[16];   // current AES key (initial = shared[:16], then session key)
  bool haveCipher_ = false;

  uint32_t seq_ = 0;     // outgoing inner-packet sequence
  std::vector<uint8_t> buf_;  // notification reassembly buffer
  EcoflowRichReading reading_;
};
