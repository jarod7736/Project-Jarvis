#!/usr/bin/env bash
# Tail jarvis/# on the broker so we can watch FSM state transitions live.
#
# Usage:
#   ./mqtt-watch.sh                 # broker on localhost, anon
#   BROKER=192.168.1.50 ./mqtt-watch.sh
#   BROKER=ha.local MQTT_USER=jarvis MQTT_PASS=... ./mqtt-watch.sh
#
# Requires mosquitto-clients (apt install mosquitto-clients).
set -euo pipefail

BROKER="${BROKER:-localhost}"
PORT="${PORT:-1883}"

args=( -h "$BROKER" -p "$PORT" -v -t 'jarvis/#' )
if [[ -n "${MQTT_USER:-}" ]]; then
  args+=( -u "$MQTT_USER" )
fi
if [[ -n "${MQTT_PASS:-}" ]]; then
  args+=( -P "$MQTT_PASS" )
fi

echo "[mqtt-watch] subscribing to jarvis/# on $BROKER:$PORT" >&2
exec mosquitto_sub "${args[@]}"
