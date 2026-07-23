# EcoFlow River 2 Max — authenticated GATT telemetry (watts, charge state, time-to-full)

- **Date:** 2026-07-15
- **Status:** 🚧 in progress — **E1–E4 done; E4 VERIFIED on real hardware.** The
  authenticated GATT session connects, handshakes, and streams decoded telemetry: observed
  `auth OK` then `dsg in=0W out=97W soc=29.8 toEmpty=80min` (discharging into the fridge,
  SoC ticking down as expected). As far as our research found, the first working
  C/C++/ESP32 EcoFlow authenticated BLE session. 25 host tests pass; opt-in strips cleanly.
  Next: E5 display layout, then E6 coexistence/robustness, then the charging-direction
  check when solar is connected.
- **Depends on:** shipped MVP (v0.1.0). Extends the existing passive-advert battery
  reading with a full authenticated GATT session.
- **Motivation:** off-grid use (solar charging at a campsite / beach). The user wants to
  see, at a glance from a distance: **is the EcoFlow charging, how fast (watts), and how
  long until full.** None of that is in the BLE advertisement — only SoC % is. It requires
  the authenticated, encrypted GATT session that the MVP plan deferred.

## Why this is now feasible (research 2026-07-15)

The MVP plan called this "a large reverse-engineering effort with no ESP32 precedent."
Two findings shrink it to a well-scoped project:

1. **River 2 Max telemetry is NOT protobuf.** It streams fixed-width little-endian
   binary "heartbeat pack" structs decoded by byte offset. (The larger EcoFlow units use
   protobuf; River 2 does not.) So **no protobuf/nanopb is needed for the telemetry we
   want** — only for one optional time-sync reply, which can be hand-built as a fixed
   byte blob.
2. **The SECP160r1 curve — the real blocker — is solved by `micro-ecc`.** It supports
   secp160r1 ECDH natively (drop-in, portable C, ESP32-compatible). No mbedTLS custom-curve
   surgery. mbedTLS (already bundled) covers MD5 and hardware-accelerated AES-CBC.

Reference implementations (both open source, MIT-ish): `rabits/ha-ef-ble` (the actively
maintained Home Assistant integration; River 2 Max supported) and `rabits/ef-ble-reverse`
(the original single-file PoC). Every constant below is cross-confirmed between the two.

## Shipping shape — advanced telemetry is OPT-IN

The GATT path is more moving parts than the MVP (needs an EcoFlow account `userId`,
holds a BLE connection that locks out the phone app). So it ships as an **opt-in
compile-time flag**, keeping the simple Tier-1 experience as the default:

- `#define ECOFLOW_GATT 0` (default, in `config.example.h`) — battery %% only, read
  passively from advertisements. No account, no connection, no contention. Exactly the
  v0.1.0 behaviour.
- `#define ECOFLOW_GATT 1` — authenticated GATT session; also yields watts, charge
  state, and time-to-full. Requires `ECOFLOW_USER_ID`. A `#if ECOFLOW_GATT` /
  `#error "ECOFLOW_GATT requires ECOFLOW_USER_ID"` guard fails the build early if the id
  is missing.

When the flag is 0, `main.cpp` never references `EcoflowSession`, so the linker strips
the session code, micro-ecc, and the 64 KB key table — the default build carries no size
or contention cost. This is what makes the feature safe to land for everyone.

**Release intent:** land as a new minor version. The PR should document, for the
community, the protocol, the `userId` fetch, the NimBLE symbol-collision gotcha and its
fix, and the opt-in flag — so others can reproduce it. This is (per our notes) the first
C/C++/ESP32 port of the EcoFlow authenticated BLE session.

## Provisioning dependency (BLOCKS end-to-end auth)

The auth step sends `MD5(userId + deviceSerial)`. We already have the **serial** (in
`config.h`). We do **not** yet have the EcoFlow account **`userId`** — a static per-account
decimal string. It must be obtained once and added to `config.h` (gitignored, like the
serial — **never** committed):

- Easiest: `ha-ef-ble`'s cloud login flow returns `data.user.userId` (needs the EcoFlow
  account email/password, one-time, over the internet), or
- From the phone app storage: Android `/data/data/com.ecoflow/files/mmkv/mmkv.default`
  (`user_id`), or the iOS app data (`data.user.userId`).

Until the `userId` is provisioned, E1–E3 (codec + crypto, host-testable) can proceed, but
E4+ (talking to the real device) cannot complete. Note the device also stores the userId of
the account it was bonded to and validates locally — so it must be the account that owns
this specific River 2 Max.

