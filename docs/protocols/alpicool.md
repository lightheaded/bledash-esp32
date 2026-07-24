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
| 7 | return differential |
| 8 | start delay |
| 9 | unit (0 = °C) |
| 10–13 | temp-comp params (tc hot/mid/cold/halt) |
| 14 | **actual temp** (signed °C) |
| 15 | input battery % |
| 16 / 17 | input voltage integer / decimal |

Signed bytes are two's-complement (`value - 256` if `> 127`).

MVP needs offsets **1** (on/off), **4** (target), **14** (actual), and optionally
**3** (battery protection).

### SET command (0x02) — writing settings

There is **no dedicated power/target command for the settings struct**: the fridge
takes a single "set all settings" packet (`cmd = 0x02`) carrying the full 14-byte
settings block, and you change one field by resending the block with only that
byte flipped. **The SET payload is exactly bytes 0–13 of the QUERY response**
(offsets above, up to and including the temp-comp params) — the read-only
telemetry at offsets 14+ (actual temp, battery %, voltage) is dropped.

```
FE FE 11 02 <14 settings bytes> <checksum:2>
      │  │  └ payload = QUERY response bytes 0..13, verbatim
      │  └ cmd = SET (0x02)
      └ len = 0x11 (17 = 1 cmd + 14 payload + 2 checksum)
```

Checksum is the same 16-bit big-endian sum as every other frame. Worked example —
**power ON** with locked=0, run=1, batProt=1, target=−18 °C (`0xEE`), max=20
(`0x14`), min=−20 (`0xEC`), ret_diff=2, rest 0:

```
FE FE 11 02  00 01 01 01 EE 14 EC 02 00 00 00 00 00 00  04 02
```

Flip payload offset **1** to `00` for OFF and recompute the checksum.

**To toggle power safely, keep the last QUERY response's bytes 0–13, change only
offset 1, rebuild the frame** — this preserves target temp, run mode, battery
protection, and the temp-comp params instead of overwriting them with guesses.
The firmware caches this prefix (`AlpicoolDriver::statusPrefix_`) so a control
action never needs to reconstruct fields it can't see.

Ordering: **BIND → QUERY (need a snapshot) → SET**. The device echoes SET back as
a notification with no functional ACK, so confirm by re-issuing QUERY (the driver
forces one on the next `poll()`).

## Licensing

`Gruni22/alpicool_ha_ble` ships **no `LICENSE` file** — treat it as
all-rights-reserved and do not copy its code verbatim. The byte layouts above
(framing, offsets, the SET struct) are protocol fact, not copyrightable; they
were re-implemented from the notes here and the driver code is original. Credit
the repo as the protocol source.
