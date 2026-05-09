"""Configuration. Everything overridable via environment variables.

Set these in the systemd unit's Environment= lines, or export before running
manually for local testing.
"""

from __future__ import annotations

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

# ── brain-mcp invocation ────────────────────────────────────────────────────
# Command + args that spawn the brain-mcp stdio server. Default assumes
# brain-mcp's deploy.sh installed it at the canonical lobsterboy path.
BRAIN_MCP_COMMAND = os.environ.get(
    "OC_BRAIN_MCP_COMMAND",
    "/home/lobsterboy/project-jarvis/tools/brain-mcp/.venv/bin/python",
)
BRAIN_MCP_ARGS = os.environ.get(
    "OC_BRAIN_MCP_ARGS",
    "-m brain_mcp.server",
).split()
# Extra env to pass into brain-mcp. The vault path must match what brain-mcp
# was deployed against.
BRAIN_VAULT_PATH = os.environ.get("BRAIN_VAULT_PATH", "/srv/2ndbrain")


# ── Agent system prompt ─────────────────────────────────────────────────────
# Biases toward terse, voice-friendly replies and steers tool selection so
# "save this" / "note that" goes to brain_capture rather than brain_search.
SYSTEM_PROMPT = """\
You are Jarvis's personal-mode assistant. The user has a personal wiki / 2nd brain
of their own notes, and you have four tools to interact with it:

- brain_search: read top-k wiki pages relevant to a query.
- brain_capture: write a new voice note into the raw/ ingestion pipeline.
- brain_lint: structural audit of the wiki (rarely needed for normal queries).
- brain_ingest_status: list raw/ files awaiting ingestion (rarely needed).

The user is talking to you over voice on a small embedded device. Replies are
spoken aloud. Constraints:

1. Be terse. 1–2 short sentences. No lists, no markdown, no preamble.
2. If the user is asking ABOUT their notes ("what do I know", "have I read",
   etc.) → use brain_search ONCE, synthesize from the returned content, answer.
3. If the user is asking you to SAVE/NOTE/REMEMBER something → call
   brain_capture exactly once with just the substantive content (strip the
   trigger phrase, e.g. "note that I called the plumber" → "called the
   plumber"). Then reply with a 2-3 word confirmation like "Got it." or "Saved."
4. If the wiki has no relevant content, say so plainly. Do not invent facts.
5. Prefer one tool call per turn. Multiple calls only if the first returned
   nothing useful and you can refine the query.
"""
