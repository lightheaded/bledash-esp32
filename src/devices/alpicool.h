// Alpicool K25 BLE driver.
// Holds a connection to the fridge, sends BIND + QUERY over char 0x1235, parses
// the FE FE-framed status notifications from char 0x1236.
// The owner drives BLE scanning and hands us the fridge's advertisement when it
// appears (see connectTo); we don't scan ourselves, so the owner can also scan
// for other devices (e.g. EcoFlow) without contention.
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
  // mac: "aa:bb:cc:dd:ee:ff". queryIntervalMs: status poll cadence.
  void begin(const char* mac, uint32_t queryIntervalMs);

  // True if this advertised device is our fridge.
  bool matches(const NimBLEAdvertisedDevice* dev) const;

  // Connect + discover + subscribe + BIND. Call with our advertisement (from the
  // owner's scan) when disconnected. Returns true once connected & bound.
  bool connectTo(const NimBLEAdvertisedDevice* dev);

  // Call frequently. Sends a QUERY when connected and the interval has elapsed.
  void poll();

  bool connected() const { return client_ && client_->isConnected(); }
  const FridgeReading& reading() const { return reading_; }

 private:
  void onDisconnect(NimBLEClient* c, int reason) override;

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
};
