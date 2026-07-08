# Agent instructions for bledash-esp32

Rules for any AI agent (Claude Code, etc.) working in this repository. Follow them
without being re-asked.

## Never commit private information

Device serials, BLE MACs, internal IP addresses, tokens, and credentials must **never**
appear in tracked files (source, plans, README, commit messages). They live only in
gitignored locations:

- `local/` — notes and secrets (e.g. `local/devices.md`, `local/ha.env`)
- `include/config.h` — build-time device identifiers

In tracked files, refer to *where* a value lives, not the value itself. When pulling data
from Home Assistant (see below), scrub identifiers before writing anything to a tracked
file.

## Free the Alpicool BLE connection before testing against the fridge

The Alpicool K25 accepts **one BLE central at a time** and stops advertising while
connected. Before flashing or running firmware that connects to the fridge, and before any
BLE scan meant to see the fridge, you MUST release the connection slot — otherwise connect
attempts fail confusingly:

1. **Disable the Home Assistant integration** (it holds a persistent connection and retries
   forever) by running from the dev machine:
   ```bash
   scripts/ha-alpicool.py disable
   ```
   **Always re-enable it when the dev/test session is done:**
   ```bash
   scripts/ha-alpicool.py enable
   ```
   Leaving it disabled silently breaks the user's home dashboard. Treat re-enabling as part
   of finishing the task, not an optional follow-up.

2. **Remind the user to force-quit the vendor phone apps** (an agent can't do this):
   - **Alpicool app** — force-kill it (swipe away, not just background). Most common thief of
     the connection mid-session.
   - **EcoFlow app** — usually harmless because the MVP only passively scans EcoFlow adverts
     (never connects). Only flag it if the River 2 Max stops showing up in scans or its
     battery byte goes stale.

The helper talks to HA over its WebSocket API and needs `local/ha.env` (`HA_HOST`,
`HA_TOKEN`). It runs from the dev machine, not over SSH: disabling a config entry is a
WebSocket-only operation and the HA OS SSH shell has no suitable client. HA is also
reachable at `ssh root@<ha-host>` (host in `local/devices.md`) for read-only inspection.

## Two EcoFlows — don't confuse them

There are two EcoFlow units in the house, both on WiFi and visible in HA, both advertising
over BLE (so a scan at home sees both):

- **River 2 Max** (serial prefix `R611`) — **the MVP target.** Portable; used in the car.
- **River 2** (serial prefix `R601`) — stationary, in the techroom. **Not the target.**

Always match the EcoFlow by the River 2 Max's specific serial (from `config.h`), never by
device-type or "any EcoFlow", or the firmware can latch onto the techroom unit. Exact
serials are in `local/devices.md`.

## Source of truth

The authoritative plan is `plans/2026-07-08-01-mvp-esp32c3-oled.md`. Read it before making
architecture or scope decisions.
