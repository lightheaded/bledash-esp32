# Alpicool K25 — BLE protocol notes

GATT protocol sourced from the verified Home Assistant integration
[`Gruni22/alpicool_ha_ble`](https://github.com/Gruni22/alpicool_ha_ble) (v3.1.5),
which runs against this exact fridge. Advertisement details observed on real
hardware at M2 (2026-07-08). Actual device MAC is in the gitignored
`local/devices.md`.

## Advertisement (observed at M2)

- **Local name:** `A1-<MAC without colons>` (e.g. `A1-FFFF1193B6DA`).
- **Advertised service UUID:** `0xFFF0` on this unit. (The integration also matches
  `0x1234`; which one appears varies by model/firmware — this K25 uses `0xFFF0`.)
- **Manufacturer data:** company ID `0xFFFF`, 8 bytes all `0xFF` — **no telemetry in
  the advertisement.** Readings require connecting and querying (see below).
- **Single BLE central:** the fridge accepts one connection and stops advertising
  while connected. See the main plan's contention section; disable the HA
  integration (`scripts/ha-alpicool.py disable`) before connecting from the ESP32.

## GATT

- **Service:** `0x1234` / `0xFFF0`.
- **Write characteristic:** `00001235-0000-1000-8000-00805f9b34fb`.
- **Notify characteristic:** `00001236-0000-1000-8000-00805f9b34fb`.

### Packet framing

```
FE FE <len> <cmd> <payload...> <checksum:2 bytes big-endian>
```

- `len` = number of bytes after the `FE FE` header (i.e. `<cmd> + payload + checksum`).
- `checksum` = 16-bit sum of all preceding bytes (`FE FE … payload`), big-endian.
- Notifications may fragment across BLE packets — reassemble on the `FE FE` header +
  `len` byte before parsing.

### Commands (literal packets)

- **BIND:** `FE FE 03 00 01 FF` — send once after connecting.
- **QUERY:** `FE FE 03 01 02 00` — request a status frame.

### QUERY response payload (single-zone K25)

Byte offsets within the payload (after `<cmd>`):

| Offset | Meaning |
|---|---|
| 0 | locked |
| 1 | **powered on** (0/1) |
| 2 | run mode (0 = Eco, 1 = Max) |
| 3 | battery-protection level (0/1/2 = low/med/high) |
| 4 | **target temp** (signed °C) |
| 5 / 6 | temp max / min |
| 9 | unit (0 = °C) |
| 14 | **actual temp** (signed °C) |
| 15 | input battery % |
| 16 / 17 | input voltage integer / decimal |

Signed bytes are two's-complement (`value - 256` if `> 127`).

MVP needs offsets **1** (on/off), **4** (target), **14** (actual), and optionally
**3** (battery protection).

## Licensing

`Gruni22/alpicool_ha_ble` — check its license before copying code verbatim. The
byte layout above is protocol fact (not copyrightable); re-implement the framing
and parsing from these notes and credit the repo.
