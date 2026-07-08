// EcoFlow River 2-series battery monitor (Tier 1: passive advertisement).
// Parses the 0xB5B5 manufacturer data to read battery % without connecting.
// Matches on the full serial so it never latches onto another EcoFlow in range.
// Protocol: see docs/protocols/ecoflow.md.
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

struct BatteryReading {
  bool valid = false;
  uint8_t percent = 0;
  uint32_t lastUpdateMs = 0;  // millis() of last matching advertisement
};

class EcoflowMonitor {
 public:
  // serial: the exact 16-char device serial to match (e.g. "R611...").
  void begin(const char* serial);

  // Feed every scanned advertisement; updates the reading if it's our device.
  void onAdvertisement(const NimBLEAdvertisedDevice* dev);

  const BatteryReading& reading() const { return reading_; }

 private:
  String serial_;
  BatteryReading reading_;
};
