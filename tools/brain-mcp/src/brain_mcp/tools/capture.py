"""brain_capture — write a voice note into raw/notes/.

Per Phase 8: timestamped filename, minimal frontmatter matching the existing
ingest skill's expectations, commit + push so the laptop's Obsidian Git pull
picks it up. No LLM, sub-second.
"""

from __future__ import annotations

import re
from collections import OrderedDict
from datetime import datetime, timezone
from pathlib import Path

from .. import config, frontmatter, vault


def _sanitize_source(source: str) -> str:
    s = re.sub(r"[^a-z0-9-]+", "-", source.lower()).strip("-")
    return s or "jarvis"


def _next_available_path(base: Path, suffix: str) -> Path:
    candidate = base.with_name(f"{base.stem}-{suffix}{base.suffix}")
    n = 2
    while candidate.exists():
        candidate = base.with_name(f"{base.stem}-{suffix}-{n}{base.suffix}")
        n += 1
    return candidate


def run(content: str, source: str = "jarvis") -> str:
    text = content.strip()
    if not text:
        return "Error: content is empty — nothing to capture."

    safe_source = _sanitize_source(source)

    pull_warning = ""
    if config.PULL_BEFORE_CAPTURE:
        try:
            vault.pull_ff_only()
        except vault.GitError as exc:
            pull_warning = f" (pull warning: {exc})"

    config.RAW_NOTES_DIR.mkdir(parents=True, exist_ok=True)

    now = datetime.now(timezone.utc)
    base_name = now.strftime("%Y-%m-%dT%H-%M-%S")
    path = config.RAW_NOTES_DIR / f"{base_name}-{safe_source}.md"
    if path.exists():
        path = _next_available_path(path, suffix="2")

    iso_z = now.strftime("%Y-%m-%dT%H:%M:%SZ")
    fm = OrderedDict([
        ("source", safe_source),
        ("captured_at", iso_z),
        # Auto-bump `updated:` on every write. `captured_at` is set-once
        # creation time; `updated` moves with each edit (and brain_lint's
        # staleness check reads this field). On a brand-new capture they're
        # the same date — that's expected.
        ("updated", now.strftime("%Y-%m-%d")),
        ("type", "note"),
    ])
    path.write_text(frontmatter.serialize(fm, text + "\n"), encoding="utf-8")

    rel = path.relative_to(config.VAULT_PATH)

    if config.PUSH_AFTER_CAPTURE:
        try:
            vault.commit_and_push(f"capture: {rel}", [path])
        except vault.GitError as exc:
            return f"Wrote {rel} but git push failed: {exc}{pull_warning}"

    return f"Captured to {rel}{pull_warning}"
