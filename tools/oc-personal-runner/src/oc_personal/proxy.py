"""LM Studio passthrough.

Anything Jarvis sends with a model name other than `oc-personal` is forwarded
verbatim to LM Studio's /v1/chat/completions. Lets Jarvis treat lobsterboy as
the only OpenClaw endpoint while keeping LM Studio's role unchanged for
gemma / claude routing.
"""

from __future__ import annotations

import logging
from typing import Any

import httpx

from . import config

log = logging.getLogger(__name__)


class LMStudioProxy:
    def __init__(self) -> None:
        # Single shared client. LM Studio supports optional server-side token
        # auth; if config.LMSTUDIO_TOKEN is set we forward it as a Bearer.
        headers: dict[str, str] = {}
        if config.LMSTUDIO_TOKEN:
            headers["Authorization"] = f"Bearer {config.LMSTUDIO_TOKEN}"
        self._client = httpx.AsyncClient(
            base_url=config.LMSTUDIO_URL,
            timeout=httpx.Timeout(connect=3.0, read=30.0, write=30.0, pool=3.0),
            headers=headers,
        )

    async def aclose(self) -> None:
        await self._client.aclose()

    async def forward(self, request: dict[str, Any]) -> dict[str, Any]:
        """Forward a chat-completions request and return LM Studio's response."""
        try:
            resp = await self._client.post("/v1/chat/completions", json=request)
        except httpx.HTTPError as exc:
            log.warning("LM Studio unreachable: %s", exc)
            return _proxy_error(f"LM Studio unreachable: {exc}")

        if resp.status_code >= 400:
            log.warning("LM Studio returned %s: %s", resp.status_code, resp.text[:200])
            return _proxy_error(f"LM Studio HTTP {resp.status_code}")

        try:
            return resp.json()
        except ValueError:
            log.warning("LM Studio returned non-JSON: %s", resp.text[:200])
            return _proxy_error("LM Studio response was not JSON")


def _proxy_error(message: str) -> dict[str, Any]:
    """OpenAI-compat error shape so Jarvis's LLMClient sees a parseable body
    even on upstream failure."""
    return {
        "id": "ocp-proxy-err",
        "object": "chat.completion",
        "model": "proxy-error",
        "choices": [
            {
                "index": 0,
                "message": {"role": "assistant", "content": f"(upstream: {message})"},
                "finish_reason": "stop",
            }
        ],
    }
