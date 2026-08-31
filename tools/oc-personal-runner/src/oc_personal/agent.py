"""Agent loop: an OpenAI-compat chat model with stdio MCP tools attached.

Runs against the same backend as the passthrough path — Lemonade on amd-halo
— so no request from this box reaches api.anthropic.com. Manual MCP
integration rather than an agent SDK: the loop stays explicit enough that a
maintainer can read it top-to-bottom.

Lifecycle:
  - At server startup we spawn one stdio child per entry in
    ``config.MCP_SERVERS`` (typically brain-mcp and google-mcp), initialize
    each session, list each one's tools, and merge them into a single
    OpenAI-shape function list. A name→session map routes subsequent tool
    calls back to the right child.
  - For each /v1/chat/completions request with model=oc-personal:
      1. Call the backend with the user's transcript + the merged tool defs.
      2. If the reply carries `tool_calls`: dispatch each one to the owning
         session via call_tool, append a `role: tool` message per call,
         repeat. Cap at MAX_AGENT_TURNS to bound latency.
      3. Return the final assistant text.

Concurrency note: MCP sessions are shared across requests. The tools we ship
are short-running and stateless w.r.t. each other (capture writes unique
filenames; search/list/draft/create are read-or-append, no transactional
overlap). A lock serializes calls anyway since Jarvis produces one voice
query at a time.
"""

from __future__ import annotations

import asyncio
import json
import logging
import re
import shlex
from contextlib import AsyncExitStack, asynccontextmanager
from datetime import datetime
from typing import Any
from zoneinfo import ZoneInfo

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from . import config
from .backend import BackendClient, BackendError

log = logging.getLogger(__name__)


def _build_stdio_params(name: str, spec: dict[str, object]) -> StdioServerParameters:
    """Render an MCP_SERVERS spec into StdioServerParameters.

    If ``spec["stderr"]`` is a path, wrap the command via
    ``/bin/sh -c "exec ... 2>>PATH"`` so the child's stderr lands somewhere
    grep-able. Without this, the MCP stdio_client silently discards
    everything the child writes to stderr — which makes "Connection closed"
    errors essentially undebuggable.
    """
    command = str(spec["command"])
    args = [str(a) for a in spec.get("args", [])]
    env = {str(k): str(v) for k, v in (spec.get("env") or {}).items()}
    stderr_path = spec.get("stderr")
    if stderr_path:
        wrapped = "exec {cmd} {args} 2>>{err}".format(
            cmd=shlex.quote(command),
            args=" ".join(shlex.quote(a) for a in args),
            err=shlex.quote(str(stderr_path)),
        )
        return StdioServerParameters(command="/bin/sh", args=["-c", wrapped], env=env)
    return StdioServerParameters(command=command, args=args, env=env)


