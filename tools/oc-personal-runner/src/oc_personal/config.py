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
# Where Jarvis used to point oc_host directly. Anything that isn't
# PERSONAL_MODEL gets forwarded here unchanged.
LMSTUDIO_URL = os.environ.get("OC_LMSTUDIO_URL", "http://192.168.1.108:1234")
# Optional Bearer token forwarded as Authorization: Bearer <token>. LM Studio
# added optional server-side token auth in a recent release; if your LM Studio
# requires one, set LM_STUDIO_TOKEN in the EnvironmentFile. Empty string means
# do not send an Authorization header.
LMSTUDIO_TOKEN = os.environ.get("LM_STUDIO_TOKEN", "")

# ── Anthropic side ──────────────────────────────────────────────────────────
ANTHROPIC_API_KEY = os.environ.get("ANTHROPIC_API_KEY", "")
# Default to current Sonnet. Overridable so the user can dial up to Opus
# on lobsterboy without rebuilding.
ANTHROPIC_MODEL = os.environ.get("OC_ANTHROPIC_MODEL", "claude-sonnet-4-7")

# ── Agent loop bounds ───────────────────────────────────────────────────────
# Per PLAN.md Phase 8: cap at 4 inner agent steps. Keeps Jarvis's 10s
# LLMClient timeout meaningful even when the agent decides to call brain_search
# multiple times.
MAX_AGENT_TURNS = int(os.environ.get("OC_MAX_TURNS", "4"))
# Output cap so a chatty Claude reply doesn't push past the device's TTS
# truncation boundary. ~80 tokens ≈ 60 words ≈ a 6-second voice response.
MAX_OUTPUT_TOKENS = int(os.environ.get("OC_MAX_OUTPUT_TOKENS", "200"))

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

1. Be terse. 1–2 short sentences. No lists, no markdown, no preamble.
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
