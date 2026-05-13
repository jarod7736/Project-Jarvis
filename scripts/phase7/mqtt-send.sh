#!/usr/bin/env bash
# Push a command into jarvis/command — the device dispatches it from IDLE
# without requiring a wake word (see src/app/state_machine.cpp:127).
#
# Usage:
#   ./mqtt-send.sh "what time is it"
#   BROKER=192.168.1.50 ./mqtt-send.sh "turn off the office light"
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <command text>" >&2
  exit 1
fi

BROKER="${BROKER:-localhost}"
PORT="${PORT:-1883}"
MSG="$*"

args=( -h "$BROKER" -p "$PORT" -t jarvis/command -m "$MSG" )
if [[ -n "${MQTT_USER:-}" ]]; then
  args+=( -u "$MQTT_USER" )
fi
if [[ -n "${MQTT_PASS:-}" ]]; then
  args+=( -P "$MQTT_PASS" )
fi

echo "[mqtt-send] -> jarvis/command: \"$MSG\"" >&2
exec mosquitto_pub "${args[@]}"
