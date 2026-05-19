"""Environment-driven configuration for jarvis-notifier.

Everything is overridable via env vars (set in the systemd unit's
Environment= lines or an EnvironmentFile=). No NVS / on-device config here —
this whole service is lobsterboy-side.
"""

from __future__ import annotations

import os

# ── HTTP listener ───────────────────────────────────────────────────────────
LISTEN_HOST = os.environ.get("NOTIFIER_LISTEN_HOST", "0.0.0.0")
# 8081 so it sits next to oc-personal-runner on 8080 without colliding.
LISTEN_PORT = int(os.environ.get("NOTIFIER_LISTEN_PORT", "8081"))

# ── MQTT broker (HA Mosquitto add-on or any 1883/anon broker) ───────────────
MQTT_HOST = os.environ.get("NOTIFIER_MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("NOTIFIER_MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("NOTIFIER_MQTT_USER", "")
MQTT_PASS = os.environ.get("NOTIFIER_MQTT_PASS", "")
MQTT_KEEPALIVE_SEC = int(os.environ.get("NOTIFIER_MQTT_KEEPALIVE_SEC", "60"))
# Client ID must be unique across the broker — collisions cause silent
# disconnect storms. The notifier and the device both connect to the same
# broker; keep IDs distinct.
MQTT_CLIENT_ID = os.environ.get("NOTIFIER_MQTT_CLIENT_ID", "jarvis-notifier")

# Topics — must match firmware src/config.h:kMqttTopicSpeak / kMqttTopicState.
# Override only if you rename the topics on both sides.
MQTT_TOPIC_SPEAK = os.environ.get("NOTIFIER_MQTT_TOPIC_SPEAK", "jarvis/speak")
MQTT_TOPIC_STATE = os.environ.get("NOTIFIER_MQTT_TOPIC_STATE", "jarvis/state")

# ── Disk-backed state ──────────────────────────────────────────────────────
# Queue holds pending medium-tier items; log records every dispatch for
# audit (priority router decisions get cheap to second-guess this way).
# Default location lives under /var/lib/jarvis-notifier — created by the
# deploy script with the service user as owner.
STATE_DIR = os.environ.get("NOTIFIER_STATE_DIR", "/var/lib/jarvis-notifier")
QUEUE_PATH = os.environ.get("NOTIFIER_QUEUE_PATH", os.path.join(STATE_DIR, "queue.json"))
LOG_PATH = os.environ.get("NOTIFIER_LOG_PATH", os.path.join(STATE_DIR, "log.jsonl"))

# Cap queue depth so a runaway producer can't fill the disk. Older items
# get dropped FIFO when over cap; dropped items still get logged so the
# loss is visible.
QUEUE_MAX_DEPTH = int(os.environ.get("NOTIFIER_QUEUE_MAX_DEPTH", "100"))

# ── Pushover (feature-flagged) ─────────────────────────────────────────────
# Both keys must be set; if either is empty, Pushover delivery is skipped
# and high-priority notifications still flow to the device via MQTT.
# Sign up at pushover.net (~$5 one-time per platform).
PUSHOVER_TOKEN = os.environ.get("PUSHOVER_TOKEN", "")
PUSHOVER_USER_KEY = os.environ.get("PUSHOVER_USER_KEY", "")
PUSHOVER_API_URL = os.environ.get(
    "PUSHOVER_API_URL", "https://api.pushover.net/1/messages.json"
)
# How long to wait for Pushover before giving up. Their API is usually
# <500ms; 10s is generous and avoids hanging the FastAPI request task.
PUSHOVER_TIMEOUT_SEC = float(os.environ.get("PUSHOVER_TIMEOUT_SEC", "10"))

# ── Drain timing ────────────────────────────────────────────────────────────
# Seconds to wait after the device transitions to IDLE before publishing
# the next queued medium item. Without this, drain → SPEAKING → IDLE
# (TTS done) → drain again creates back-to-back announcements with no
# breathing room — better to give the user a moment to register the
# previous one and optionally wake-word their way in.
IDLE_DRAIN_DELAY_SEC = float(os.environ.get("NOTIFIER_IDLE_DRAIN_DELAY_SEC", "3"))

# Per-tier defaults for Pushover priority. Pushover scale: -2 (silent) ..
# 2 (emergency, requires ack). We never use 2 (would require config of
# retry/expire and is overkill for this use case).
PUSHOVER_PRIORITY_HIGH = int(os.environ.get("PUSHOVER_PRIORITY_HIGH", "1"))
PUSHOVER_PRIORITY_MED = int(os.environ.get("PUSHOVER_PRIORITY_MED", "0"))
PUSHOVER_PRIORITY_LOW = int(os.environ.get("PUSHOVER_PRIORITY_LOW", "-1"))


def pushover_enabled() -> bool:
    """True iff both Pushover keys are configured. Otherwise dispatch
    routes only via MQTT / log, and Pushover sends are skipped with a
    warning."""
    return bool(PUSHOVER_TOKEN) and bool(PUSHOVER_USER_KEY)
