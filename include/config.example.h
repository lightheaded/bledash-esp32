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

// --- Telemetry logging + upload (opt-in; see
//     plans/2026-07-24-01-telemetry-logging-upload.md) ---
// 0 = default: no flash logging, no WiFi. The logger, WiFi/TLS stack, and CA
//     bundle are compiled out entirely, exactly like ECOFLOW_GATT=0.
// 1 = log one sample per poll cycle to a LittleFS ring (works offline; this is
//     the backbone), and — once the uploader lands (T3) — drain the backlog to
//     TELEMETRY_URL over WiFi whenever a network is in range.
#define TELEMETRY_UPLOAD 0
// Tag attached to every sample so multiple devices land in distinct series
// (line protocol: bledash,device=<tag> ...). Used from T1 (logging) onward.
#define TELEMETRY_DEVICE_TAG "car"
// Hostname the device presents to DHCP (how it shows up in your router /
// hotspot client list).
#define WIFI_HOSTNAME "bledash-esp32"
// WiFi networks to seek, highest priority FIRST. Add as many X(ssid, password)
// lines as you like — the device scans and joins the highest-priority network
// in range (so a home AP is preferred over a metered phone hotspot when both
// are present). Real SSIDs/passwords are private; keep them only in gitignored
// config.h, never in a tracked/public file.
#define WIFI_AP_LIST \
  X("HomeWiFi", "home-password") \
  X("PhoneHotspot", "hotspot-password")
// Line-protocol ingest endpoint, e.g. "https://host/write". Bring your own sink
// (InfluxDB, VictoriaMetrics/vmagent, Telegraf — anything that accepts InfluxDB
// line protocol). Credentials are HTTP basic auth.
#define TELEMETRY_URL ""
#define TELEMETRY_USER ""
#define TELEMETRY_PASS ""
// TLS is verified against the embedded ISRG Root X1 (Let's Encrypt) by default.
// For a sink with a different issuer, define TELEMETRY_CA_PEM as the CA PEM
// string here; leave it undefined for Let's Encrypt endpoints.
// #define TELEMETRY_CA_PEM "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"
