"""Environment-driven configuration for morning-brief.

Both target services run on the same lobsterboy host so localhost URLs
are the right defaults. Override via the systemd unit's Environment=
lines (or export before running manually).
"""

from __future__ import annotations

import os

# ── Upstream services ──────────────────────────────────────────────────────
# Where to POST the chat-completions request. The oc-personal-runner
# listens here; model=oc-personal triggers the agent loop (brain + gcal
# + gmail tools attached).
OC_PERSONAL_URL = os.environ.get("OC_PERSONAL_URL", "http://localhost:8080")
OC_PERSONAL_MODEL = os.environ.get("OC_PERSONAL_MODEL", "oc-personal")

# Where to POST the brief. The notifier service routes by tier and
# decides whether to MQTT-publish, Pushover-push, queue, or just log.
NOTIFIER_URL = os.environ.get("NOTIFIER_URL", "http://localhost:8081")

# ── Behavior knobs ─────────────────────────────────────────────────────────
# Tier for the morning brief. High = TTS now + Pushover; medium = TTS
# on next idle, no phone push; low = log only (testing). Default high —
# this fires at 08:00 when the user expects to be near or arriving at
# the desk, and the phone push backstops the case where they're not.
BRIEF_TIER = os.environ.get("MORNING_BRIEF_TIER", "high")

# What appears as the source attribution in the notifier log + the
# Pushover title. Helps when you're auditing what spoke to you when.
BRIEF_SOURCE = os.environ.get("MORNING_BRIEF_SOURCE", "morning-brief")
BRIEF_TITLE = os.environ.get("MORNING_BRIEF_TITLE", "Today's focus")

# ── Timeouts ───────────────────────────────────────────────────────────────
# Agent calls can take 10-30s when multiple tool calls are involved
# (gcal_list_events + brain_list_projects + maybe a brain_search). Give
# it a generous budget — this is a scheduled job, not an interactive
# request, and a slow but successful brief beats a timed-out skip.
OC_TIMEOUT_SEC = float(os.environ.get("OC_TIMEOUT_SEC", "60"))
# Notifier POST is local; should always be sub-second.
NOTIFIER_TIMEOUT_SEC = float(os.environ.get("NOTIFIER_TIMEOUT_SEC", "10"))

# ── Dry-run ────────────────────────────────────────────────────────────────
# When truthy ("1", "true", "yes"), print the brief to stdout and skip
# the notifier POST. Useful for `./deploy.sh test` and for tuning the
# prompt without spamming the device. Empty/unset = real send.
DRY_RUN = os.environ.get("MORNING_BRIEF_DRY_RUN", "").strip().lower() in (
    "1", "true", "yes",
)
