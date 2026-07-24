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

  // Send a QUERY now (if connected), independent of the poll cadence. Used to
  // poll for confirmation right after a SET.
  void requestStatusNow();

  // Set the compressor on/off. Rebuilds the fridge's full 14-byte settings
  // struct from the last QUERY snapshot (preserving every other setting) with
  // only the power byte changed, and sends it as a SET (cmd 0x02). Returns false
  // if not ready (disconnected, or no status snapshot cached yet). Forces a
  // re-QUERY on the next poll() so reading() reflects the applied state.
  bool setPower(bool on);

  bool connected() const { return client_ && client_->isConnected(); }
  // True once we can safely issue a SET: connected AND we hold a QUERY snapshot
  // to reconstruct the settings struct from.
  bool canControl() const { return connected() && haveStatusPrefix_; }
  // True between a setPower() SET and the next status frame that resolves it —
  // i.e. while we've asked for a change but reading() may still show the old
  // state. Lets the UI show a "confirming" indicator instead of a stale value.
  // Auto-expires after kPowerPendingMaxMs so a lost reply on a weak link can't
  // wedge the indicator on indefinitely.
  bool powerChangePending() const {
    return powerPending_ && (millis() - powerSetMs_ < kPowerPendingMaxMs);
  }
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

  // First 14 bytes of the last QUERY response payload = the fridge's full
  // settings struct (locked, powerOn, runMode, batProtect, target, temp max/min,
  // return-diff, start-delay, unit, 4×tc). SET (0x02) resends this struct, so a
  // safe power toggle copies it verbatim and flips only the power byte. Cleared
  // on disconnect so we never build a SET from stale settings.
  uint8_t statusPrefix_[14] = {0};
  bool haveStatusPrefix_ = false;

  // Set when a SET is sent, cleared by the next status frame (the confirming
  // re-QUERY) or on disconnect. Drives the UI "confirming" indicator.
  static constexpr uint32_t kPowerPendingMaxMs = 5000;
  bool powerPending_ = false;
  uint32_t powerSetMs_ = 0;
};
