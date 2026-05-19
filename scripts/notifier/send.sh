#!/usr/bin/env bash
# Send a one-off notification to the running jarvis-notifier service.
#
# Usage:
#   send.sh <high|medium|low> "message text" [source] [title]
#
# Examples:
#   send.sh high "Meeting in 5 minutes"
#   send.sh medium "boat: next step is to order the bilge pump" cron-resurface
#   send.sh low "test note" deploy-validation "Heartbeat"
#
# Tunable env vars:
#   NOTIFIER_URL  default http://localhost:8081

set -euo pipefail

TIER="${1:-}"
TEXT="${2:-}"
SOURCE="${3:-cli}"
TITLE="${4:-}"

if [ -z "$TIER" ] || [ -z "$TEXT" ]; then
    echo "usage: $0 <high|medium|low> \"message text\" [source] [title]" >&2
    exit 64
fi

case "$TIER" in
    high|medium|low) ;;
    *) echo "error: tier must be high|medium|low (got: $TIER)" >&2; exit 64 ;;
esac

URL="${NOTIFIER_URL:-http://localhost:8081}/notify"

# Build the JSON via python so we don't fight quoting and have correct
# escapes for arbitrary message text. jq would work too but python is
# guaranteed to be present on lobsterboy.
BODY=$(python3 - "$TIER" "$TEXT" "$SOURCE" "$TITLE" <<'PY'
import json, sys
tier, text, source, title = sys.argv[1:5]
payload = {"tier": tier, "text": text, "source": source}
if title:
    payload["title"] = title
print(json.dumps(payload))
PY
)

curl -fsS -X POST "$URL" \
    -H 'Content-Type: application/json' \
    -d "$BODY" \
    | python3 -m json.tool
