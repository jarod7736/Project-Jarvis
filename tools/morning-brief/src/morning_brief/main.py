"""Entry point: ask oc-personal for ONE focus, post it to jarvis-notifier."""

from __future__ import annotations

import asyncio
import logging
import sys
from typing import Any

import httpx

from . import config
from .prompt import PROMPT

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
log = logging.getLogger("morning_brief")


async def ask_agent(client: httpx.AsyncClient) -> str | None:
    """POST the brief prompt to oc-personal-runner's chat-completions
    endpoint. Returns the assistant's text reply, or None on failure."""
    url = f"{config.OC_PERSONAL_URL.rstrip('/')}/v1/chat/completions"
    body: dict[str, Any] = {
        "model": config.OC_PERSONAL_MODEL,
        "messages": [{"role": "user", "content": PROMPT}],
    }
    log.info("calling oc-personal %s", url)
    try:
        resp = await client.post(url, json=body, timeout=config.OC_TIMEOUT_SEC)
    except httpx.HTTPError as exc:
        log.error("oc-personal request failed: %s", exc)
        return None

    if resp.status_code != 200:
        # Surface body for debugging; oc-personal returns informative
        # error envelopes for missing keys / unloaded models.
        log.error("oc-personal HTTP %d: %s", resp.status_code, resp.text[:500])
        return None

    try:
        data = resp.json()
    except ValueError:
        log.error("oc-personal returned non-JSON: %s", resp.text[:200])
        return None

    # OpenAI chat-completions shape: choices[0].message.content
    try:
        text = data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        log.error("oc-personal response missing choices[0].message.content: %s",
                  str(data)[:500])
        return None

    text = (text or "").strip()
    if not text:
        log.error("oc-personal returned empty content")
        return None

    log.info("agent reply (%d chars): %s", len(text), text)
    return text


async def send_to_notifier(client: httpx.AsyncClient, text: str) -> bool:
    """POST the brief text to the notifier as a high-tier proactive push."""
    url = f"{config.NOTIFIER_URL.rstrip('/')}/notify"
    body = {
        "text": text,
        "tier": config.BRIEF_TIER,
        "source": config.BRIEF_SOURCE,
        "title": config.BRIEF_TITLE,
    }
    log.info("posting to notifier %s tier=%s", url, config.BRIEF_TIER)
    try:
        resp = await client.post(
            url, json=body, timeout=config.NOTIFIER_TIMEOUT_SEC,
        )
    except httpx.HTTPError as exc:
        log.error("notifier request failed: %s", exc)
        return False

    if resp.status_code != 200:
        log.error("notifier HTTP %d: %s", resp.status_code, resp.text[:500])
        return False

    try:
        data = resp.json()
        log.info("notifier accepted: %s", data)
    except ValueError:
        log.info("notifier accepted (non-JSON response): %s", resp.text[:200])
    return True


async def run() -> int:
    async with httpx.AsyncClient() as client:
        text = await ask_agent(client)
        if text is None:
            log.error("no brief to deliver — exiting non-zero so the timer "
                      "shows the failure in journalctl")
            return 1

        if config.DRY_RUN:
            print(text)
            log.info("dry-run set — skipping notifier post")
            return 0

        ok = await send_to_notifier(client, text)
        return 0 if ok else 2


def main() -> int:
    return asyncio.run(run())


if __name__ == "__main__":
    sys.exit(main())
