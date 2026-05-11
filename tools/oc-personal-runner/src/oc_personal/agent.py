"""Agent loop: Claude (Anthropic SDK) with stdio MCP tools attached.

Manual MCP integration rather than the Claude Agent SDK — keeps the loop
explicit so a maintainer can read it top-to-bottom without learning a new SDK.

Lifecycle:
  - At server startup we spawn one stdio child per entry in
    ``config.MCP_SERVERS`` (typically brain-mcp and google-mcp), initialize
    each session, list each one's tools, and merge them into a single
    Anthropic-tool list. A name→session map routes subsequent tool calls
    back to the right child.
  - For each /v1/chat/completions request with model=oc-personal:
      1. Call Claude with the user's transcript + the merged tool definitions.
      2. If response.stop_reason == "tool_use": dispatch each tool_use block
         to the owning session via call_tool, append tool_result, repeat.
         Cap at MAX_AGENT_TURNS to bound latency.
      3. Return the final assistant text.

Concurrency note: MCP sessions are shared across requests. The tools we ship
are short-running and stateless w.r.t. each other (capture writes unique
filenames; search/list/draft/create are read-or-append, no transactional
overlap). A lock serializes calls anyway since Jarvis produces one voice
query at a time.
"""

from __future__ import annotations

import asyncio
import logging
import shlex
from contextlib import AsyncExitStack, asynccontextmanager
from typing import Any

from anthropic import AsyncAnthropic
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from . import config

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

    def __init__(self) -> None:
        self._anthropic = AsyncAnthropic(api_key=config.ANTHROPIC_API_KEY)
        self._sessions: dict[str, ClientSession] = {}
        # tool name → session that owns it. Built from list_tools() of each
        # child at startup; tools_anthropic is the merged Anthropic-shape
        # tool list passed to messages.create().
        self._tool_to_session: dict[str, ClientSession] = {}
        self._tools_anthropic: list[dict[str, Any]] = []
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
                    self._tools_anthropic.append({
                        "name": t.name,
                        "description": t.description or "",
                        "input_schema": t.inputSchema,
                    })
                    tool_names_here.append(t.name)
                log.info(
                    "mcp session %s initialized with %d tools: %s",
                    srv_name, len(tool_names_here), tool_names_here,
                )

            log.info(
                "agent ready: %d MCP servers, %d total tools",
                len(self._sessions), len(self._tools_anthropic),
            )
            try:
                yield
            finally:
                self._sessions.clear()
                self._tool_to_session.clear()
                self._tools_anthropic.clear()

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
            messages: list[dict[str, Any]] = [
                {"role": "user", "content": user_text},
            ]

            final_text = ""
            for turn in range(config.MAX_AGENT_TURNS):
                resp = await self._anthropic.messages.create(
                    model=config.ANTHROPIC_MODEL,
                    system=config.SYSTEM_PROMPT,
                    max_tokens=config.MAX_OUTPUT_TOKENS,
                    tools=self._tools_anthropic,
                    messages=messages,
                )

                # Capture any text Claude emitted in this turn — we'll keep
                # the last non-empty value as the final reply.
                turn_text = "".join(
                    getattr(b, "text", "") for b in resp.content if b.type == "text"
                )
                if turn_text.strip():
                    final_text = turn_text.strip()

                if resp.stop_reason != "tool_use":
                    break

                # Append assistant turn (with tool_use blocks) verbatim.
                messages.append({"role": "assistant", "content": resp.content})

                # Dispatch each tool_use block in order.
                tool_results = []
                for block in resp.content:
                    if block.type != "tool_use":
                        continue
                    log.info("agent calling tool %s with %s", block.name, block.input)
                    try:
                        out = await self._call_tool(block.name, dict(block.input))
                    except Exception as exc:
                        log.exception("tool call failed")
                        out = f"Tool {block.name} raised: {exc}"
                    tool_results.append({
                        "type": "tool_result",
                        "tool_use_id": block.id,
                        "content": out[:8000],
                    })
                messages.append({"role": "user", "content": tool_results})
            else:
                # Hit MAX_AGENT_TURNS without stop_reason changing.
                log.warning(
                    "agent loop hit %d-turn cap without natural termination",
                    config.MAX_AGENT_TURNS,
                )
                if not final_text:
                    final_text = "I got distracted searching. Try asking again."

            return _openai_chat_response(final_text or "(no reply)")


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
