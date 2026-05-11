"""Minimal Obsidian-flavored YAML frontmatter parser/serializer.

The 2nd Brain vault uses simple ``key: value`` frontmatter — scalars only,
no lists / nested maps / multiline strings in practice. We deliberately
avoid a PyYAML dependency: it's ~2 MB on disk plus a slow import, and we
only need the subset Obsidian writes.

Round-trip contract:

    fm, body = parse(text)
    text_out = serialize(fm, body)
    assert parse(text_out) == (fm, body)

Preserves key insertion order so edits don't churn frontmatter shape
unnecessarily (matters for git diff readability).
"""

from __future__ import annotations

from collections import OrderedDict


_FENCE = "---"


def parse(text: str) -> tuple["OrderedDict[str, str]", str]:
    """Split text into (frontmatter dict, body).

    If the text does not begin with a ``---`` fence, returns an empty
    frontmatter dict and the input as body. Tolerates BOMs and a single
    leading blank line, both of which Obsidian occasionally emits.
    """
    fm: OrderedDict[str, str] = OrderedDict()
    src = text.lstrip("﻿")

    # Allow one leading blank line before the fence — Obsidian sometimes
    # produces this when a plugin appends to an empty file.
    stripped = src.lstrip("\n")
    if not stripped.startswith(_FENCE):
        return fm, text

    lines = stripped.splitlines(keepends=True)
    # lines[0] is the opening fence
    end_idx = None
    for i in range(1, len(lines)):
        if lines[i].rstrip("\r\n") == _FENCE:
            end_idx = i
            break
    if end_idx is None:
        # Opening fence with no close — treat as no frontmatter rather than
        # mangling the file.
        return fm, text

    for raw in lines[1:end_idx]:
        line = raw.rstrip("\r\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        key, sep, value = line.partition(":")
        if not sep:
            # Malformed line — skip rather than error. Preserve in body? No:
            # if the fence said this is frontmatter, the user expects FM
            # semantics. Dropping silently is the lesser evil.
            continue
        fm[key.strip()] = value.strip()

    body = "".join(lines[end_idx + 1 :])
    return fm, body


def serialize(fm: "OrderedDict[str, str] | dict[str, str]", body: str) -> str:
    """Render frontmatter + body back to text.

    Empty frontmatter dict is rendered with no fence at all (matches how
    Obsidian renders fence-less notes — avoids gratuitous diffs on files
    that never had frontmatter to begin with).

    Body is emitted verbatim. Caller is responsible for any trailing
    newline normalization.
    """
    if not fm:
        return body
    lines = [_FENCE]
    for key, value in fm.items():
        lines.append(f"{key}: {value}")
    lines.append(_FENCE)
    return "\n".join(lines) + "\n" + body


def today_iso() -> str:
    """ISO date string (YYYY-MM-DD) suitable for the ``updated:`` field.

    Date precision rather than full timestamp — matches the staleness
    semantics in ``brain_lint`` and avoids unhelpful churn on edits that
    happen multiple times the same day.
    """
    from datetime import datetime, timezone

    return datetime.now(timezone.utc).strftime("%Y-%m-%d")
