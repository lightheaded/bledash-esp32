#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["websockets>=12"]
# ///
"""Toggle the Home Assistant Alpicool BLE integration from the dev machine.

The K25 fridge accepts a single BLE central. HA (via its Bluetooth proxies)
holds a persistent connection and retries reconnects forever, so it must be
disabled while developing/testing this firmware against the fridge, and
re-enabled afterwards. See AGENTS.md.

Usage:
    scripts/ha-alpicool.py status
    scripts/ha-alpicool.py disable
    scripts/ha-alpicool.py enable

Config (never committed): local/ha.env with
    HA_HOST=<ip-or-hostname>          # no scheme, no port
    HA_TOKEN=<long-lived access token>
Create the token in HA: your profile -> Security -> Long-lived access tokens.
Environment variables HA_HOST / HA_TOKEN override the file.
"""

import asyncio
import json
import os
import sys
from pathlib import Path

import websockets

DOMAIN = "alpicool_ble"
REPO_ROOT = Path(__file__).resolve().parent.parent
ENV_FILE = REPO_ROOT / "local" / "ha.env"


def load_config() -> tuple[str, str]:
    conf: dict[str, str] = {}
    if ENV_FILE.exists():
        for line in ENV_FILE.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, _, value = line.partition("=")
                conf[key.strip()] = value.strip()
    host = os.environ.get("HA_HOST") or conf.get("HA_HOST")
    token = os.environ.get("HA_TOKEN") or conf.get("HA_TOKEN")
    if not host or not token:
        sys.exit(
            f"Missing HA_HOST and/or HA_TOKEN.\n"
            f"Create {ENV_FILE} (gitignored) with:\n"
            f"  HA_HOST=<ha ip>\n"
            f"  HA_TOKEN=<long-lived access token>\n"
            f"Token: HA profile -> Security -> Long-lived access tokens."
        )
    return host, token


class HaWs:
    def __init__(self, ws):
        self.ws = ws
        self.msg_id = 0

    async def command(self, payload: dict) -> dict:
        self.msg_id += 1
        payload = {"id": self.msg_id, **payload}
        await self.ws.send(json.dumps(payload))
        while True:
            msg = json.loads(await self.ws.recv())
            if msg.get("id") == self.msg_id and msg.get("type") == "result":
                if not msg.get("success"):
                    sys.exit(f"HA command failed: {msg.get('error')}")
                return msg.get("result") or {}


async def run(action: str) -> None:
    host, token = load_config()
    async with websockets.connect(f"ws://{host}:8123/api/websocket") as ws:
        assert json.loads(await ws.recv())["type"] == "auth_required"
        await ws.send(json.dumps({"type": "auth", "access_token": token}))
        auth = json.loads(await ws.recv())
        if auth["type"] != "auth_ok":
            sys.exit(f"HA auth failed: {auth.get('message', auth)}")

        ha = HaWs(ws)
        entries = await ha.command(
            {"type": "config_entries/get", "domain": DOMAIN}
        )
        if not entries:
            sys.exit(f"No config entry found for domain '{DOMAIN}'.")
        entry = entries[0]

        if action == "status":
            print(
                f"{entry['title']}: state={entry['state']} "
                f"disabled_by={entry['disabled_by']}"
            )
            return

        disabled_by = "user" if action == "disable" else None
        current = entry["disabled_by"]
        if current == disabled_by:
            print(f"{entry['title']}: already {action}d (disabled_by={current}).")
            return

        result = await ha.command(
            {
                "type": "config_entries/disable",
                "entry_id": entry["entry_id"],
                "disabled_by": disabled_by,
            }
        )
        note = " (HA restart required!)" if result.get("require_restart") else ""
        print(f"{entry['title']}: {action}d{note}.")
        if action == "disable":
            print("HA has released the fridge; it may take ~10 s to advertise again.")


def main() -> None:
    if len(sys.argv) != 2 or sys.argv[1] not in ("status", "disable", "enable"):
        sys.exit(__doc__.strip())
    asyncio.run(run(sys.argv[1]))


if __name__ == "__main__":
    main()
