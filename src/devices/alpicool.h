// Alpicool K25 BLE driver.
// Connects to the fridge, sends BIND + QUERY over char 0x1235, parses the
// FE FE-framed status notifications from char 0x1236.
// Protocol: see docs/protocols/alpicool.md.
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <vector>

struct FridgeReading {
  bool valid = false;         // have we ever parsed a status frame?
  bool poweredOn = false;     // compressor on/off
  int8_t targetTemp = 0;      // setpoint, °C
  int8_t actualTemp = 0;      // measured, °C
  uint8_t batProtect = 0;     // battery-protection level 0/1/2
  uint32_t lastUpdateMs = 0;  // millis() of last successful parse
};

class AlpicoolDriver : public NimBLEClientCallbacks {
 public:
  // mac: lowercase or uppercase "aa:bb:cc:dd:ee:ff". queryIntervalMs: poll cadence.
  void begin(const char* mac, uint32_t queryIntervalMs);

  // Call frequently from loop(). Manages (re)connect and periodic QUERY.
  void loop();

  bool connected() const { return client_ && client_->isConnected(); }
  const FridgeReading& reading() const { return reading_; }

  // True if the fridge was seen advertising during the last connect attempt
  // (distinguishes "out of range / held elsewhere" from "connected").
  bool wasAdvertising() const { return wasAdvertising_; }

 private:
  void onConnect(NimBLEClient* c) override;
  void onDisconnect(NimBLEClient* c, int reason) override;

  bool ensureConnected();
  bool discoverChars();
  void onNotify(const uint8_t* data, size_t len);
  void handleFrame(const uint8_t* frame, size_t total);
  void parseStatus(const uint8_t* payload, size_t len);
  void sendPacket(const uint8_t* data, size_t len);

  String targetMac_;
  uint32_t queryIntervalMs_ = 60000;

  NimBLEClient* client_ = nullptr;
  NimBLERemoteCharacteristic* writeChar_ = nullptr;
  NimBLERemoteCharacteristic* notifyChar_ = nullptr;

  std::vector<uint8_t> buf_;  // notification reassembly buffer
  FridgeReading reading_;
  uint32_t lastQueryMs_ = 0;
  uint32_t lastConnAttemptMs_ = 0;
  bool bound_ = false;
  bool wasAdvertising_ = false;
};
