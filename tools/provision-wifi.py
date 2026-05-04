#!/usr/bin/env python3
"""Provision WiFi credentials to a Project Jarvis CoreS3 over USB Serial.

Run this after a fresh boot, when the device prints:
    [PROV] Listening on USB Serial for up to 180 seconds...

Usage:
    python3 tools/provision-wifi.py [device_path]

Default device path is /dev/ttyACM0. Prompts for SSID (visible) and password
(silent via getpass), then writes a single JSON line over USB Serial. Uses
json.dumps so any quote / backslash / unicode characters in the credentials
are properly escaped — sidesteps every shell-quoting trap.
"""
import getpass
import json
import sys


def main() -> int:
    device = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"

    ssid = input("SSID: ").strip()
    if not ssid:
        print("error: SSID cannot be empty", file=sys.stderr)
        return 2

    pw = getpass.getpass("Password: ")

    payload = json.dumps({"ssid": ssid, "pass": pw}) + "\n"

    try:
        with open(device, "wb", buffering=0) as f:
            f.write(payload.encode("utf-8"))
    except OSError as e:
        print(f"error: could not write to {device}: {e}", file=sys.stderr)
        return 1

    print(f"wrote {len(payload)} bytes to {device}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
