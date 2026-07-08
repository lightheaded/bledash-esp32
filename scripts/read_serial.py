#!/usr/bin/env -S uv run --quiet --with pyserial --script
"""Reset the ESP32-C3 over its native USB and print serial output for a while.

Non-interactive alternative to `pio device monitor` (which needs a TTY).
Usage: scripts/read_serial.py [port] [seconds]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem101"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

ser = serial.Serial(port, 115200, timeout=0.2)
# USB-Serial-JTAG reset: RTS drives EN (reset), DTR drives GPIO9 (boot select).
ser.setDTR(False)
ser.setRTS(True)   # assert reset
time.sleep(0.1)
ser.setRTS(False)  # release -> boot into app
ser.reset_input_buffer()

deadline = time.time() + duration
while time.time() < deadline:
    line = ser.readline()
    if line:
        sys.stdout.write(line.decode("utf-8", "replace"))
        sys.stdout.flush()
ser.close()
