# Telemetry logging + upload — performance graphs for fridge & battery

- **Date:** 2026-07-24
- **Status:** 🚧 in progress — server side live and verified end-to-end (see
  "Server side" below); **T1 (host-testable logger core) implemented** on branch
  `telemetry-logging`, behind `TELEMETRY_UPLOAD` (off by default). T2–T5 not
  started (need hardware / a field cycle).
- **Depends on:** v0.2.0 (fridge driver + EcoFlow GATT telemetry). Touches
  `main.cpp` and adds a new `src/telemetry/` layer; no changes to the BLE
  drivers themselves.
- **Motivation:** answer performance questions that need time-series data, not
  a glanceable display: how fast does the fridge pull down from ambient, how
  much cold does it retain unpowered (parked, compressor off), how long does
  the battery last at a given draw, what does solar recovery look like. All of
  that unfolds over hours while nobody is watching — usually with **no network
  in range**.

## The shape of the problem

The interesting curves happen while the car is parked and offline. Any design
that only records while a network is up misses exactly the data we want. So:

1. **Local flash logging is the backbone** — one sample per poll cycle
   (60 s), always, network or not.
2. **Upload is a drain, not a pipeline** — whenever WiFi happens to be
   available (phone hotspot in the car, typically), the accumulated backlog is
   pushed out with original timestamps and only then discarded.
3. **The sink must accept late timestamped samples.** Ordinary
   current-value-only sinks (plain MQTT/state APIs) can't represent "here's
   what happened 9 hours ago". The server side must ingest history.

The display/UX of v0.2.0 is unchanged. Analysis happens off-device (Grafana or
anything else that can query the sink); the firmware never renders graphs.

## Wire format & endpoint — generic by design

The device speaks **InfluxDB line protocol** over a single HTTPS POST with
HTTP basic auth:

```
POST <TELEMETRY_URL>?precision=s
Authorization: Basic <TELEMETRY_USER:TELEMETRY_PASS>

bledash,device=car fridge_temp_c=4,fridge_setpoint_c=-18,fridge_on=1 1753364000
bledash,device=car eco_soc_pct=87.4,eco_in_w=0,eco_out_w=52,eco_remain_min=433 1753364060
...
```

Line protocol was chosen because it is trivial to emit from C (one `snprintf`
per line), carries explicit timestamps, and is accepted by many self-hostable
sinks: InfluxDB, VictoriaMetrics, vmagent (relaying into Prometheus remote
write), Telegraf. Nothing in this repo assumes any particular backend — the
endpoint, credentials, and device tag all live in gitignored `config.h`.

Fields (all optional per line — emit what's valid that minute):

| field | source |
|---|---|
| `fridge_temp_c`, `fridge_setpoint_c`, `fridge_on` | Alpicool QUERY (also valid while compressor is off — retention curves come free) |
| `eco_soc_pct` | GATT SoC when authenticated, else passive advert % |
| `eco_in_w`, `eco_out_w`, `eco_remain_min` | EcoFlow GATT session only |

## Server side (reference deployment — already live)

A stock **vmagent** behind an authenticated public HTTPS ingress, remote-writing
into an existing Prometheus that has
`storage.tsdb.out_of_order_time_window: 14d` set (that window is what makes
backlogged uploads land at their true timestamps; Prometheus ≥2.39). Verified
end-to-end: a sample POSTed with a 2-hour-old timestamp appears in Prometheus
at the correct historical time; unauthenticated requests get 401. Grafana
queries `bledash_*` metrics directly.

Zero custom server code. Hostnames, IPs, and credentials are deliberately not
in this repo — deployment details live in the (private) infra repo and
`local/telemetry-ingest.md`.

## Opt-in, like ECOFLOW_GATT

Ships behind a compile-time flag so the default build stays WiFi-free and
carries no size/RAM/coexistence cost:

```c
// config.example.h additions
#define TELEMETRY_UPLOAD 0        // 1 = log to flash + upload over WiFi
#define WIFI_SSID ""              // hotspot/home AP to join opportunistically
#define WIFI_PASS ""
#define TELEMETRY_URL ""          // e.g. "https://host/write" (line protocol)
#define TELEMETRY_USER ""
#define TELEMETRY_PASS ""
#define TELEMETRY_DEVICE_TAG "car"
```

With the flag off, the linker strips the logger, WiFi, TLS, and the CA bundle —
same pattern that keeps ECOFLOW_GATT=0 builds lean. A build guard `#error`s if
the flag is on but URL/SSID are empty.

TLS: the firmware embeds the **ISRG Root X1** CA (public certificate, fine to
commit) for Let's Encrypt endpoints; `config.h` can override with a custom CA
PEM for other setups.

## Storage design

- **LittleFS on the default FS partition** (~1.4 MB on the stock 4 MB
  partition table — no partition change needed; app is at ~48% of its 1.25 MB
  slot with GATT on).
- **Append-only CSV-ish lines, two-file ring** (`log.a`/`log.b`, ~600 KB
  each): when the active file fills, switch and truncate the other. ~50 B/min
  ≈ 70 KB/day → the ring holds ~2 weeks; matches the server's 14 d
  out-of-order window. Flash wear is negligible at this rate.
- **Upload cursor** (file + offset) persisted separately; advanced **only
  after a 2xx** from the endpoint, so a dropped connection re-sends rather
  than loses. Uploads are chunked (~300 lines / ~16 KB per POST).
- **Timestamps (as implemented in T1):** each record carries a one-char flag —
  `A<epoch>` when the RTC looks set (epoch ≥ 2023-11-14), else `R<sec-since-boot>`.
  Nothing is rewritten in place: a relative record is resolved to real time at
  *upload* by adding this boot's epoch offset (`bootEpoch = now − millis()/1000`,
  computed once SNTP syncs in T3). Records from a boot that never synced can't be
  placed on the timeline and are dropped with a counter. This is simpler than the
  original rewrite-in-place idea and safe in practice: the ESP32 is powered by the
  EcoFlow, reboots are rare, and the hotspot appears at least daily.

