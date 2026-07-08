// Example configuration. Copy to config.h and fill in real values.
//   cp include/config.example.h include/config.h
// config.h is gitignored — it holds private device identifiers (MACs, serials).
#pragma once

// --- OLED I2C pins ---
// This board family is commonly wired SDA=GPIO5, SCL=GPIO6 for the onboard OLED.
// Some vendor pinouts label I2C as GPIO8/GPIO9 (likely the header breakout).
// M1 confirms which pair drives the onboard display; adjust here if needed.
#define OLED_SDA 5
#define OLED_SCL 6

// --- Devices (used from M2 onward; not needed for M1) ---
// Alpicool fridge BLE MAC, e.g. "AA:BB:CC:DD:EE:FF"
#define ALPICOOL_MAC ""
// EcoFlow River 2 Max serial to match in BLE advertisements, e.g. "R611XXXXXXXXXXXX".
// Match on THIS serial, not device type — a second EcoFlow may be in range.
#define ECOFLOW_SERIAL ""
