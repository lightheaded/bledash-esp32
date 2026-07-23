#!/usr/bin/env python3
"""Fetch your EcoFlow account userId for the authenticated BLE handshake.

The River 2 Max GATT auth sends MD5(userId + serial). The userId is a static
per-account decimal string; this hits EcoFlow's cloud login once to retrieve it.
After that it is reusable offline — put it in include/config.h as
ECOFLOW_USER_ID (gitignored; never commit it).

Mirrors ha-ef-ble's login flow (custom_components/ef_ble/eflib/login.py):
POST https://<region-host>/auth/login with the password base64-encoded.

Usage:
    scripts/ecoflow_userid.py EMAIL PASSWORD [--host api.ecoflow.com]
    scripts/ecoflow_userid.py --phone DIGITS PASSWORD   # CN accounts (no +86)

Region host: most non-China accounts use api.ecoflow.com; EU accounts often
need api-e.ecoflow.com (try api-a / api-j / api-r if login fails). Phone-only
accounts use api-cn.ecoflow.com.

Treat the password as a live credential; it is only sent to EcoFlow, once.
"""
import argparse
import base64
import json
import sys
import urllib.request


def fetch(identifier: str, password: str, host: str, is_phone: bool) -> dict:
    payload = {
        "scene": "IOT_APP",
        "appVersion": "1.0.0",
        "oauth": {"bundleId": "com.ef.EcoFlow"},
        "userType": "ECOFLOW",
        "password": base64.b64encode(password.encode()).decode(),
    }
    payload["phone" if is_phone else "email"] = identifier
    req = urllib.request.Request(
        f"https://{host}/auth/login",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    return json.load(urllib.request.urlopen(req, timeout=30))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("identifier", help="account email, or phone digits with --phone")
    ap.add_argument("password")
    ap.add_argument("--phone", action="store_true", help="identifier is a phone number")
    ap.add_argument("--host", default=None, help="region host (default auto)")
    args = ap.parse_args()

    host = args.host or ("api-cn.ecoflow.com" if args.phone else "api.ecoflow.com")
    try:
        r = fetch(args.identifier, args.password, host, args.phone)
    except Exception as e:  # noqa: BLE001
        print(f"request to {host} failed: {e}", file=sys.stderr)
        print("try a different region host: --host api-e.ecoflow.com", file=sys.stderr)
        return 2

    if r.get("code") != "0":
        print(f"login failed: {r.get('message')!r} (full: {r})", file=sys.stderr)
        return 1
    user_id = r["data"]["user"]["userId"]
    print(user_id)
    print(f'\nAdd to include/config.h:\n  #define ECOFLOW_USER_ID "{user_id}"',
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
