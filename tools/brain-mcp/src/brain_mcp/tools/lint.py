"""brain_lint — structural audit of the wiki/.

Mirrors the laptop's /brain-lint skill but returns structured signals rather
than a prose report. The four deterministic checks are fully resolved here;
the three interpretive checks return raw co-occurrence data the calling
agent can summarize prose-style if asked.

Deterministic checks: Orphans, Broken Links, Index Gaps, Missing Pages.
Interpretive (raw signals only): Stale, Missing Cross-References, Data Gaps.
"""

from __future__ import annotations

import json
import re
from collections import defaultdict
from pathlib import Path

from .. import config


_WIKI_LINK_RE = re.compile(r"\[\[([^\]|#]+?)(?:#[^\]|]*)?(?:\|[^\]]+)?\]\]")


def _slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def _collect_pages() -> list[Path]:
    if not config.WIKI_DIR.exists():
        return []
    return sorted(p for p in config.WIKI_DIR.rglob("*.md") if p.is_file())


def run() -> str:
    pages = _collect_pages()
    if not pages:
        return json.dumps({
            "error": f"No pages under {config.WIKI_DIR}.",
        }, indent=2)

    # slug → Path. Filename-derived slug is what `[[Page Name]]` resolves to.
    page_slug_to_path: dict[str, Path] = {p.stem.lower(): p for p in pages}
    page_slugs = set(page_slug_to_path)

    inbound: dict[str, set[str]] = defaultdict(set)
    broken_links: list[dict[str, str]] = []
    target_mentions: dict[str, set[str]] = defaultdict(set)

    contents: dict[Path, str] = {}
    for p in pages:
        try:
            contents[p] = p.read_text(encoding="utf-8")
        except OSError:
            contents[p] = ""

    for src_path, text in contents.items():
        src_rel = str(src_path.relative_to(config.VAULT_PATH))
        for raw_link in _WIKI_LINK_RE.findall(text):
            target_slug = _slug(raw_link)
            target_mentions[target_slug].add(src_rel)
            if target_slug in page_slugs:
                inbound[target_slug].add(src_rel)
            else:
                broken_links.append({"in": src_rel, "link": raw_link})

    # 1. Orphans
    orphans = sorted(
        str(page_slug_to_path[s].relative_to(config.VAULT_PATH))
        for s in page_slugs
        if not inbound[s]
    )

    # 2. Broken links — already collected.

    # 3. Index gaps
    index_gaps: list[str] = []
    if config.INDEX_MD.exists():
        index_text = config.INDEX_MD.read_text(encoding="utf-8")
        for p in pages:
            rel = str(p.relative_to(config.VAULT_PATH))
            if rel not in index_text and p.stem not in index_text:
                index_gaps.append(rel)
    else:
        index_gaps = ["<index.md missing>"]

    # 5. Missing pages — link targets not present, mentioned in 2+ pages
    missing_pages = [
        {"slug": slug, "mentioned_in": sorted(srcs)}
        for slug, srcs in target_mentions.items()
        if slug not in page_slugs and len(srcs) >= 2
    ]
    missing_pages.sort(key=lambda m: -len(m["mentioned_in"]))

    # 4. Stale candidates — raw signals only. Frontmatter `updated` parsing
    #    is intentionally minimal; the calling agent decides what counts as
    #    stale relative to source dates it can read separately.
    stale_candidates: list[dict[str, str]] = []
    fm_updated_re = re.compile(r"^updated:\s*(\S+)\s*$", re.MULTILINE)
    for p, text in contents.items():
        m = fm_updated_re.search(text[:1000])
        if m:
            stale_candidates.append({
                "path": str(p.relative_to(config.VAULT_PATH)),
                "updated": m.group(1),
            })

    # 6. Missing cross-references — bare entity-like names appearing in
    #    multiple pages without a [[link]] wrapping. Heuristic: title-cased
    #    multi-word phrases. Returns raw signals only.
    titlecase_re = re.compile(r"\b([A-Z][a-z]+(?:\s+[A-Z][a-z]+){1,3})\b")
    mention_index: dict[str, set[str]] = defaultdict(set)
    for p, text in contents.items():
        rel = str(p.relative_to(config.VAULT_PATH))
        # Strip wiki-links so we count only un-linked mentions.
        bare = _WIKI_LINK_RE.sub("", text)
        for match in titlecase_re.findall(bare):
            mention_index[match].add(rel)
    cross_ref_candidates = [
        {"phrase": phrase, "mentioned_in": sorted(srcs)}
        for phrase, srcs in mention_index.items()
        if len(srcs) >= 2 and _slug(phrase) not in page_slugs
    ]
    cross_ref_candidates.sort(key=lambda c: -len(c["mentioned_in"]))
    cross_ref_candidates = cross_ref_candidates[:25]

    report = {
        "vault": str(config.VAULT_PATH),
        "page_count": len(pages),
        "orphans": orphans,
        "broken_links": broken_links,
        "index_gaps": index_gaps,
        "missing_pages": missing_pages,
        "stale_candidates": stale_candidates,
        "cross_ref_candidates": cross_ref_candidates,
    }
    return json.dumps(report, indent=2)
