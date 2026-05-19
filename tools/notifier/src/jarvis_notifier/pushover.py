"""Pushover delivery client.

Pushover API: POST https://api.pushover.net/1/messages.json
Form fields:
  token     application API token (we own)
  user      user/group key (recipient)
  message   message body (required, max 1024 chars)
  title     optional title (max 250 chars, defaults to app name)
  priority  -2 silent, -1 quiet, 0 normal, 1 high (bypass quiet hours),
            2 emergency (we never use; needs retry/expire and ack)
  sound     optional sound name (we leave to user's per-app default)

Failure modes worth knowing:
  - 400: bad request (invalid token, message too long). We log + give up.
  - 429: rate limited. Pushover has a 10k msg/month soft quota on the
         basic plan; for our use case (a handful per day) we will never
         hit this in practice.
  - network: timeout / DNS. Logged, surfaced in /healthz; high tier
         still made it to the device via MQTT, so the user heard it —
         just didn't see it on the phone.
"""

from __future__ import annotations

import logging
from typing import Any

import httpx

from . import config

log = logging.getLogger(__name__)


async def send(
    message: str,
    *,
    title: str | None = None,
    priority: int = 0,
) -> dict[str, Any]:
    """Send one message. Returns a dict with `{ok, status, body, skipped}`.

    `skipped=True` means Pushover was not configured (no token/user) —
    not an error, just nothing to do. The caller should not retry.
    """
    if not config.pushover_enabled():
        log.warning("pushover skipped: keys not configured")
        return {"ok": False, "skipped": True, "reason": "pushover-not-configured"}

    data = {
        "token": config.PUSHOVER_TOKEN,
        "user": config.PUSHOVER_USER_KEY,
        # Truncate defensively — Pushover rejects >1024 chars outright.
        "message": message[:1024],
        "priority": priority,
    }
    if title:
        data["title"] = title[:250]

    try:
        async with httpx.AsyncClient(timeout=config.PUSHOVER_TIMEOUT_SEC) as client:
            resp = await client.post(config.PUSHOVER_API_URL, data=data)
    except httpx.HTTPError as exc:
        log.error("pushover network error: %s", exc)
        return {"ok": False, "skipped": False, "error": f"network: {exc}"}

    body: dict[str, Any] = {}
    try:
        body = resp.json()
    except ValueError:
        body = {"raw": resp.text[:200]}

    if resp.status_code != 200 or body.get("status") != 1:
        log.error(
            "pushover non-success: status=%d body=%s",
            resp.status_code, body,
        )
        return {
            "ok": False,
            "skipped": False,
            "status": resp.status_code,
            "body": body,
        }

    return {"ok": True, "skipped": False, "status": resp.status_code, "body": body}
