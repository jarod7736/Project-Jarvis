"""FastAPI entry point for jarvis-notifier.

Deployment (lobsterboy):

    cd ~/project-jarvis/tools/notifier
    python -m venv .venv
    .venv/bin/pip install -e .

    # Smoke test (foreground):
    NOTIFIER_MQTT_HOST=localhost \\
    NOTIFIER_MQTT_USER=jarvis \\
    NOTIFIER_MQTT_PASS=... \\
    PUSHOVER_TOKEN=... PUSHOVER_USER_KEY=... \\
    .venv/bin/python -m jarvis_notifier.server

    # Then from another shell:
    curl -sX POST http://localhost:8081/notify \\
        -H 'Content-Type: application/json' \\
        -d '{"text":"hello jarvis","tier":"high"}'

Production: install the systemd unit at tools/notifier/systemd/
jarvis-notifier.service via tools/notifier/deploy.sh.
"""

from __future__ import annotations

import asyncio
import logging
import os
import sys
from contextlib import asynccontextmanager
from typing import Any, Literal

from fastapi import FastAPI, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field
import uvicorn

from . import config
from .mqtt_client import MqttBridge
from .queue_store import QueueStore
from .router import VALID_TIERS, Router, Tier

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
log = logging.getLogger("jarvis_notifier")


class NotifyRequest(BaseModel):
    text: str = Field(..., min_length=1, max_length=1024)
    tier: Literal["high", "medium", "low"] = "medium"
    source: str | None = Field(None, max_length=64)
    title: str | None = Field(None, max_length=250)


# Module-level singletons populated by the lifespan handler.
router: Router | None = None
mqtt: MqttBridge | None = None
queue: QueueStore | None = None
_watch_task: asyncio.Task[None] | None = None


@asynccontextmanager
async def lifespan(_app: FastAPI):
    global router, mqtt, queue, _watch_task

    loop = asyncio.get_running_loop()
    queue = QueueStore(config.QUEUE_PATH, max_depth=config.QUEUE_MAX_DEPTH)
    mqtt = MqttBridge(loop)
    router = Router(mqtt, queue)

    mqtt.start()
    _watch_task = loop.create_task(router.watch_state(), name="state-watcher")

    log.info(
        "ready: listening on %s:%d, mqtt=%s:%d, queue=%s (depth=%d), pushover=%s",
        config.LISTEN_HOST, config.LISTEN_PORT,
        config.MQTT_HOST, config.MQTT_PORT,
        config.QUEUE_PATH, queue.depth(),
        "on" if config.pushover_enabled() else "off",
    )
    try:
        yield
    finally:
        if _watch_task is not None:
            _watch_task.cancel()
            try:
                await _watch_task
            except (asyncio.CancelledError, Exception):
                pass
        if mqtt is not None:
            mqtt.stop()


app = FastAPI(title="jarvis-notifier", lifespan=lifespan)


@app.get("/healthz")
async def healthz() -> dict[str, Any]:
    if router is None:
        return {"status": "starting"}
    return router.health()


@app.post("/notify")
async def notify(req: NotifyRequest) -> dict[str, Any]:
    if router is None:
        raise HTTPException(status_code=503, detail="router not ready")
    if req.tier not in VALID_TIERS:
        raise HTTPException(status_code=400, detail="invalid tier")
    result = await router.dispatch(
        text=req.text, tier=req.tier, source=req.source, title=req.title,
    )
    return {
        "accepted": result.accepted,
        "tier": result.tier,
        "item_id": result.item_id,
        "spoken": result.spoken,
        "pushed": result.pushed,
        "queued": result.queued,
        "detail": result.detail,
    }


@app.get("/queue")
async def get_queue() -> JSONResponse:
    if queue is None:
        raise HTTPException(status_code=503, detail="queue not ready")
    return JSONResponse(queue.list_all())


@app.delete("/queue/{item_id}")
async def delete_queue_item(item_id: str) -> dict[str, Any]:
    if queue is None:
        raise HTTPException(status_code=503, detail="queue not ready")
    removed = queue.remove(item_id)
    if removed is None:
        raise HTTPException(status_code=404, detail="not found")
    return {"removed": True, "item_id": item_id}


@app.post("/queue/drain")
async def drain_one() -> dict[str, Any]:
    """Force one drain — for operator testing or scheduled morning-brief
    style flushes. Bypasses the IDLE check."""
    if router is None:
        raise HTTPException(status_code=503, detail="router not ready")
    return await router.drain_one()


def main() -> None:
    uvicorn.run(
        app,
        host=config.LISTEN_HOST,
        port=config.LISTEN_PORT,
        log_level=os.environ.get("NOTIFIER_LOG_LEVEL", "info"),
    )


if __name__ == "__main__":
    sys.exit(main())
