"""Configuration. Everything overridable via environment variables.

Set these in the systemd unit's Environment= lines, or export before running
manually for local testing.
"""

from __future__ import annotations

import json
import os

# ── HTTP listener ───────────────────────────────────────────────────────────
LISTEN_HOST = os.environ.get("OC_LISTEN_HOST", "0.0.0.0")
LISTEN_PORT = int(os.environ.get("OC_LISTEN_PORT", "8080"))

# ── Model alias for the agentic personal-mode path ──────────────────────────
# Must match firmware src/config.h:kOcPersonalModel exactly.
PERSONAL_MODEL = os.environ.get("OC_PERSONAL_MODEL", "oc-personal")

# ── Pass-through target for non-personal models ─────────────────────────────
# Any chat-completions request whose model != PERSONAL_MODEL gets forwarded
# here (model name optionally rewritten, see PROXY_FORCE_MODEL below).
# Provider-agnostic: works against any OpenAI-compat backend (Lemonade,
# Ollama, LM Studio, vLLM, llama.cpp server). Default is Lemonade on the
# amd-halo box, reachable over the tailnet.
BACKEND_URL = os.environ.get("OC_BACKEND_URL", "http://amd-halo:13305")
# Optional Bearer token. Lemonade requires an API key; set
# OC_BACKEND_TOKEN in the EnvironmentFile (/etc/oc-personal/secrets.env).
# Ollama ignores it. Empty string means do not send an Authorization
# header.
BACKEND_TOKEN = os.environ.get("OC_BACKEND_TOKEN", "")

# ── Proxy model override ────────────────────────────────────────────────────
# If set, the proxy path rewrites the incoming model name to this value
# before forwarding to BACKEND_URL. Lets us pin lobsterboy's pass-through
# to a known-good backend model regardless of what the client requests —
# the firmware still sends its baked-in kOcLocalModel name.
# Empty string = forward whatever the client sent (legacy behavior).
# `coder` is a Lemonade tool-calling alias (verified ~4s). Do NOT use
# `chat` — it does not exist on Lemonade and every passthrough 404s
# (fixed 2026-08-30).
PROXY_FORCE_MODEL = os.environ.get("OC_PROXY_FORCE_MODEL", "coder")

# Floor for the forwarded token budget. The firmware hard-codes max_tokens=80
# (src/config.h:kOcMaxTokens) and can only be changed by a reflash. Local
# reasoning models spend their budget on a `reasoning_content` channel first
# and emit visible `content` only after it closes — `chat` burned 283
# completion tokens answering "why is the sky blue" in one sentence (measured
# 2026-07-29). At 80 tokens the reply comes back as an empty string and the
# device speaks nothing, so raise anything below this floor. The device only
# ever reads `content`, so the extra budget buys reasoning the user never
# hears. 0 disables the floor.
PROXY_MIN_MAX_TOKENS = int(os.environ.get("OC_PROXY_MIN_MAX_TOKENS", "512"))

# ── Agent model ─────────────────────────────────────────────────────────────
# The agentic personal path runs on the same OpenAI-compat backend as the
# passthrough — nothing here calls api.anthropic.com. `coder` is Lemonade's
# tool-calling alias; it is the right pick over `chat` because the agent's job
# is tool routing against a 10-tool schema, and `chat` burns an order of
# magnitude more completion tokens on reasoning to reach the same call.
AGENT_MODEL = os.environ.get("OC_AGENT_MODEL", "coder")

# How long to wait on a backend reply. The agent replays up to 8 KB of tool
# output per turn, so prefill alone can run tens of seconds on a large local
# model — the old 30s httpx default was marginal.
BACKEND_READ_TIMEOUT = float(os.environ.get("OC_BACKEND_READ_TIMEOUT", "60"))

