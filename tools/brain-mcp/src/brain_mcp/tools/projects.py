"""brain_list_projects + brain_set_next_action — project-tracking tools.

Schema convention (kept loose so existing pages can opt in incrementally):

    ---
    type: project
    status: active | backlog | done | abandoned
    next_action: <imperative phrase>
    priority: high | medium | low
    updated: YYYY-MM-DD
    ---

``type: project`` is the discriminator brain_list_projects filters on.
None of the other fields are required — pages may have status without a
next_action, or vice versa. Missing fields are returned as null.

Both tools are deliberately narrow:

  - list_projects is read-only and does not pull. The brain-sync timer
    pulls every 5 minutes; the staleness budget for "what projects are
    active" is well within that.
  - set_next_action does the smallest possible edit (one or two
    frontmatter keys + commit + push) so the diff is reviewable and the
    git history reads as a series of decisions, not a series of churn.
"""

from __future__ import annotations

import json
from pathlib import Path

from .. import config, frontmatter, vault


_VALID_STATUSES = {"active", "backlog", "done", "abandoned"}


def _collect_pages() -> list[Path]:
    if not config.WIKI_DIR.exists():
        return []
    return sorted(p for p in config.WIKI_DIR.rglob("*.md") if p.is_file())


def _summarize(path: Path) -> dict[str, object] | None:
    """Read a page; return a project summary or None if not type: project."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None
    fm, _ = frontmatter.parse(text)
    if fm.get("type") != "project":
        return None
    rel = path.relative_to(config.VAULT_PATH).as_posix()
    return {
        "path": rel,
        "status": fm.get("status"),
        "next_action": fm.get("next_action"),
        "priority": fm.get("priority"),
        "updated": fm.get("updated"),
    }


def list_projects(status: str | None = None) -> str:
    """Return JSON list of pages with ``type: project`` frontmatter.

    Filters by status if provided. Sorted by status (active first), then
    by priority (high first), then by updated descending — so the most
    actionable items surface first in voice responses.
    """
    if status is not None and status not in _VALID_STATUSES:
        return json.dumps({
            "error": f"Unknown status {status!r}. Valid: {sorted(_VALID_STATUSES)}",
        }, indent=2)

    projects: list[dict[str, object]] = []
    for page in _collect_pages():
        summary = _summarize(page)
        if summary is None:
            continue
        if status is not None and summary.get("status") != status:
            continue
        projects.append(summary)

    status_rank = {"active": 0, "backlog": 1, "done": 2, "abandoned": 3}
    priority_rank = {"high": 0, "medium": 1, "low": 2}
    # Stable-sort trick: secondary key first (descending), then primary keys.
    # Python's sort is guaranteed stable, so this leaves projects ordered by
    # (status asc, priority asc, updated desc) with missing values sorted last.
    projects.sort(key=lambda p: str(p.get("updated") or ""), reverse=True)
    projects.sort(key=lambda p: (
        status_rank.get(str(p.get("status") or ""), 99),
        priority_rank.get(str(p.get("priority") or ""), 99),
    ))

    return json.dumps({
        "vault": str(config.VAULT_PATH),
        "filter": {"status": status} if status else None,
        "count": len(projects),
        "projects": projects,
    }, indent=2)


def _resolve_page(name: str) -> Path | None:
    """Map a user-supplied page name to a real file under wiki/.

    Accepts: ``boat``, ``boat.md``, ``projects/boat``, full ``wiki/...`` path,
    or absolute path that resolves under VAULT_PATH. Case-insensitive on
    stem comparison so voice transcripts ("Boat") match disk ("boat.md").
    """
    if not name:
        return None
    candidate = name.strip()
    if not candidate.endswith(".md"):
        candidate_md = candidate + ".md"
    else:
        candidate_md = candidate

    # Try direct path under WIKI_DIR.
    direct = config.WIKI_DIR / candidate_md
    if direct.is_file():
        return direct

    # Fall back to a case-insensitive stem scan. Avoid matching outside
    # WIKI_DIR even if the user supplied a traversal-y path.
    target_stem = Path(candidate_md).stem.lower()
    for page in _collect_pages():
        if page.stem.lower() == target_stem:
            return page
    return None


def set_next_action(page: str, action: str) -> str:
    """Set ``next_action:`` on a project page; bump ``updated:``; commit + push.

    If the page doesn't yet have ``type: project``, this tool refuses —
    creating projects is an intentional act and should go through editing
    on the laptop side, not a voice command (avoids accidental
    proliferation of half-baked project pages from misheard ASR).
    """
    action = action.strip()
    if not action:
        return "Error: action is empty — nothing to set."

    path = _resolve_page(page)
    if path is None:
        return f"Error: no wiki page matches {page!r}."

    text = path.read_text(encoding="utf-8")
    fm, body = frontmatter.parse(text)
    if fm.get("type") != "project":
        return (
            f"Refusing to edit {path.relative_to(config.VAULT_PATH)}: "
            f"frontmatter type is {fm.get('type')!r}, not 'project'. "
            f"Add `type: project` on the laptop first."
        )

    before = dict(fm)
    fm["next_action"] = action
    fm["updated"] = frontmatter.today_iso()

    if dict(fm) == before:
        return f"No change — {path.relative_to(config.VAULT_PATH)} already has that next_action."

    new_text = frontmatter.serialize(fm, body)
    path.write_text(new_text, encoding="utf-8")

    rel = path.relative_to(config.VAULT_PATH)
    if config.PUSH_AFTER_CAPTURE:
        try:
            vault.commit_and_push(f"project: set next_action on {rel}", [path])
        except vault.GitError as exc:
            return f"Edited {rel} but git push failed: {exc}"

    return f"Set next_action on {rel}: {action}"