class BrainAgent:
    """Holds long-lived MCP sessions and runs request-scoped agent loops."""

    def __init__(self, backend: BackendClient) -> None:
        self._backend = backend
        self._sessions: dict[str, ClientSession] = {}
        # tool name → session that owns it. Built from list_tools() of each
        # child at startup; tools_openai is the merged function list passed
        # to the backend on every turn.
        self._tool_to_session: dict[str, ClientSession] = {}
        self._tools_openai: list[dict[str, Any]] = []
        self._lock = asyncio.Lock()

    @asynccontextmanager
    async def lifecycle(self):
        # mcp's stdio_client wraps an anyio TaskGroup whose cancel scope is
        # bound to the entering task; nested `async with` blocks must remain
        # in the live call frame of the FastAPI lifespan task (manual
        # __aenter__/__aexit__ across separate methods produces "Attempted
        # to exit cancel scope in a different task" on startup).
        #
        # AsyncExitStack lets us spawn N children with the same per-task
        # semantics as a single nested `async with` chain.
        async with AsyncExitStack() as stack:
            for srv_name, spec in config.MCP_SERVERS.items():
                params = _build_stdio_params(srv_name, spec)
                read, write = await stack.enter_async_context(stdio_client(params))
                session = await stack.enter_async_context(ClientSession(read, write))
                await session.initialize()
                tools_resp = await session.list_tools()
                self._sessions[srv_name] = session
                tool_names_here: list[str] = []
                for t in tools_resp.tools:
                    if t.name in self._tool_to_session:
                        # Two servers exposing the same tool name is a config
                        # bug — silently shadowing would route to the wrong
                        # backend at random.
                        owner = next(
                            (n for n, s in self._sessions.items()
                             if s is self._tool_to_session[t.name]),
                            "<unknown>",
                        )
                        log.error(
                            "duplicate tool name %r from %s shadows %s — ignoring later",
                            t.name, srv_name, owner,
                        )
                        continue
                    self._tool_to_session[t.name] = session
                    self._tools_openai.append({
                        "type": "function",
                        "function": {
                            "name": t.name,
                            "description": t.description or "",
                            "parameters": t.inputSchema,
                        },
                    })
                    tool_names_here.append(t.name)
                log.info(
                    "mcp session %s initialized with %d tools: %s",
                    srv_name, len(tool_names_here), tool_names_here,
                )

            log.info(
                "agent ready: %d MCP servers, %d total tools, model=%s",
                len(self._sessions), len(self._tools_openai), config.AGENT_MODEL,
            )
            try:
                yield
            finally:
                self._sessions.clear()
                self._tool_to_session.clear()
                self._tools_openai.clear()

    @property
    def tool_names(self) -> list[str]:
        return [t["function"]["name"] for t in self._tools_openai]

    @property
    def ready(self) -> bool:
        return bool(self._sessions)

    @property
    def server_names(self) -> list[str]:
        return sorted(self._sessions)

    async def _call_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Call an MCP tool on its owning session; flatten the result."""
        session = self._tool_to_session.get(name)
        if session is None:
            return f"Tool {name!r} is not registered with any MCP server."
        result = await session.call_tool(name, arguments)
        # MCP tool results come back as a list of content blocks; for our
        # tools they're all text. Concatenate; preserve ordering.
        parts: list[str] = []
        for block in result.content:
            text = getattr(block, "text", None)
            if text is not None:
                parts.append(text)
        return "\n".join(parts) if parts else ""

    async def handle(self, request: dict[str, Any]) -> dict[str, Any]:
        """Run an agent loop for a chat-completions request and return the
        OpenAI-compat response shape Jarvis's LLMClient expects."""
        user_text = _extract_last_user_message(request)
        if user_text is None:
            return _openai_error("no user message in request")

        async with self._lock:
            # The model has no clock; without this prefix it invents a date
            # near its training cutoff and asks the calendar tool about the
            # wrong day. America/Chicago matches the primary Google Calendar tz.
            now = datetime.now(ZoneInfo("America/Chicago"))
            date_prefix = (
                f"Current date: {now.strftime('%A %Y-%m-%d')}. "
                f"Current time: {now.strftime('%H:%M %Z')}. "
                f"When the user says \"today\" / \"tomorrow\" / \"this week\", "
                f"resolve against this date.\n\n"
            )
            messages: list[dict[str, Any]] = [
                {"role": "system", "content": date_prefix + config.SYSTEM_PROMPT},
                {"role": "user", "content": user_text},
            ]

            final_text = ""
            for _turn in range(config.MAX_AGENT_TURNS):
                try:
                    resp = await self._backend.chat({
                        "model": config.AGENT_MODEL,
                        "messages": messages,
                        "tools": self._tools_openai,
                        "max_tokens": config.AGENT_MAX_TOKENS,
                        "stream": False,
                    })
                except BackendError as exc:
                    log.warning("agent backend call failed: %s", exc)
                    return _openai_error(str(exc))

                message = _first_message(resp)
                # `content` is the spoken channel. Reasoning models also emit
                # `reasoning_content`, which we deliberately drop — it must
                # never reach TTS, and replaying it wastes context.
                turn_text = (message.get("content") or "").strip()
                if turn_text:
                    final_text = turn_text

                tool_calls = message.get("tool_calls") or []
                if not tool_calls:
                    break

                # Echo the assistant turn back so the model sees its own call,
                # normalized to the two fields the backend needs on replay.
                messages.append({
                    "role": "assistant",
                    "content": message.get("content") or "",
                    "tool_calls": tool_calls,
                })

                # Dispatch each call in order; one `role: tool` reply per call.
                for call in tool_calls:
                    fn = call.get("function") or {}
                    name = str(fn.get("name") or "")
                    arguments, err = _parse_tool_arguments(fn.get("arguments"))
                    if err is not None:
                        log.warning("bad tool arguments for %s: %s", name, err)
                        out = f"Tool {name} arguments were not valid JSON: {err}"
                    else:
                        log.info("agent calling tool %s with %s", name, arguments)
                        try:
                            out = await self._call_tool(name, arguments)
                        except Exception as exc:
                            log.exception("tool call failed")
                            out = f"Tool {name} raised: {exc}"
                    messages.append({
                        "role": "tool",
                        "tool_call_id": str(call.get("id") or ""),
                        "content": out[:8000],
                    })
            else:
                # Hit MAX_AGENT_TURNS with the model still asking for tools.
                log.warning(
                    "agent loop hit %d-turn cap without natural termination",
                    config.MAX_AGENT_TURNS,
                )
                if not final_text:
                    final_text = "I got distracted searching. Try asking again."

            reply = _clamp_reply(final_text or "(no reply)", config.REPLY_MAX_WORDS)
            return _openai_chat_response(reply)


