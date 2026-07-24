// One telemetry sample — the fridge + EcoFlow fields we log once per poll cycle.
// Pure data, no Arduino/NimBLE, so the encoder and ring logger that consume it
// stay host-testable (`pio test -e native`). Every field is optional: a given
// minute emits only what was valid then (compressor off still yields fridge
// temps; EcoFlow watts exist only during an authenticated GATT session). The
// line-protocol encoder skips fields whose have-flag is false.
// See plans/2026-07-24-01-telemetry-logging-upload.md.
#pragma once

#include <stdint.h>

namespace telemetry {

struct Sample {
  bool haveFridgeTemp = false;
  int16_t fridgeTempC = 0;      // measured cabinet temp, °C
  bool haveFridgeSetpoint = false;
  int16_t fridgeSetpointC = 0;  // configured setpoint, °C
  bool haveFridgeOn = false;
  bool fridgeOn = false;        // compressor on/off

  bool haveSoc = false;
  float socPct = 0.0f;          // battery state of charge, %

  bool haveInWatts = false;
  uint16_t inWatts = 0;         // charge power in (GATT session only)
  bool haveOutWatts = false;
  uint16_t outWatts = 0;        // load power out (GATT session only)
  bool haveRemainMin = false;
  uint32_t remainMin = 0;       // minutes to full/empty (GATT session only)

  // True if any field is present. An empty sample (e.g. nothing in range yet) is
  // not worth a flash write or an upload line.
  bool any() const {
    return haveFridgeTemp || haveFridgeSetpoint || haveFridgeOn || haveSoc ||
           haveInWatts || haveOutWatts || haveRemainMin;
  }
};

}  // namespace telemetry