# ── Agent loop bounds ───────────────────────────────────────────────────────
# Per PLAN.md Phase 8: cap at 4 inner agent steps. Keeps Jarvis's 10s
# LLMClient timeout meaningful even when the agent decides to call brain_search
# multiple times.
MAX_AGENT_TURNS = int(os.environ.get("OC_MAX_TURNS", "4"))
# Per-turn completion budget. This is NOT the spoken-reply length: local
# reasoning models spend most of it on a `reasoning_content` channel the agent
# drops before TTS (299 completion tokens for a single tool call, measured on
# `chat` 2026-07-29). Sized so a tool call can't be truncated mid-JSON; reply
# brevity is enforced by SYSTEM_PROMPT, not by this cap.
AGENT_MAX_TOKENS = int(os.environ.get("OC_AGENT_MAX_TOKENS", "1024"))
# Bound on the *spoken* reply, which AGENT_MAX_TOKENS can no longer provide:
# that budget has to stay large enough for a tool call to survive, so reply
# length needs its own limit. 60 words ≈ the 6-second utterance the old
# 200-token cap targeted. Trimming happens on a sentence boundary, so unlike
# the old token cap this can't stop TTS mid-word. 0 disables trimming.
REPLY_MAX_WORDS = int(os.environ.get("OC_REPLY_MAX_WORDS", "60"))

# ── MCP server invocations ──────────────────────────────────────────────────
# The agent spawns one stdio child per entry in MCP_SERVERS. Each entry is
#
#     "name": {
#         "command": "/abs/path/to/python",   # interpreter / binary
#         "args":    ["-m", "brain_mcp.server"],  # argv tail
#         "env":     {"BRAIN_VAULT_PATH": "/srv/2ndbrain"},  # spawn env (merged
#                                                            # with parent)
#         "stderr":  "/tmp/<name>.err",       # optional; if set, child stderr
#                                              # is redirected here via a sh -c
#                                              # wrapper (MCP stdio_client
#                                              # otherwise discards it).
#     }
#
# Configurable via the OC_MCP_SERVERS env var (JSON object). The default
# below preserves the original single-brain-mcp behavior so existing
# deploys that have not migrated still work.
#
# Falls back to the legacy OC_BRAIN_MCP_{COMMAND,ARGS} + BRAIN_VAULT_PATH
# vars if OC_MCP_SERVERS is unset — useful while transitioning the
# systemd unit. Removable once all deploys are on the JSON form.
_LEGACY_BRAIN_MCP_COMMAND = os.environ.get(
    "OC_BRAIN_MCP_COMMAND",
    "/home/lobsterboy/project-jarvis/tools/brain-mcp/.venv/bin/python",
)
_LEGACY_BRAIN_MCP_ARGS = os.environ.get(
    "OC_BRAIN_MCP_ARGS",
    "-m brain_mcp.server",
).split()
BRAIN_VAULT_PATH = os.environ.get("BRAIN_VAULT_PATH", "/srv/2ndbrain")


def _default_mcp_servers() -> dict[str, dict[str, object]]:
    return {
        "brain": {
            "command": _LEGACY_BRAIN_MCP_COMMAND,
            "args": _LEGACY_BRAIN_MCP_ARGS,
            "env": {"BRAIN_VAULT_PATH": BRAIN_VAULT_PATH},
            "stderr": "/tmp/brain-mcp.err",
        },
    }


def _parse_mcp_servers() -> dict[str, dict[str, object]]:
    raw = os.environ.get("OC_MCP_SERVERS", "").strip()
    if not raw:
        return _default_mcp_servers()
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"OC_MCP_SERVERS is not valid JSON: {exc}") from exc
    if not isinstance(parsed, dict) or not parsed:
        raise RuntimeError("OC_MCP_SERVERS must be a non-empty JSON object")
    for name, spec in parsed.items():
        if not isinstance(spec, dict):
            raise RuntimeError(f"OC_MCP_SERVERS[{name!r}] must be an object")
        if "command" not in spec:
            raise RuntimeError(f"OC_MCP_SERVERS[{name!r}] missing 'command'")
        spec.setdefault("args", [])
        spec.setdefault("env", {})
        if not isinstance(spec["args"], list):
            raise RuntimeError(f"OC_MCP_SERVERS[{name!r}].args must be a list")
        if not isinstance(spec["env"], dict):
            raise RuntimeError(f"OC_MCP_SERVERS[{name!r}].env must be an object")
    return parsed


MCP_SERVERS = _parse_mcp_servers()


