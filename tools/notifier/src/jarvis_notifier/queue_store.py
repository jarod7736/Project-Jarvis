"""File-backed FIFO queue for medium-priority notifications.

Atomicity story: writes go to a temp file and are renamed into place
(os.replace is atomic on POSIX). Reads load the whole file. The queue is
tiny — capped at QUEUE_MAX_DEPTH (default 100) — so JSON-round-tripping
the whole thing on every mutation is fine and the simplicity is worth more
than a sqlite-backed alternative.

Concurrency story: only the notifier process touches the queue. FastAPI
handlers run on a single asyncio loop, and the MQTT thread surfaces
events through a thread-safe asyncio queue (see mqtt_client.py) — so
queue mutations always happen on the event-loop thread. Hence no lock.
"""

from __future__ import annotations

import json
import logging
import os
import tempfile
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path

log = logging.getLogger(__name__)


@dataclass
class QueueItem:
    text: str
    tier: str  # "medium" — high never enters, low never enters
    source: str | None = None
    title: str | None = None
    id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])
    queued_at: float = field(default_factory=time.time)


class QueueStore:
    """Disk-backed FIFO. Newest goes to the end; we drain from the head."""

    def __init__(self, path: str | os.PathLike[str], max_depth: int) -> None:
        self.path = Path(path)
        self.max_depth = max_depth
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._items: list[QueueItem] = self._load()

    # ── Load / persist ────────────────────────────────────────────────
    def _load(self) -> list[QueueItem]:
        if not self.path.exists():
            return []
        try:
            with self.path.open("r", encoding="utf-8") as f:
                raw = json.load(f)
            return [QueueItem(**entry) for entry in raw]
        except (json.JSONDecodeError, TypeError, ValueError) as exc:
            # Corrupted file — back it up and start fresh rather than
            # crash-loop on every startup. The lost notifications are
            # less important than service availability.
            backup = self.path.with_suffix(".corrupt.json")
            log.error(
                "queue file unreadable (%s); backing up to %s and starting empty",
                exc, backup,
            )
            try:
                self.path.replace(backup)
            except OSError:
                pass
            return []

    def _persist(self) -> None:
        data = [asdict(item) for item in self._items]
        # Atomic write: temp file in the same dir (so rename is atomic),
        # then os.replace into place.
        tmp_fd, tmp_path = tempfile.mkstemp(
            prefix=".queue-", suffix=".json", dir=self.path.parent
        )
        try:
            with os.fdopen(tmp_fd, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
            os.replace(tmp_path, self.path)
        except Exception:
            # Clean up the temp file if the rename never happened.
            try:
                os.unlink(tmp_path)
            except OSError:
                pass
            raise

    # ── Public API ───────────────────────────────────────────────────
    def push(self, item: QueueItem) -> tuple[QueueItem, QueueItem | None]:
        """Add an item to the tail. If the queue is at cap, drop the head
        FIFO and return it as the second element so the caller can log
        the loss."""
        dropped: QueueItem | None = None
        if len(self._items) >= self.max_depth:
            dropped = self._items.pop(0)
            log.warning(
                "queue at cap (%d); dropping oldest item id=%s",
                self.max_depth, dropped.id,
            )
        self._items.append(item)
        self._persist()
        return item, dropped

    def pop_head(self) -> QueueItem | None:
        """Pop the oldest item. Returns None if empty."""
        if not self._items:
            return None
        item = self._items.pop(0)
        self._persist()
        return item

    def peek_head(self) -> QueueItem | None:
        return self._items[0] if self._items else None

    def list_all(self) -> list[dict]:
        return [asdict(item) for item in self._items]

    def remove(self, item_id: str) -> QueueItem | None:
        """Remove by ID. Returns the removed item or None if not found."""
        for i, item in enumerate(self._items):
            if item.id == item_id:
                self._items.pop(i)
                self._persist()
                return item
        return None

    def depth(self) -> int:
        return len(self._items)
