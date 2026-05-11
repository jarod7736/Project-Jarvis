"""MCP server entry point for the 2nd Brain.

Deployment (lobsterboy):

    sudo mkdir -p /srv && sudo chown $USER /srv
    git clone git@github.com:jarod7736/2ndBrain.git /srv/2ndbrain
    cd /path/to/Project-Jarvis/tools/brain-mcp
    python -m venv .venv
    .venv/bin/pip install -e .

OpenClaw spawns this server as a stdio child of its `oc-personal` agent
loop. Example MCP client config (the exact format depends on OpenClaw's
agent runner; adapt as needed):

    {
      "mcpServers": {
        "brain": {
          "command": "/srv/brain-mcp/.venv/bin/python",
          "args": ["-m", "brain_mcp.server"],
          "env": {
            "BRAIN_VAULT_PATH": "/srv/2ndbrain"
          }
        }
      }
    }

Standalone smoke test (without OpenClaw):

    BRAIN_VAULT_PATH=/srv/2ndbrain BRAIN_PUSH=0 \\
        python -m brain_mcp.server

The server speaks MCP over stdio — for interactive testing use
`mcp dev` from the official Python SDK or the inspector at
https://modelcontextprotocol.io.
"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from .tools import capture, ingest, lint, projects, search


mcp = FastMCP("brain")


@mcp.tool()
def brain_search(query: str, k: int = 8) -> str:
    """Return wiki/ pages most relevant to a query.

    The wiki is markdown maintained by the user via Obsidian. This tool
    scores pages by filename and body keyword overlap and returns raw page
    contents — synthesis is the calling agent's job.

    Args:
        query: Natural-language question or topic (e.g. "kettlebells",
            "what do I know about stoicism").
        k: Max pages to return. Default 8.
    """
    return search.run(query, k=k)


@mcp.tool()
def brain_capture(content: str, source: str = "jarvis") -> str:
    """Save a voice note into the user's 2nd brain ingestion pipeline.

    Writes raw/notes/<ISO8601>-<source>.md with minimal frontmatter so the
    existing /brain-ingest skill picks it up next pass. Commits and pushes
    to GitHub. No LLM, sub-second.

    Args:
        content: The note body. Strip the trigger phrase ("note that",
            "save this to my brain", etc.) before passing — only the
            substantive content should land in the file.
        source: Tag stored in frontmatter. Default "jarvis".
    """
    return capture.run(content, source=source)


@mcp.tool()
def brain_lint() -> str:
    """Audit the wiki for orphans, broken links, index gaps, and missing pages.

    Returns a structured JSON report. Deterministic checks (orphans, broken
    links, index gaps, missing pages) are fully resolved; interpretive
    signals (stale candidates, cross-reference candidates) are returned as
    raw data for the calling agent to summarize prose-style if asked.
    """
    return lint.run()


@mcp.tool()
def brain_list_projects(status: str | None = None) -> str:
    """List wiki pages with ``type: project`` frontmatter.

    Pages opt in by adding ``type: project`` (plus optional ``status``,
    ``next_action``, ``priority``, ``updated``). This tool returns a JSON
    list sorted by status (active → backlog → done → abandoned) and
    priority (high → medium → low), with newest ``updated`` first within
    ties. Missing fields come back as null — callers should treat the
    schema as "loose."

    Args:
        status: Optional filter. One of "active", "backlog", "done",
            "abandoned". When omitted, returns all project pages.
    """
    return projects.list_projects(status=status)


@mcp.tool()
def brain_set_next_action(page: str, action: str) -> str:
    """Set the ``next_action:`` field on a project page; bump ``updated:``.

    Commits and pushes to GitHub. Refuses to edit pages that do not yet
    have ``type: project`` in their frontmatter — creating projects from
    voice is intentionally not supported (avoids ASR-driven proliferation
    of half-baked project pages).

    Args:
        page: Page name. Accepts the stem ("boat"), filename ("boat.md"),
            or a wiki-relative path ("projects/boat"). Case-insensitive
            stem match falls back if the direct path lookup fails.
        action: The imperative phrase to record (e.g. "order the bilge
            pump"). Stored verbatim — strip the trigger phrase before
            calling.
    """
    return projects.set_next_action(page=page, action=action)


@mcp.tool()
def brain_ingest_status() -> str:
    """List raw/ files awaiting ingestion into the wiki.

    Note: full LLM-driven ingest is not yet implemented in this server.
    Use this tool to surface pending files; run /brain-ingest from a
    Claude Code session on the laptop to actually process them.
    """
    return ingest.status()


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