# ── Agent system prompt ─────────────────────────────────────────────────────
# Biases toward terse, voice-friendly replies and steers tool selection. The
# routing rules below matter because most user utterances are ambiguous between
# "tell me what's there" (brain_search), "what's my plan" (brain_list_projects),
# "the plan is X" (brain_set_next_action), and "save this thought"
# (brain_capture) — and the wrong choice produces either junk wiki pages or
# missed updates.
SYSTEM_PROMPT = """\
You are Jarvis's personal-mode assistant. The user has a personal wiki / 2nd brain
and a personal Google account (Calendar + Gmail). You have ten tools:

WIKI (2nd brain):
- brain_search: read top-k wiki pages relevant to a query.
- brain_capture: write a new voice note into the raw/ ingestion pipeline.
- brain_list_projects: list pages tagged `type: project`, optionally filtered
  by status (active|backlog|done|abandoned). Returns next_action and priority.
- brain_set_next_action: set the `next_action` field on a named project page.
- brain_lint: structural audit of the wiki (rarely needed for normal queries).
- brain_ingest_status: list raw/ files awaiting ingestion (rarely needed).

CALENDAR (personal Google Calendar):
- gcal_list_events: events in a time window. Default window is now → +24h.
- gcal_create_event: create an event. ISO 8601 start/end. No attendee invites.

EMAIL (personal Gmail):
- gmail_list_unread: subjects/from/snippet for unread inbox messages.
- gmail_search: full Gmail query syntax (from:, subject:, has:attachment, etc.).
- gmail_get_thread: full message bodies for a thread_id.
- gmail_create_draft: compose a draft. NEVER sends. User reviews in Gmail.

The user is talking to you over voice on a small embedded device. Replies are
spoken aloud. Constraints:

1. Be terse. 1–2 short sentences, under 60 words total. No lists, no markdown,
   no preamble, no sign-off, and no "would you like me to..." follow-up offers.
   If a tool returns many items, speak only the most important one or two and
   stop.
2. WIKI READ: if the user is asking ABOUT their notes ("what do I know", "have
   I read", etc.) → use brain_search ONCE, synthesize, answer.
3. PROJECT STATUS: if asking about a project's status or NEXT STEP ("what's
   next on the boat", "what am I working on", "what's active") → use
   brain_list_projects (filter status="active" for "what am I working on") or
   brain_search for the specific project. Read the next_action and speak it.
4. PROJECT UPDATE: if the user states what the NEXT STEP on a project is
   ("next step on the boat is order the bilge pump") → call
   brain_set_next_action(page=<project>, action=<imperative>) ONCE. Strip
   trigger phrase. Reply with 2-3 word confirmation.
5. FREE-FORM NOTE: if asking you to SAVE/NOTE/REMEMBER a standalone thought
   (NOT a next-step update) → brain_capture ONCE with the substantive content.
   "Got it." or "Saved."
6. TIEBREAKER (note vs update): if the utterance names a specific project AND
   an imperative action, it's set_next_action. If it's a standalone thought,
   it's capture. When unsure, ask one short clarifying question rather than
   guessing wrong.
7. CALENDAR READ: "what's on my calendar", "what's today/tomorrow", "do I have
   anything at <time>" → gcal_list_events. Default window is fine for "today"
   and "next few hours". For specific days, set time_min/time_max in ISO 8601.
8. CALENDAR WRITE: "add X to my calendar", "schedule X for <time>" → ask for
   any missing required field (title, start, end) in one short clarifier,
   then gcal_create_event ONCE. Confirm with the event summary and time.
9. EMAIL READ: "what's in my inbox", "any email from X", "did Y reply" →
   gmail_list_unread for inbox triage, gmail_search for targeted lookups,
   gmail_get_thread to read a specific conversation.
10. EMAIL WRITE: "draft a reply to X saying Y", "write Z an email about W" →
    gmail_create_draft ONCE. NEVER send. Always tell the user "Drafted; review
    in Gmail before sending." Refuse if asked to send directly — explain that
    the device-side flow is draft-only by design.
11. WIKI vs CALENDAR/EMAIL routing: if the user asks "what's on my calendar"
    use gcal_list_events, NOT brain_search. If asking "what did I write about
    X" use brain_search, NOT gmail_search. Use email tools only when the
    request explicitly names email/inbox/Gmail or a sender.
12. If a tool returns "Auth error" → tell the user plainly that the relevant
    integration isn't set up. Do not invent data.
13. If the wiki/inbox/calendar has no relevant content, say so plainly. Do not
    invent facts.
14. Prefer one tool call per turn. Multiple calls only if the first returned
    nothing useful and you can refine.
"""
