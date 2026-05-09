"""brain_search — return wiki/ pages most relevant to a query.

Mirrors the laptop's /brain-query skill semantics: read index.md to identify
candidates, then score by filename, frontmatter tag, and body keyword
overlap. Returns raw markdown — the calling agent (OpenClaw with this MCP
attached) does the synthesis.

Deliberately not a vector search. Phase 8's design note: if a real index is
added later (sqlite-vec, chroma over wiki/), it slots in behind this same
signature.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .. import config


_STOPWORDS = {
    "a", "an", "the", "is", "are", "was", "were", "be", "been", "being",
    "of", "to", "in", "on", "at", "by", "for", "with", "about", "into",
    "from", "up", "down", "out", "over", "under", "again", "then", "once",
    "here", "there", "when", "where", "why", "how", "all", "any", "both",
    "each", "few", "more", "most", "other", "some", "such", "no", "nor",
    "not", "only", "own", "same", "so", "than", "too", "very", "can",
    "will", "just", "should", "now", "i", "me", "my", "we", "our", "you",
    "your", "he", "him", "his", "she", "her", "it", "its", "they", "them",
    "their", "what", "which", "who", "whom", "this", "that", "these",
    "those", "am", "do", "does", "did", "doing", "have", "has", "had",
    "and", "but", "if", "or", "because", "as", "until", "while", "know",
    "read", "tell", "find", "show", "get", "getting", "say", "said",
    "thing", "things", "stuff", "lot", "really",
}


@dataclass
class _Hit:
    path: Path
    score: int
    title_overlap: int
    body_hits: int

    @property
    def rel(self) -> str:
        return str(self.path.relative_to(config.VAULT_PATH))


def _tokenize(text: str) -> set[str]:
    words = re.findall(r"[a-zA-Z][a-zA-Z0-9'-]*", text.lower())
    return {w for w in words if w not in _STOPWORDS and len(w) > 2}


def _score(path: Path, query_terms: set[str], content: str) -> _Hit | None:
    if not query_terms:
        return None
    title = path.stem.replace("-", " ").replace("_", " ").lower()
    title_terms = _tokenize(title)
    title_overlap = len(query_terms & title_terms)

    content_lc = content.lower()
    body_hits = sum(content_lc.count(t) for t in query_terms)

    if title_overlap == 0 and body_hits == 0:
        return None

    # Title overlap weighted heavily so the right page rises even with thin
    # body matches; body_hits provides the tiebreaker between sibling pages.
    score = title_overlap * 10 + body_hits
    return _Hit(path=path, score=score, title_overlap=title_overlap, body_hits=body_hits)


def run(query: str, k: int = 8) -> str:
    if not query.strip():
        return "Error: empty query."

    query_terms = _tokenize(query)
    if not query_terms:
        return f"No searchable terms in query: {query!r}"

    if not config.WIKI_DIR.exists():
        return (
            f"Wiki directory not found at {config.WIKI_DIR}. "
            f"Is BRAIN_VAULT_PATH set correctly and the repo cloned?"
        )

    hits: list[_Hit] = []
    for path in config.WIKI_DIR.rglob("*.md"):
        try:
            content = path.read_text(encoding="utf-8")
        except OSError:
            continue
        hit = _score(path, query_terms, content)
        if hit is not None:
            hits.append(hit)

    hits.sort(key=lambda h: h.score, reverse=True)
    top = hits[:k]

    if not top:
        return (
            f"No wiki pages matched query terms {sorted(query_terms)}. "
            f"This means either the wiki doesn't cover the topic yet, or "
            f"the query phrasing didn't match indexed terms — try synonyms."
        )

    parts = [
        f"# Top {len(top)} of {len(hits)} matches for: {query}",
        f"_Query terms: {sorted(query_terms)}_",
        "",
    ]
    for hit in top:
        try:
            content = hit.path.read_text(encoding="utf-8")
        except OSError:
            continue
        parts.append(
            f"## {hit.rel}  "
            f"(score={hit.score}, title={hit.title_overlap}, body={hit.body_hits})"
        )
        parts.append("")
        parts.append(content.rstrip())
        parts.append("")
        parts.append("---")
        parts.append("")

    return "\n".join(parts)
