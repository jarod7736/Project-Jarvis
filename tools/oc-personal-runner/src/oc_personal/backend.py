"""Shared OpenAI-compat backend client.

Both request paths talk to the same box — Lemonade on amd-halo by default:

  - ``proxy.py`` forwards non-personal chat-completions verbatim.
  - ``agent.py`` drives the tool-calling loop for ``model=oc-personal``.

Keeping one client means the backend-specific quirks (bearer auth, and the
explicit ``/api/v1/load`` retry Lemonade needs for path-registered models)
are implemented once instead of drifting between the two callers.

Provider-agnostic: anything that speaks /v1/chat/completions works
(Lemonade, Ollama, LM Studio, vLLM, llama.cpp server).
"""

from __future__ import annotations

import logging
from typing import Any

import httpx

from . import config

log = logging.getLogger(__name__)


class BackendError(RuntimeError):
    """Backend was unreachable, errored, or returned something unparseable.

    Callers render their own OpenAI-shaped error body — the proxy and the
    agent report failures differently to Jarvis.
    """


class BackendClient:
    def __init__(self) -> None:
        # Optional server-side token auth. Lemonade requires an API key; set
        # OC_BACKEND_TOKEN in the EnvironmentFile. Ollama ignores it.
        headers: dict[str, str] = {}
        if config.BACKEND_TOKEN:
            headers["Authorization"] = f"Bearer {config.BACKEND_TOKEN}"
        # Read timeout covers prefill + generation. The agent path replays up
        # to 8 KB of tool output per turn, so a turn can legitimately take
        # tens of seconds on a 120B model — well above the single-shot
        # passthrough's needs, hence the generous default.
        self._client = httpx.AsyncClient(
            base_url=config.BACKEND_URL,
            timeout=httpx.Timeout(
                connect=3.0, read=config.BACKEND_READ_TIMEOUT, write=30.0, pool=3.0
            ),
            headers=headers,
        )

    async def aclose(self) -> None:
        await self._client.aclose()

    async def chat(self, payload: dict[str, Any]) -> dict[str, Any]:
        """POST a chat-completions payload; return the decoded response.

        Raises BackendError on transport failure, HTTP error, or non-JSON.
        """
        resp = await self._post_chat(payload)

        # Lemonade models registered by local path (rather than pulled from
        # the catalog) can't be auto-loaded by a chat request: the implicit
        # load resolves the name against the Hugging Face API and 404s. An
        # explicit /api/v1/load bypasses that lookup, but doesn't survive a
        # lemond restart — so load on demand here and retry once.
        if _is_model_load_error(resp):
            model = str(payload.get("model", ""))
            log.warning("backend can't auto-load %r; loading explicitly", model)
            if await self.load_model(model):
                resp = await self._post_chat(payload)

        if resp.status_code >= 400:
            log.warning("backend returned %s: %s", resp.status_code, resp.text[:200])
            raise BackendError(f"backend HTTP {resp.status_code}")

        try:
            return resp.json()
        except ValueError as exc:
            log.warning("backend returned non-JSON: %s", resp.text[:200])
            raise BackendError("backend response was not JSON") from exc

    async def _post_chat(self, payload: dict[str, Any]) -> httpx.Response:
        try:
            return await self._client.post("/v1/chat/completions", json=payload)
        except httpx.HTTPError as exc:
            log.warning("backend unreachable: %s", exc)
            raise BackendError(f"backend unreachable: {exc}") from exc

    async def load_model(self, model: str) -> bool:
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
