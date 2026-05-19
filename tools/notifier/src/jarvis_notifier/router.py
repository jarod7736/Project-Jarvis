"""Priority router — the brains of the notifier.

Tier semantics (matches plans/adhd-brainstorm.md §"Notification priority
tiers"):

    high   → MQTT publish to jarvis/speak immediately +
             Pushover send (if configured).
    medium → enqueue on disk. If device is currently IDLE, drain
             immediately after IDLE_DRAIN_DELAY_SEC. Otherwise wait for
             the next IDLE transition. No Pushover by default.
    low    → log only. No device push, no phone push. Visible in
             /queue and the morning brief (when Sprint 2 lands).

Backpressure: the queue caps at QUEUE_MAX_DEPTH (default 100); FIFO
drops the oldest item when overfull. Dropped items are logged so the
loss is visible.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Literal

from . import config, pushover
from .mqtt_client import MqttBridge
from .queue_store import QueueItem, QueueStore

log = logging.getLogger(__name__)

Tier = Literal["high", "medium", "low"]
VALID_TIERS: tuple[Tier, ...] = ("high", "medium", "low")


@dataclass
class DispatchResult:
    accepted: bool
    tier: Tier
    item_id: str | None
    spoken: bool          # True iff MQTT publish to jarvis/speak fired
    pushed: bool          # True iff Pushover send returned ok
    queued: bool          # True iff item was added to the medium queue
    detail: dict[str, Any]


class Router:
    def __init__(self, mqtt: MqttBridge, queue: QueueStore) -> None:
        self.mqtt = mqtt
        self.queue = queue
        self._log_path = Path(config.LOG_PATH)
        self._log_path.parent.mkdir(parents=True, exist_ok=True)
        # Track stats for /healthz.
        self._stats = {
            "high_count": 0,
            "medium_count": 0,
            "low_count": 0,
            "drained_count": 0,
            "dropped_count": 0,
        }
        # Used by the state-watcher task to debounce the IDLE drain — we
        # only fire one drain per IDLE-edge, not on every state event.
        self._idle_drain_task: asyncio.Task[None] | None = None

    # ── Audit log (jsonl) ────────────────────────────────────────────
    def _log_event(self, event: dict[str, Any]) -> None:
        event = {"ts": time.time(), **event}
        try:
            with self._log_path.open("a", encoding="utf-8") as f:
                f.write(json.dumps(event) + "\n")
        except OSError as exc:
            log.error("log write failed: %s", exc)

    # ── Public surface ──────────────────────────────────────────────
    async def dispatch(
        self,
        text: str,
        tier: Tier,
        source: str | None = None,
        title: str | None = None,
    ) -> DispatchResult:
        if tier not in VALID_TIERS:
            raise ValueError(f"invalid tier: {tier!r}")

        self._stats[f"{tier}_count"] += 1

        if tier == "low":
            self._log_event({
                "kind": "dispatch", "tier": "low", "text": text,
                "source": source, "title": title,
            })
            return DispatchResult(
                accepted=True, tier="low", item_id=None,
                spoken=False, pushed=False, queued=False,
                detail={"reason": "low-tier-log-only"},
            )

        if tier == "high":
            spoken = self.mqtt.publish_speak(text)
            push_result = await pushover.send(
                text,
                title=title or "Jarvis",
                priority=config.PUSHOVER_PRIORITY_HIGH,
            )
            self._log_event({
                "kind": "dispatch", "tier": "high", "text": text,
                "source": source, "title": title,
                "spoken": spoken, "pushover": push_result,
            })
            return DispatchResult(
                accepted=True, tier="high", item_id=None,
                spoken=spoken,
                pushed=bool(push_result.get("ok")),
                queued=False,
                detail={"pushover": push_result},
            )

        # medium
        item = QueueItem(text=text, tier="medium", source=source, title=title)
        _, dropped = self.queue.push(item)
        if dropped is not None:
            self._stats["dropped_count"] += 1
            self._log_event({"kind": "drop", "item": asdict(dropped)})
        self._log_event({"kind": "queue", "item": asdict(item)})

        # If the device is already IDLE, schedule a drain. The watcher
        # task ordinarily fires drains on IDLE *transitions*; this path
        # covers the case where the device has been idle and we just
        # received a new medium item.
        if self.mqtt.last_state == "IDLE":
            self._schedule_idle_drain()

        return DispatchResult(
            accepted=True, tier="medium", item_id=item.id,
            spoken=False, pushed=False, queued=True,
            detail={
                "queue_depth": self.queue.depth(),
                "dropped_id": dropped.id if dropped else None,
            },
        )

    # ── IDLE drain plumbing ──────────────────────────────────────────
    def _schedule_idle_drain(self) -> None:
        """Schedule a single drain of the head queue item after the
        configured delay. If a drain is already pending, do nothing —
        we only want one outstanding drain at a time."""
        if self._idle_drain_task is not None and not self._idle_drain_task.done():
            return
        if self.queue.depth() == 0:
            return
        self._idle_drain_task = asyncio.create_task(self._drain_after_delay())

    async def _drain_after_delay(self) -> None:
        try:
            await asyncio.sleep(config.IDLE_DRAIN_DELAY_SEC)
            # Re-check state right before publishing — the device might
            # have moved back into LISTENING/SPEAKING in the meantime
            # (e.g. user wake-worded during the delay window). In that
            # case, do nothing; the next IDLE transition will retry.
            if self.mqtt.last_state != "IDLE":
                log.info(
                    "drain aborted: device no longer idle (state=%s)",
                    self.mqtt.last_state,
                )
                return
            item = self.queue.pop_head()
            if item is None:
                return
            spoken = self.mqtt.publish_speak(item.text)
            self._stats["drained_count"] += 1
            self._log_event({
                "kind": "drain", "item_id": item.id, "text": item.text,
                "spoken": spoken,
            })
            log.info("drained item id=%s spoken=%s", item.id, spoken)
        except asyncio.CancelledError:
            pass
        except Exception as exc:
            log.exception("drain task crashed: %s", exc)

    async def watch_state(self) -> None:
        """Long-running task: react to device state transitions and
        drain the medium queue whenever the device hits IDLE. Cancelled
        on app shutdown."""
        log.info("state watcher starting")
        try:
            while True:
                prev, new = await self.mqtt.next_state_event()
                if new == "IDLE" and prev != "IDLE":
                    log.info("IDLE transition observed; scheduling drain")
                    self._schedule_idle_drain()
        except asyncio.CancelledError:
            log.info("state watcher cancelled")
            raise

    # ── /healthz surface ─────────────────────────────────────────────
    def health(self) -> dict[str, Any]:
        return {
            "status": "ok",
            "mqtt_connected": self.mqtt.connected,
            "device_state": self.mqtt.last_state,
            "queue_depth": self.queue.depth(),
            "pushover_enabled": config.pushover_enabled(),
            "stats": dict(self._stats),
        }

    # ── Manual drain (admin endpoint) ────────────────────────────────
    async def drain_one(self) -> dict[str, Any]:
        """Force one drain regardless of device state. Useful for
        operator testing / morning-brief style scheduled flushes."""
        item = self.queue.pop_head()
        if item is None:
            return {"drained": False, "reason": "queue-empty"}
        spoken = self.mqtt.publish_speak(item.text)
        self._stats["drained_count"] += 1
        self._log_event({
            "kind": "drain_manual", "item_id": item.id, "text": item.text,
            "spoken": spoken,
        })
        return {"drained": True, "item_id": item.id, "spoken": spoken}
