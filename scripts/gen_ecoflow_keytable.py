#!/usr/bin/env python3
"""Generate src/devices/ecoflow_keytable.cpp from the ha-ef-ble key table.

The EcoFlow session-key derivation indexes a fixed 65280-byte table extracted
from the vendor app. It is a shared constant (not a user secret): the same bytes
ship openly in rabits/ha-ef-ble (eflib/keydata.py) and rabits/ef-ble-reverse
(login_key.bin). This script fetches that source and emits the C++ array so the
firmware carries an identical copy.

Usage:
    scripts/gen_ecoflow_keytable.py            # fetch from GitHub, regenerate
    scripts/gen_ecoflow_keytable.py keydata.py # use a local keydata.py

Expected decoded length 0xFF00 (65280); sha256
3d1ba59b40c46bc86ba35fa231a3db196b448bbdfb1635c88ddd42f24abd4b2a
"""
import base64
import hashlib
import importlib.util
import os
import re
import sys
import urllib.request

SRC_URL = (
    "https://raw.githubusercontent.com/rabits/ha-ef-ble/main/"
    "custom_components/ef_ble/eflib/keydata.py"
)
OUT = os.path.join(
    os.path.dirname(__file__), "..", "src", "devices", "ecoflow_keytable.cpp"
)
EXPECT_LEN = 0xFF00
EXPECT_SHA = "3d1ba59b40c46bc86ba35fa231a3db196b448bbdfb1635c88ddd42f24abd4b2a"


def load_bytes(arg: str | None) -> bytes:
    if arg:
        spec = importlib.util.spec_from_file_location("keydata", arg)
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        return bytes(m._data)
    text = urllib.request.urlopen(SRC_URL, timeout=30).read().decode()
    b64 = "".join(re.findall(r'b"\\\n(.*?)"', text, re.S)).replace("\\\n", "")
    if not b64:
        # Fall back to exec if the source layout changed.
        ns: dict = {"base64": base64}
        exec(text, ns)
        return bytes(ns["_data"])
    return base64.b64decode(b64)


def main() -> int:
    data = load_bytes(sys.argv[1] if len(sys.argv) > 1 else None)
    sha = hashlib.sha256(data).hexdigest()
    print(f"decoded length: {len(data)}  sha256: {sha}")
    if len(data) != EXPECT_LEN:
        print(f"ERROR: expected {EXPECT_LEN} bytes", file=sys.stderr)
        return 1
    if sha != EXPECT_SHA:
        print("WARNING: sha256 differs from the pinned value", file=sys.stderr)

    with open(OUT, "w") as f:
        f.write(
            "// EcoFlow session-key table — a fixed vendor constant (65280 bytes) used by\n"
            "// genSessionKey() to derive the AES session key. Not a user secret: it is the\n"
            "// same table shipped openly in rabits/ha-ef-ble (eflib/keydata.py) and\n"
            "// rabits/ef-ble-reverse (login_key.bin). GENERATED — do not edit by hand.\n"
            "// Regenerate: scripts/gen_ecoflow_keytable.py\n"
            '#include "devices/ecoflow_build.h"\n'
            "#if ECOFLOW_ENABLE_GATT_TU\n\n"
            '#include "devices/ecoflow_keytable.h"\n\n'
            "const uint8_t kEcoflowKeyTable[kEcoflowKeyTableLen] = {\n"
        )
        for i in range(0, len(data), 16):
            f.write("  " + "".join(f"0x{b:02x}," for b in data[i : i + 16]) + "\n")
        f.write("};\n\n#endif  // ECOFLOW_ENABLE_GATT_TU\n")
    print(f"wrote {os.path.relpath(OUT)} ({os.path.getsize(OUT)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