_SENTENCE_END = re.compile(r"(?<=[.!?])\s+")


def _clamp_reply(text: str, max_words: int) -> str:
    """Trim a spoken reply to roughly `max_words`, cutting between sentences.

    Reply length used to be bounded by the request's max_tokens. That knob
    can't do it any more: local reasoning models spend most of their budget on
    a hidden channel, so the budget must stay large enough for a tool call to
    survive. Cutting on a sentence boundary means TTS never stops mid-word —
    strictly gentler than the token cap it replaces.
    """
    if max_words <= 0 or len(text.split()) <= max_words:
        return text

    kept: list[str] = []
    used = 0
    for sentence in _SENTENCE_END.split(text):
        n = len(sentence.split())
        if kept and used + n > max_words:
            break
        kept.append(sentence)
        used += n
        if used >= max_words:
            break

    clamped = " ".join(kept).strip()
    log.info("trimmed reply from %d to %d words", len(text.split()), len(clamped.split()))
    # A single sentence longer than the budget is kept whole rather than cut
    # mid-word — better a slightly long utterance than a severed one.
    return clamped


def _first_message(resp: dict[str, Any]) -> dict[str, Any]:
    """Pull choices[0].message out of a chat-completions body, defensively.

    A backend that returns a malformed body should degrade to "(no reply)"
    rather than raising inside the request handler.
    """
    choices = resp.get("choices")
    if not isinstance(choices, list) or not choices:
        log.warning("backend response had no choices: %s", str(resp)[:200])
        return {}
    message = choices[0].get("message") if isinstance(choices[0], dict) else None
    return message if isinstance(message, dict) else {}


def _parse_tool_arguments(raw: Any) -> tuple[dict[str, Any], str | None]:
    """Normalize a tool call's `arguments` into a dict.

    OpenAI-compat backends send a JSON *string*; a few send the object
    directly. Returns (arguments, error_message).
    """
    if raw is None or raw == "":
        return {}, None
    if isinstance(raw, dict):
        return raw, None
    if isinstance(raw, str):
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError as exc:
            return {}, str(exc)
        if not isinstance(parsed, dict):
            return {}, f"expected an object, got {type(parsed).__name__}"
        return parsed, None
    return {}, f"unsupported arguments type {type(raw).__name__}"


def _extract_last_user_message(request: dict[str, Any]) -> str | None:
    msgs = request.get("messages") or []
    for m in reversed(msgs):
        if m.get("role") == "user":
            content = m.get("content")
            if isinstance(content, str):
                return content
            # OpenAI also allows list-of-content-blocks; flatten text parts.
            if isinstance(content, list):
                texts = [b.get("text", "") for b in content if isinstance(b, dict)]
                joined = "".join(texts).strip()
                if joined:
                    return joined
    return None


def _openai_chat_response(text: str) -> dict[str, Any]:
    """Minimal OpenAI-compat chat-completions response. Fields Jarvis's
    LLMClient::query reads: choices[0].message.content. Everything else is
    nice-to-have for compatibility with future tooling."""
    return {
        "id": "ocp-0",
        "object": "chat.completion",
        "model": config.PERSONAL_MODEL,
        "choices": [
            {
                "index": 0,
                "message": {"role": "assistant", "content": text},
                "finish_reason": "stop",
            }
        ],
        "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
    }


def _openai_error(message: str) -> dict[str, Any]:
    return _openai_chat_response(f"(error: {message})")
