"""Agent loop: Claude (Anthropic SDK) with brain-mcp tools attached.

Manual MCP integration rather than the Claude Agent SDK — keeps the loop
explicit so a maintainer can read it top-to-bottom without learning a new SDK.

Lifecycle:
  - At server startup we spawn brain-mcp as a stdio child via mcp.client.stdio,
    initialize the MCP session, and list its tools once.
  - For each /v1/chat/completions request with model=oc-personal:
      1. Convert the MCP tool list to Anthropic's tool schema.
      2. Call Claude with the user's transcript + tool definitions.
      3. If response.stop_reason == "tool_use": dispatch each tool_use block
         to brain-mcp via session.call_tool, append tool_result, repeat.
         Cap at MAX_AGENT_TURNS to bound latency.
      4. Return the final assistant text.

Concurrency note: a single MCP session is shared across requests. brain-mcp
tools are short-running and stateless w.r.t. each other (capture writes
unique filenames, search/lint/ingest_status are read-only), so serialization
is fine for the call volume Jarvis generates (one voice query at a time).
A lock guards the session in case of concurrent HTTP requests anyway.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from anthropic import AsyncAnthropic
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from . import config

log = logging.getLogger(__name__)


class BrainAgent:
    """Holds the long-lived MCP session and runs request-scoped agent loops."""

    def __init__(self) -> None:
        self._anthropic = AsyncAnthropic(api_key=config.ANTHROPIC_API_KEY)
        self._session: ClientSession | None = None
        self._tools_anthropic: list[dict[str, Any]] = []
        self._mcp_ctx: Any = None
        self._client_ctx: Any = None
        self._lock = asyncio.Lock()

    async def start(self) -> None:
        """Spawn brain-mcp and cache its tool definitions."""
        params = StdioServerParameters(
            command=config.BRAIN_MCP_COMMAND,
            args=config.BRAIN_MCP_ARGS,
            env={"BRAIN_VAULT_PATH": config.BRAIN_VAULT_PATH},
        )
        # Hold the context managers open for the lifetime of the agent;
        # close them in stop().
        self._client_ctx = stdio_client(params)
        read, write = await self._client_ctx.__aenter__()
        self._mcp_ctx = ClientSession(read, write)
        self._session = await self._mcp_ctx.__aenter__()
        await self._session.initialize()

        tools_resp = await self._session.list_tools()
        self._tools_anthropic = [
            {
                "name": t.name,
                "description": t.description or "",
                "input_schema": t.inputSchema,
            }
            for t in tools_resp.tools
        ]
        log.info(
            "brain-mcp session initialized with %d tools: %s",
            len(self._tools_anthropic),
            [t["name"] for t in self._tools_anthropic],
        )

    async def stop(self) -> None:
        # Best-effort close. Order matters — session before stdio transport.
        if self._mcp_ctx is not None:
            try:
                await self._mcp_ctx.__aexit__(None, None, None)
            except Exception:
                log.exception("error closing MCP session")
        if self._client_ctx is not None:
            try:
                await self._client_ctx.__aexit__(None, None, None)
            except Exception:
                log.exception("error closing MCP transport")

    async def _call_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Call an MCP tool and flatten the result to a single string."""
        assert self._session is not None
        result = await self._session.call_tool(name, arguments)
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
