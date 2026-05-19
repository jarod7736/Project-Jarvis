"""OpenAI-compat backend passthrough.

Anything Jarvis sends with a model name other than `oc-personal` is
forwarded verbatim to whichever OpenAI-compat backend is configured —
currently Ollama, but any /v1/chat/completions speaker (LM Studio, vLLM,
llama.cpp's server, etc.) works without code changes. Lets Jarvis treat
lobsterboy as the only OpenClaw endpoint while a local-LLM box handles
the gemma / general-chat routing.

Provider-agnostic naming so a future swap is one env-var change
(`OC_BACKEND_URL`), not a code rename.
"""

from __future__ import annotations

import logging
from typing import Any

import httpx

from . import config

log = logging.getLogger(__name__)


class OpenAICompatProxy:
    def __init__(self) -> None:
        # Single shared client. Some backends (LM Studio, vLLM) support
        # optional server-side token auth; if config.BACKEND_TOKEN is set
        # we forward it as a Bearer. Ollama ignores it by default.
        headers: dict[str, str] = {}
        if config.BACKEND_TOKEN:
            headers["Authorization"] = f"Bearer {config.BACKEND_TOKEN}"
        self._client = httpx.AsyncClient(
            base_url=config.BACKEND_URL,
            timeout=httpx.Timeout(connect=3.0, read=30.0, write=30.0, pool=3.0),
            headers=headers,
        )

    async def aclose(self) -> None:
        await self._client.aclose()

    async def forward(self, request: dict[str, Any]) -> dict[str, Any]:
        """Forward a chat-completions request and return the backend's response."""
        try:
            resp = await self._client.post("/v1/chat/completions", json=request)
        except httpx.HTTPError as exc:
            log.warning("backend unreachable: %s", exc)
            return _proxy_error(f"backend unreachable: {exc}")

        if resp.status_code >= 400:
            log.warning("backend returned %s: %s", resp.status_code, resp.text[:200])
            return _proxy_error(f"backend HTTP {resp.status_code}")

        try:
            return resp.json()
        except ValueError:
            log.warning("backend returned non-JSON: %s", resp.text[:200])
            return _proxy_error("backend response was not JSON")


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
