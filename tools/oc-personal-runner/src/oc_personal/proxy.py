"""OpenAI-compat backend passthrough.

Anything Jarvis sends with a model name other than `oc-personal` is
forwarded to whichever OpenAI-compat backend is configured — currently
Lemonade on amd-halo, but any /v1/chat/completions speaker (Ollama,
LM Studio, vLLM, llama.cpp's server, etc.) works without code changes.
If PROXY_FORCE_MODEL is set, the model name is rewritten before
forwarding; otherwise the request goes through verbatim. Lets Jarvis
treat lobsterboy as the only OpenClaw endpoint while a local-LLM box
handles the general-chat routing.

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
        if config.PROXY_FORCE_MODEL:
            request = {**request, "model": config.PROXY_FORCE_MODEL}
        try:
            resp = await self._client.post("/v1/chat/completions", json=request)
        except httpx.HTTPError as exc:
            log.warning("backend unreachable: %s", exc)
            return _proxy_error(f"backend unreachable: {exc}")

        # Lemonade models registered by local path (rather than pulled from
        # the catalog) can't be auto-loaded by a chat request: the implicit
        # load resolves the name against the Hugging Face API and 404s. An
        # explicit /api/v1/load bypasses that lookup, but doesn't survive a
        # lemond restart — so load on demand here and retry once.
        if _is_model_load_error(resp):
            model = request.get("model", "")
            log.warning("backend can't auto-load %r; loading explicitly", model)
            if await self._load_model(model):
                try:
                    resp = await self._client.post("/v1/chat/completions", json=request)
                except httpx.HTTPError as exc:
                    log.warning("backend unreachable after load: %s", exc)
                    return _proxy_error(f"backend unreachable: {exc}")

        if resp.status_code >= 400:
            log.warning("backend returned %s: %s", resp.status_code, resp.text[:200])
            return _proxy_error(f"backend HTTP {resp.status_code}")

        try:
            return resp.json()
        except ValueError:
            log.warning("backend returned non-JSON: %s", resp.text[:200])
            return _proxy_error("backend response was not JSON")

    async def _load_model(self, model: str) -> bool:
        """Explicitly load `model` on the backend. Returns True on success.

        Loading a large model is far slower than a chat turn (~36s observed
        for gpt-oss-120b), so this overrides the client's read timeout.
        """
        if not model:
            return False
        try:
            resp = await self._client.post(
                "/api/v1/load",
                json={"model_name": model},
                timeout=httpx.Timeout(connect=3.0, read=300.0, write=30.0, pool=3.0),
            )
        except httpx.HTTPError as exc:
            log.warning("explicit load of %r failed: %s", model, exc)
            return False
        if resp.status_code >= 400:
            log.warning(
                "explicit load of %r returned %s: %s",
                model, resp.status_code, resp.text[:200],
            )
            return False
        log.info("loaded %r on demand", model)
        return True


def _is_model_load_error(resp: httpx.Response) -> bool:
    """True if the backend rejected the request because the model wasn't loaded.

    Lemonade reports this as an `model_load_error` in an OpenAI-shaped error
    body. Any other 4xx/5xx is a real failure and shouldn't trigger a reload.
    """
    if resp.status_code < 400:
        return False
    try:
        error = resp.json().get("error")
    except ValueError:
        return False
    if not isinstance(error, dict):
        return False
    return error.get("code") == "model_load_error"


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
