# bledash-esp32

Tiny ESP32 firmware that connects to your Bluetooth Low Energy (BLE) devices, polls them for status, and shows the readings on a small OLED — so you can glance at what your gear is doing without opening a vendor app on your phone.

Built for a very specific use case (see [MVP scope](#mvp-scope)) but designed so you can add support for more BLE devices, more screens, and more targets over time.

**Status:** early / MVP. First supported devices: **Alpicool** car fridges and **EcoFlow River 2** power stations. First supported hardware: **ESP32‑C3 "MINI" dev board with 0.42″ OLED**.

## Why

Vendor apps for BLE gear (fridges, power stations, chargers, scales, sensors) are all similarly annoying — you unlock your phone, open a proprietary app, wait for BLE pairing, look at a fancy but slow UI, close it. If all you want is *"what temp is the fridge, what's the SoC on the battery"*, an always-on tiny screen wins.

This firmware is a BLE Central that polls one or more BLE Peripherals on a schedule and renders their key metrics on a small OLED. No phone, no cloud, no vendor account.

## MVP scope

- **Hardware:** ESP32‑C3 "MINI" board (2.4 GHz WiFi, BLE 5.0, 4 MB flash, ceramic antenna, USB Type‑C, onboard 0.42″ SSD1306 OLED, 72×40 px).
- **Power:** 5 V via USB‑C from the car's USB port (always‑on while the car is on).
- **Devices:**
  - Alpicool K‑series 12/24V compressor fridge — target temp, actual temp, battery‑protection mode, on/off.
  - EcoFlow River 2 portable power station — battery %, input W, output W, remaining time.
- **UI:** rotating pages on the 72×40 display, one device per page, 3‑second dwell.
- **Cycle:** poll each device once per minute, sleep the radio between polls.

**Non‑goals (for MVP):** battery‑powered operation, e‑ink display, web UI, cloud sync, MQTT bridge, OTA updates, multi‑fridge support. Some of these are on the roadmap; see [`plans/`](plans/).

## Hardware

The MVP targets one specific board because it was €3.34 on AliExpress and it works:

- **MINI ESP32‑C3 Development Board with 0.42" OLED**
  - ESP32‑C3 (RISC‑V single core, 160 MHz)
  - 4 MB flash, WiFi 4, Bluetooth 5.0 LE
  - Onboard 72×40 SSD1306 OLED (I²C, addr `0x3C`)
  - USB Type‑C, ceramic antenna
  - Widely cloned; sold under a dozen names

The firmware is written to be portable to other ESP32 targets (S3, S2, classic ESP32) as long as they have BLE and some sort of I²C/SPI display. PRs welcome.

## Quick start

> ⚠️ Not runnable yet — MVP in progress. This section describes the intended workflow.

```bash
# 1. Install PlatformIO (or use the Arduino IDE with ESP32 board support 3.x+)
pipx install platformio

# 2. Clone and configure
git clone https://github.com/lightheaded/bledash-esp32
cd bledash-esp32
cp config.example.h include/config.h  # set device MAC addresses

# 3. Flash
pio run -t upload
pio device monitor
```

## Roadmap

Tracked as dated plan documents under [`plans/`](plans/). Each plan is a self‑contained proposal; once shipped, it moves to `plans/done/` with a link to the commits.

Near term:
- MVP: Alpicool + EcoFlow on the ESP32‑C3 MINI board (in progress — see `plans/2026-07-08-01-mvp-esp32c3-oled.md`).
- Reverse‑engineer notes for both BLE protocols, published under `docs/protocols/`.
- Support for the LOLIN S3 Mini + 2.13″ e‑ink shield (battery‑powered v2 — see the "off‑grid" plan when it's written).

Later:
- MQTT bridge (optional WiFi mode for home use).
- Web config page over SoftAP for setting device MACs without a rebuild.
- Add more supported devices — smart plugs, ATC/pvvx thermometers, BLE scales.

## Related work / prior art

- **Alpicool BLE protocol** — reverse‑engineered by various community efforts; `docs/protocols/alpicool.md` will link to the ones this project drew from.
- **EcoFlow local API** — the `ecoflow-mqtt` and `hassio-ecoflow` projects have documented the LAN protocol; the BLE protocol is less well documented.
- **ATC / pvvx firmware** for Xiaomi thermometers — inspiration for the "small device, big number, glanceable" UI philosophy.

## Contributing

This is a personal homelab hobby project first and an open‑source project second. If you've got a similar itch, PRs are welcome — but please:

1. Open an issue first for anything larger than a bug fix, so we can agree on the shape.
2. New device support goes in `src/devices/<vendor>_<model>.{cpp,h}` with a documented protocol note under `docs/protocols/`.
3. No proprietary SDKs — everything must be reverse‑engineered from observation, not decompiled from vendor apps.

## License

MIT — see [`LICENSE`](LICENSE).

## Acknowledgements

- The various anonymous forum posters who reverse‑engineered the Alpicool and EcoFlow BLE payloads.
- The ESP32 Arduino BLE library maintainers.
