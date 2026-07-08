# EcoFlow River 2 series — BLE protocol notes

Observed on real hardware at M2 (2026-07-08) with the M2 scanner firmware, and
cross-checked against Home Assistant's cloud sensors. Actual device serials/MACs
are in the gitignored `local/devices.md`.

## Two tiers

The MVP uses **Tier 1 only** (passive advertisement → battery %). Tier 2
(authenticated GATT for watts) is deferred to a v2 plan.

## Tier 1 — advertisement (no connection, no auth)

River 2-series units broadcast a BLE advertisement with:

- **Local name:** `EF-R1xxxx` (River 2 Max) or `EF-R0xxxx` (River 2), where `xxxx`
  is the last 4 digits of the serial.
- **Manufacturer data**, company ID **`0xB5B5`** (46517), 26 bytes:

  | Offset | Bytes | Meaning |
  |---|---|---|
  | 0–1 | `B5 B5` | company ID (little-endian) |
  | 2 | `13` | constant (0x13 = 19; likely a type/length marker) |
  | 3–18 | 16× ASCII | **device serial** (e.g. `R611…` / `R601…`) |
  | 19 | 1 | **battery % (state of charge)** |
  | 20 | `00` | unknown |
  | 21 | `01` | unknown (constant so far) |
  | 22–23 | `00 00` | unknown |
  | 24 | `3E` | constant so far |
  | 25 | 1 | varies between units — likely a checksum |

### Battery-% offset — verified

At M2, two units were in range simultaneously:

- River 2 Max: advert byte[19] = `0x64` = 100 → HA `car_ecoflow_battery_level` = 100%.
- River 2 (techroom): advert byte[19] = `0x62` = 98 → HA `techroom_..._battery_level` = 98%.

Both match, so **offset 19 is the SoC in whole percent**.

### Advertising is not continuous when idle

Observed at M5: the River 2 Max stopped advertising over BLE (and stopped
reporting to the EcoFlow cloud) after roughly an hour idle — both transports
went quiet together, so it's a device power/idle state, not a BLE-specific
quirk. When it's actually working (e.g. powering the fridge in the car) it
advertises fine. The firmware treats a battery reading with no fresh
advertisement as stale (`--` on the display) after `kBatteryStaleMs`.

### Implementation notes for the firmware

- Match on the **full serial** at offset 3–18, not on company ID or name prefix:
  there are two River 2-series units in the house and both advertise `0xB5B5`.
  The target serial lives in `config.h` as `ECOFLOW_SERIAL`.
- Passive scanning is enough; no connection is made, so there is no contention
  with the EcoFlow phone app or anything else.
- Watts / input / output / remaining time are **not** in the advertisement (all the
  trailing bytes are constant or a checksum). They require the Tier 2 GATT session.

## Tier 2 — authenticated GATT (deferred, not implemented)

Full telemetry (watts, remaining time) needs an authenticated, encrypted GATT
session: an EcoFlow account userId, an ECDH handshake on the legacy SECP160r1
curve, a proprietary key table extracted from the vendor app, MD5 session-key
derivation, and protobuf decoding. Reverse-engineered in Python by
[`rabits/ha-ef-ble`](https://github.com/rabits/ha-ef-ble) (River 2 Max supported)
and [`rabits/ef-ble-reverse`](https://github.com/rabits/ef-ble-reverse). No
C/C++/ESP32 implementation exists; porting it would be a first. EcoFlow devices
also allow only one BLE central at a time, so a Tier 2 client contends with the
phone app.
