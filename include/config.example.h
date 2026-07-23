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

// --- EcoFlow telemetry depth ---
// 0 = battery %% only, read passively from BLE advertisements. No EcoFlow account
//     needed, no connection, never contends with the phone app. This is the default.
// 1 = advanced: an authenticated, encrypted GATT session that also yields charge/
//     discharge watts and time-to-full. Requires ECOFLOW_USER_ID below, holds a BLE
//     connection to the unit (so the EcoFlow phone app can't connect at the same
//     time), and is more moving parts. Opt in only if you want the richer data.
#define ECOFLOW_GATT 0
// EcoFlow account userId that this River 2 Max is bonded to (a decimal string,
// e.g. "1234567890123456789"). ONLY needed when ECOFLOW_GATT is 1: the handshake
// sends MD5(ECOFLOW_USER_ID + ECOFLOW_SERIAL). Fetch it once from the EcoFlow cloud
// login endpoint (see scripts/ecoflow_userid.py) — never commit it.
#define ECOFLOW_USER_ID ""