## The proprietary key table

The session-key derivation indexes a fixed **65,280-byte (0xFF00) key table** extracted
from the vendor app (identical in both reference repos: `ha-ef-ble/eflib/keydata.py`
base64 blob == `ef-ble-reverse` `login_key.bin`). It is a shared vendor constant, not a
user secret. Embed it in firmware as a `PROGMEM`/`const` byte array in its own source file
(e.g. `src/devices/ecoflow_keytable.cpp`, generated from the base64). ~64 KB of the 4 MB
flash — negligible.

## BLE contention & coexistence (changes vs MVP)

The MVP never connected to the EcoFlow, so it had zero contention. Holding a GATT session
changes that:

- **Single central rule:** while bledash holds the EcoFlow GATT session, the **EcoFlow
  phone app cannot connect**, and vice-versa. When the user opens the app, our session will
  drop; we must reconnect-with-backoff when it frees up. Acceptable, but document it.
- **HA is unaffected:** HA reads the EcoFlow over WiFi→cloud, not BLE. HA's cloud sensors
  remain the **validation oracle** (compare our watts / SoC / time-to-full against
  `sensor.car_ecoflow_*`).
- **Two concurrent GATT connections on one core:** bledash will now hold BOTH the Alpicool
  (existing) and the EcoFlow. NimBLE on the C3 supports multiple connections
  (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS`, default 3) — confirm/raise in `platformio.ini`. The
  scan loop still feeds advert discovery (to learn the EcoFlow MAC by serial). Keep the main
  loop a simple serialized state machine.
- **EcoFlow idle drop-off:** per the MVP protocol notes, the River 2 Max stops advertising
  (and cloud-reporting) after ~1 h idle. When it's actually charging/working it's up. Treat
  a dropped session as normal; reconnect when it reappears in the scan.

## Protocol reference (self-contained; sources above)

**GATT (River 2 "rfcomm" variant):**
- Write char: `00000002-0000-1000-8000-00805f9b34fb`
- Notify char: `00000003-0000-1000-8000-00805f9b34fb`
- Writes during session use write-with-response.

**Outer frame `EncPacket`** (what goes over BLE), little-endian, prefix `0x5A5A`:
`5A 5A | type<<4 (0x00 cmd / 0x10 data) | 0x01 | len=payload+2 (u16) | payload | CRC16`.
Payload is plaintext for handshake command frames, AES-128-CBC for data frames.

**Inner packet `Packet` V2** (River 2 Max = version 2), prefix `0xAA`:
`AA | 02 | payload_len(u16) | CRC8(hdr) | 0x0D | seq(u32) | 00 00 | src | dst | cmd_set |
cmd_id | payload | CRC16`. (V3 inserts dsrc,ddst at 14; River 2 Max is V2 — none.)

**CRCs:** CRC8 = poly `0x07` init `0x00` (CRC-8/CCITT), on inner header byte @4.
CRC16 = poly `0x8005` reflected, init `0x0000` (CRC-16/ARC), on outer + inner trailers.

**Handshake (encrypt_type 7):**
1. Connect; subscribe notify.
2. Send ephemeral **secp160r1** public key (raw 40-byte X‖Y): payload `01 00` + pubkey.
3. Receive device pubkey (40 bytes). ECDH → 20-byte shared X. `IV = MD5(shared)` (16 B).
   Initial cipher = AES-128-CBC(key = shared[:16], IV).
4. Send `02` → device replies (encrypted) with 16-byte `sRand` + 2-byte `seed`.
5. `session_key = MD5( keytable[pos:pos+16] ‖ sRand[0:16] )`, where
   `pos = seed[0]*0x10 + ((seed[1]-1)&0xFF)*0x100`. Swap cipher to AES-128-CBC(session_key,
   same IV).
6. Auth: send inner `Packet(src=0x21,dst=0x35,cmd_set=0x35,cmd_id=0x86)` with payload =
   32 ASCII bytes = **UPPERCASE hex of `MD5(userId ‖ serial)`**. Success reply
   `src=0x35,set=0x35,id=0x86,payload[0]=0x00`.

**Post-auth behaviors required to get the full stream:**
- **Echo/ack received packets** (copy, swap src↔dst, set dsrc=ddst=0x01, write back) —
  without this the device sends only minimal data.
- **Service the time request:** device sends `src=0x35,set=0x01,id=0x52`; reply with
  `u32 LE unix_seconds | i8 tz_h | i8 tz_min` to `dst=0x35,set=0x01,id=0x52`. (The extra
  protobuf `SysUTCSync` is optional; can hand-build a minimal blob if needed.) No hardware
  RTC — we have no wall-clock; send a plausible epoch (or 0 + UTC offset). Confirm the device
  accepts it; time-to-full doesn't depend on our clock being correct.

**Telemetry structs we care about** (little-endian, decoded by offset; frames may be short
so guard on length):

| Value | Pack (src/set/id) | Field | Type @ offset |
|---|---|---|---|
| Input watts (charge) | PD `0x02/0x20/0x02` | `watts_in_sum` | u16 @ 17 |
| Output watts | PD `0x02/0x20/0x02` | `watts_out_sum` | u16 @ 15 |
| SoC % (hi-res) | BMS `0x03/0x20/0x32` | `f32_show_soc` | f32 @ 53 |
| **Time-to-full (min)** | EMS `0x03/0x20/0x02` | `chg_remain_time` | u32 @ 17 |
| Time-to-empty (min) | EMS `0x03/0x20/0x02` | `dsg_remain_time` | u32 @ 21 |

Charging vs discharging is directly known from `watts_in_sum` vs `watts_out_sum` (no need
to infer from SoC slope). Solar/DC input specifically is in the MPPT pack
(`0x05/0x20/0x02`, `in_watts`) if we want to distinguish solar from AC later.

## Crypto/lib stack decision

| Concern | Choice |
|---|---|
| secp160r1 ECDH | **micro-ecc** (`kmackay/micro-ecc`), compile out the other 4 curves |
| MD5, AES-128-CBC | **vendored portable C** (in `ecoflow_crypto.cpp`) |
| Protobuf | **not needed** for telemetry; hand-build the one time-sync blob if required |

Deviation from the original stack idea: MD5/AES are **vendored portable
implementations**, not mbedTLS. The data volumes (handshake + tiny heartbeats) make
hardware AES irrelevant, and vendoring lets the exact same code run under the host
unit tests (`pio test -e native`) and on-device — no mbedTLS shim on the host. Both
are validated against independent golden vectors (Python `hashlib` for MD5,
`pyca/cryptography` for AES-CBC).

**micro-ecc gotcha:** secp160r1's order `n` > 2^160, so the **private key is 21 bytes**,
not 20. Always size buffers via `uECC_curve_private_key_size()`. Public key is 40 bytes
(raw X‖Y, no `04` prefix) — matches the wire format directly. ~80-bit security + MD5 is
weak by modern standards; acceptable only because we're interoperating with a fixed legacy
system, not designing new security.

## Milestones

- **E1 — Scaffolding & provisioning.** Add `ECOFLOW_USER_ID` to `config.example.h` +
  `config.h`. Add micro-ecc to `platformio.ini` (`lib_deps`), compile out unused curves.
  Generate `ecoflow_keytable.cpp` from the base64 blob. Add a **native test env** to
  `platformio.ini` so the pure-logic layer is host-testable. *No device needed.*
- **E2 — Pure codec layer (host tests).** CRC8/CRC16, `EncPacket` encode/decode, `Packet`
  V2 encode/decode, `Type7` AES-CBC wrap, `genSessionKey`, telemetry struct parsers.
  Unit-test against vectors captured from the Python reference. *No device.*
- **E3 — Crypto handshake (host tests where possible).** secp160r1 keygen + ECDH via
  micro-ecc; `IV = MD5(shared)`; session-key derivation; auth-hash builder. Verify the
  shared secret + derived key against a Python-generated vector (feed a fixed peer key).
  *No device for the math; the live exchange is E4.*
- **E4 — On-device GATT client + handshake.** New `EcoflowSession` (NimBLE client): find
  the EcoFlow MAC from the existing serial-matched advert, connect, subscribe, run the E2/E3
  state machine to authenticated, echo/ack packets, service time-sync. Serial-log the raw
  first heartbeats. *Needs device + provisioned `userId`; release the phone app first.*
  **✅ First task done — micro-ecc ↔ NimBLE symbol collision resolved** (discovered at E1):
  NimBLE bundles tinycrypt, which exports the *same* `uECC_*` symbol names (old micro-ecc
  0.x API, P-256 only), so linking `kmackay/micro-ecc` into the device build failed with
  "multiple definition of `uECC_make_key`/`uECC_shared_secret`/`uECC_valid_point`/…".
  Fixed by vendoring micro-ecc into `lib/ef_microecc/` with a `ef_uecc_prefix.h` header
  (included at the top of the vendored `uECC.h`) that `#define`s every exported micro-ecc
  function to an `ef_uECC_*` prefix. NimBLE's tinycrypt doesn't include that header, so it
  keeps the plain names — no clash. Verified: `uECC.c.o` for the C3 exports only
  `ef_uECC_*` symbols, `pio run -e esp32-c3-mini` links clean, and the 25 native tests
  still pass with the vendored copy. `${ecc.build_flags}` and the crypto/keytable sources
  are back in the device build.

  **E4 session (`ecoflow_session.{h,cpp}`, wired into `main.cpp` behind `ECOFLOW_GATT`) —
  VERIFIED on hardware.** NimBLE client: connect, discover rfcomm chars, subscribe, run
  the ECDH → key-info → auth handshake off the host-tested primitives, reassemble `0x5A5A`
  frames, decrypt data frames, decode heartbeats into `EcoflowRichReading`, derive charge
  state from in/out watts. Flash 47.5% on / 41.3% off (opt-out strips via `ecoflow_build.h`).

  Findings from bring-up (the resolved `LIVE-TUNE` questions):
  - **NimBLE callback deadlock (critical):** driving the handshake from inside the notify
    callback and calling a blocking `writeValue` there wedges the BLE host task (and the
    whole loop — the watchdog never even fired). Fix: the notify callback only *buffers*
    complete frames (guarded by a `portMUX`); all decode + writes happen in `poll()` on the
    main task. This is the load-bearing architectural fix.
  - **Range:** at −101 dBm (tent↔beach) the session can't come up; at −59…−71 dBm (co-located)
    it's solid. Added an RSSI gate (`kEcoMinRssiDbm = −88`) — below it we don't grab the
    connection (which would also stop the unit advertising) and stay on passive battery-%.
  - **Framing:** the device sends exactly one complete EncPacket per notification. Handshake
    pubkey + key-info arrive as command frames (type 0x00); everything post-cipher (auth
    result, heartbeats) as data frames (type 0x10). Confirmed.
  - **Acking not required** for the basic power heartbeats — they stream without it, so
    `ackPacket()` stays commented out (avoids any feedback-loop risk). The reference only
    needs acks for extra config/prediction data we don't use.
  - **Offsets confirmed live:** SoC (BMS f32@53), out watts (PD u16@15), and discharge
    remaining (EMS u32@21 → `toEmpty=80min`, a sane estimate) all read correctly.
  - **Time-sync:** telemetry flows without us answering the RTC request; `sendTimeSync()`
    is wired but its necessity for charge/discharge data is unconfirmed (harmless).
- **E5 — Telemetry parse + display.** Extend `EcoflowMonitor`/reading with `chargingWatts`,
  `dischargingWatts`, `socPreciseTenths`, `minutesToFull`, and a charge state. Redesign the
  right column (72×40 is tiny): SoC big + a compact charge line, e.g. `↑ 96W  1:20` when
  charging / `↓ 45W` discharging / `idle`. Validate against HA cloud + the app.
- **E6 — Robustness & coexistence.** Two concurrent GATT connections + scan on one core;
  reconnect/backoff when the phone app steals the slot; session-stale handling; keep the
  passive-advert SoC as a fallback when no session. Update `docs/protocols/ecoflow.md`
  (Tier 2 now implemented) and the README roadmap.

## Non-goals for this plan

- On-device **control** (turning outputs on/off, setting charge limits) — read-only here.
- Porting the protobuf path for the larger EcoFlow models.
- Cloud/WiFi/MQTT — BLE only, consistent with the MVP.
- Storing EcoFlow account **email/password** on-device — only the derived `userId` is
  needed, and only at build time.

## Risks

- **`userId` provisioning** — ✅ resolved; provisioned in `config.h`.
- **micro-ecc ↔ NimBLE symbol collision** — ✅ resolved and verified (symbol prefix; see E4).
- **Handshake interop bugs** — byte order / point encoding / CRC params must match
  bit-for-bit. *Largely de-risked:* E2/E3 are host-tested against independent golden
  vectors — CRC-8/16, both frame layouts, all telemetry offsets, AES-CBC, MD5, the
  secp160r1 ECDH shared secret, IV, session-key derivation, and the auth hash all match.
  The remaining unknowns are live-exchange framing details (packet acking, time-sync) that
  can only be confirmed against the real device at E4.
- **Phone-app contention** — expected; design reconnect for it, don't fight it.
- **Two-connection stability on the C3** — verify NimBLE max-connections and RAM headroom
  early in E4; fall back to prioritizing whichever device matters if unstable.
- **No verification possible in the field today** — this is a multi-session build; the
  first real end-to-end test needs the device, the `userId`, and the phone app closed.