## WiFi ↔ BLE coexistence — the load-bearing risk

The ESP32-C3 has **one radio** shared by BLE and WiFi. Today the loop blocks
in 2.5 s BLE scan windows and holds up to two GATT connections. Adding STA +
TLS on top is the genuine unknown, so it gets spiked first (T2) before any
uploader work:

- Coexistence is supported by the IDF scheduler under Arduino, but airtime is
  split — expect slower scans and possibly GATT supervision timeouts.
- **Fallback design if always-on STA is unstable:** upload *windows* — every
  N minutes (or when the backlog exceeds a threshold), pause BLE scanning,
  associate, sync NTP, drain the backlog, disconnect, resume BLE. The fridge
  poll cadence (60 s) tolerates a missed cycle; readings just go stale ~2 min.
- `WiFiClientSecure` is allocated only for the duration of an upload
  (~45 KB heap during handshake) and freed after, so the steady-state RAM
  cost is near zero.

## Milestones

- **T1 — Logger (host-testable core). ✅ done** (`telemetry-logging`).
  `src/telemetry/`: `Sample` struct, line-protocol codec (`line_protocol`), and
  two-segment ring writer + upload cursor behind a `FileStore` interface
  (`ring_log`), with a LittleFS binding + `Logger` facade for the device. 23
  native tests cover the codec and the ring/cursor edge cases (wrap + eviction,
  partial upload, reboot mid-file, reboot across a wrap, drop accounting). One
  sample per poll cycle is assembled from the live readings and appended, wired
  into `loop()` behind `TELEMETRY_UPLOAD` (off by default: the default build is
  byte-identical, flag-on adds ~74 KB for LittleFS). `readBatch`/`commit` (the
  uploader's drain API) are built and tested here; the uploader that calls them
  is T3. Smoke-tested on the ESP32-C3: LittleFS mounts, a sample is appended
  each cycle, and the backlog persists across a reboot (`logging ready (pending
  41 B)` after a reset), confirming the meta/cursor reload on real flash.
- **T2 — Coexistence spike.** STA join + SNTP while both GATT sessions run.
  Measure: scan hit rate, fridge poll success, EcoFlow session stability, heap
  headroom (with TLS handshake). Decide always-on vs upload-window model.
  *Everything after this adapts to what T2 finds.*
- **T3 — Uploader.** Backlog drain over HTTPS with basic auth, chunking,
  2xx-gated cursor advance, retry/backoff, epoch backfill of pre-sync samples.
  Serial-log a one-line summary per drain (lines sent, HTTP status, seconds).
- **T4 — Field verification.** A real drive + overnight parked cycle;
  confirm the cooldown/retention/discharge curves render correctly from the
  uploaded data with true timestamps.
- **T5 — Docs + release.** README roadmap update ("MQTT bridge" item replaced
  by this), config.example.h documented, PR. Public docs describe the feature
  as "bring your own line-protocol endpoint".

## Non-goals

- On-device web UI / CSV download / SoftAP (Grafana-or-similar is the
  frontend; revisit only if a network-less readout is ever needed).
- MQTT, Home Assistant integration, or any live "current state" push — the
  sink's history is the product; HA can be layered on later if wanted.
- Remote control over WiFi (BLE + button remain the only control paths).
- OTA updates (separate concern, still on the roadmap).

## Security posture

The upload credential is compiled into the device and travels in the car, so
the threat model treats it as **eventually extractable** (ESP32-C3 flash is
unencrypted by default; anyone who steals the device can dump it). The design
keeps the blast radius of that leak small rather than trying to make the secret
unextractable:

- **The credential can only write metrics.** It cannot read existing data,
  reach other services, or touch the cluster. On the sink side a valid
  credential is additionally constrained by a relabel allowlist to the
  `bledash_*` metric names and the `{device}` label — verified live: a rogue
  metric name is dropped and unexpected labels are stripped. So a leaked
  credential can't even pollute arbitrary series or balloon cardinality.
- **Transport:** HTTPS only (HTTP 301-redirects), TLS 1.1 refused, LE cert.
- **Edge protections:** basicAuth on every path (vmagent's own admin/debug
  endpoints included) plus a rate-limit that caps a flood — verified: a burst
  returns 429s.
- **Device-loss runbook:** if the ESP32 is lost or stolen, rotate immediately —
  the leaked WiFi hotspot password and ingest credential are both in its flash:
  1. Change the phone hotspot password.
  2. Regenerate the ingest htpasswd (`openssl passwd -apr1`), re-encrypt the
     sink's secret, redeploy.
  3. Reflash the recovered/replacement device with the new `config.h`.
  Until step 2, the old credential can still write `bledash_*` samples (bounded,
  low-value) — not urgent, but do it.
- **Not doing flash encryption in v1:** it defends a low-value secret at real
  complexity cost; rotation-on-loss is the better trade. Revisit only if the
  device ever holds something more sensitive.

## Risks

- **Coexistence instability** — spiked first (T2); upload-window fallback
  keeps the feature viable even in the worst case.
- **TLS heap pressure alongside NimBLE** — transient allocation only during
  drains; measured in T2 with both GATT links up.
- **Hotspot quirks** — phone hotspots NAT everything and sometimes sleep;
  uploads are already retry-safe by design (cursor gated on 2xx).
- **Clock correctness** — SNTP on every association; samples carry server-side
  sanity (the sink rejects samples outside its out-of-order window, which
  bounds how wrong a stale-clock upload can be).
