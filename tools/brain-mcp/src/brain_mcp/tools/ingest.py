"""brain_ingest_status — list raw/ files awaiting ingestion.

This is the v1 surface of brain-ingest. Full LLM-driven ingest (the laptop
skill /brain-ingest's behavior) requires a Claude API key on lobsterboy plus
a per-file prompt loop, and is deferred to v2 — see PLAN.md Phase 8 open
questions.

Until then, the calling agent gets a manifest of pending files and a clear
suggestion to run /brain-ingest from a Claude Code session on the laptop.
That keeps capture working today (raw/notes/ writes flow correctly) while
the agentic ingest pipeline is built out separately.
"""

from __future__ import annotations

import json
from pathlib import Path

from .. import config


_SCAN_DIRS = (
    config.RAW_NOTES_DIR,
    config.RAW_ARTICLES_DIR,
    config.RAW_PDFS_DIR,
)


def status() -> str:
    if not config.VAULT_PATH.exists():
        return json.dumps({
            "error": f"Vault not found at {config.VAULT_PATH}",
        }, indent=2)

    if config.INDEX_MD.exists():
        index_text = config.INDEX_MD.read_text(encoding="utf-8")
    else:
        index_text = ""

    pending: list[str] = []
    for d in _SCAN_DIRS:
        if not d.exists():
            continue
        for p in sorted(d.iterdir()):
            if not p.is_file():
                continue
            if p.name.startswith(".") or p.name.startswith(".readme"):
                continue
            # Heuristic match: skill ingests by filename string. If the name
            # appears anywhere in index.md (raw row, sources table, log.md
            # cross-link), assume already processed.
            if p.name in index_text:
                continue
            pending.append(str(p.relative_to(config.VAULT_PATH)))

    if not pending:
        message = "No new sources in raw/ — nothing to ingest."
    else:
        message = (
            f"{len(pending)} pending source(s) in raw/. "
            f"brain_ingest_run is not yet implemented in this MCP server "
            f"(see Phase 8 open questions). Run /brain-ingest from a Claude "
            f"Code session on the laptop to process these."
        )

    return json.dumps({
        "pending_count": len(pending),
        "pending": pending,
        "message": message,
    }, indent=2)
