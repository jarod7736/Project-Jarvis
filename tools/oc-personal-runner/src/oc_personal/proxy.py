"""OpenAI-compat backend passthrough.

Anything Jarvis sends with a model name other than `oc-personal` is
forwarded to whichever OpenAI-compat backend is configured — currently
Lemonade on amd-halo, but any /v1/chat/completions speaker (Ollama,
LM Studio, vLLM, llama.cpp's server, etc.) works without code changes.
If PROXY_FORCE_MODEL is set, the model name is rewritten before
forwarding; otherwise the request goes through verbatim. Lets Jarvis
treat lobsterboy as the only OpenClaw endpoint while a local-LLM box
handles the general-chat routing.

Transport lives in backend.py, shared with the agent path.
"""

from __future__ import annotations

import logging
from typing import Any

from . import config
from .backend import BackendClient, BackendError

log = logging.getLogger(__name__)


class OpenAICompatProxy:
    def __init__(self, backend: BackendClient) -> None:
        self._backend = backend

    async def forward(self, request: dict[str, Any]) -> dict[str, Any]:
        """Forward a chat-completions request and return the backend's response."""
        try:
            return await self._backend.chat(_rewrite(request))
        except BackendError as exc:
            return _proxy_error(str(exc))


def _rewrite(request: dict[str, Any]) -> dict[str, Any]:
    """Apply the pass-through rewrites: pinned model, floored token budget.

    Both exist because the firmware's values are baked in at flash time and
    the backend behind this proxy changes far more often than the device does.
    """
    patched = dict(request)
    if config.PROXY_FORCE_MODEL:
        patched["model"] = config.PROXY_FORCE_MODEL

    floor = config.PROXY_MIN_MAX_TOKENS
    if floor:
        requested = patched.get("max_tokens")
        # bool is an int subclass; a JSON `true` here is malformed input, not
        # a budget.
        valid = isinstance(requested, int) and not isinstance(requested, bool)
        if not valid or requested < floor:
            if valid:
                log.debug("raising max_tokens %s → %s", requested, floor)
            patched["max_tokens"] = floor
    return patched


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
