#!/usr/bin/env python3
"""Provision Jarvis credentials to a CoreS3 over USB Serial.

Run after a fresh boot, when the device prints:
    [PROV] Listening on USB Serial for up to 180 seconds...

Usage:
    python3 tools/provision-wifi.py [--ota] [--no-wifi] [--device PATH]

Examples:
    python3 tools/provision-wifi.py                    # WiFi only (default)
    python3 tools/provision-wifi.py --ota              # WiFi + OTA password / firmware URL
    python3 tools/provision-wifi.py --no-wifi --ota    # OTA only (WiFi already set)

Filename kept as provision-wifi.py for back-compat with prior phases; the
script now provisions any subset of the bag-of-keys JSON the device's
provisionFromSerial() accepts. Uses json.dumps so any quote / backslash /
unicode characters in the credentials are properly escaped — sidesteps every
shell-quoting trap.
"""
import argparse
import getpass
import json
import sys


def main() -> int:
    p = argparse.ArgumentParser(
        description="Provision Jarvis credentials over USB Serial.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--device", default="/dev/ttyACM0",
                   help="Serial device path (default: /dev/ttyACM0)")
    p.add_argument("--no-wifi", action="store_true",
                   help="Skip SSID/password prompts")
    p.add_argument("--ota", action="store_true",
                   help="Also prompt for ota_pass + fw_url (Phase 7 OTA)")
    args = p.parse_args()

    payload: dict[str, str] = {}

    if not args.no_wifi:
        ssid = input("SSID: ").strip()
        if not ssid:
            print("error: SSID cannot be empty (use --no-wifi to skip)", file=sys.stderr)
            return 2
        pw = getpass.getpass("WiFi password: ")
        payload["ssid"] = ssid
        payload["pass"] = pw

    if args.ota:
        # Empty inputs are skipped — partial OTA provisioning is fine.
        ota_pass = getpass.getpass("OTA password (ota_pass, blank to skip): ")
        if ota_pass:
            payload["ota_pass"] = ota_pass
        fw_url = input("Firmware URL (fw_url, blank to skip): ").strip()
        if fw_url:
            payload["fw_url"] = fw_url

    if not payload:
        print("error: nothing to send (use --ota and/or omit --no-wifi)", file=sys.stderr)
        return 2

    line = json.dumps(payload) + "\n"

    try:
        with open(args.device, "wb", buffering=0) as f:
            f.write(line.encode("utf-8"))
    except OSError as e:
        print(f"error: could not write to {args.device}: {e}", file=sys.stderr)
        return 1

    # Echo back what we sent — secrets shown as <set> only.
    secret_keys = {"pass", "ota_pass", "ha_token", "oc_key", "tts_api_key"}
    redacted = {k: ("<set>" if k in secret_keys else v) for k, v in payload.items()}
    print(f"wrote {len(line)} bytes to {args.device}: {json.dumps(redacted)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
