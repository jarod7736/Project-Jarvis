"""FastAPI entry point for oc-personal-runner.

Deployment (lobsterboy):

    cd ~/project-jarvis/tools/oc-personal-runner
    python -m venv .venv
    .venv/bin/pip install -e .

    # Smoke test (foreground)
    ANTHROPIC_API_KEY=sk-ant-... \\
    OC_LMSTUDIO_URL=http://192.168.1.108:1234 \\
    OC_BRAIN_MCP_COMMAND=/home/$USER/project-jarvis/tools/brain-mcp/.venv/bin/python \\
        .venv/bin/python -m oc_personal.server

    # Then from another shell:
    curl -s http://localhost:8080/v1/chat/completions \\
        -H 'Content-Type: application/json' \\
        -d '{"model":"oc-personal","messages":[{"role":"user","content":"what do I know about kettlebells"}]}' | jq .

Production: install the systemd unit at tools/oc-personal-runner/systemd/
oc-personal.service and `systemctl enable --now oc-personal.service`.

Once running, point Jarvis's NVS `oc_host` at this server's URL — e.g.
http://lobsterboy.tail1c66ec.ts.net:8080 — and Jarvis's existing personal_query
and journal_note intents wire up end-to-end.
"""

from __future__ import annotations

import logging
import os
import sys
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
import uvicorn

from . import config
from .agent import BrainAgent
from .proxy import LMStudioProxy

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
log = logging.getLogger("oc_personal")


# Module-level singletons populated in lifespan().
agent: BrainAgent | None = None
proxy: LMStudioProxy | None = None


@asynccontextmanager
async def lifespan(_app: FastAPI):
    global agent, proxy
    if not config.ANTHROPIC_API_KEY:
        log.error("ANTHROPIC_API_KEY is not set; oc-personal calls will fail.")

    agent = BrainAgent()
    proxy = LMStudioProxy()
    async with agent.lifecycle():
        log.info(
            "ready: listening on %s:%d, personal_model=%s, lmstudio=%s",
            config.LISTEN_HOST,
            config.LISTEN_PORT,
            config.PERSONAL_MODEL,
            config.LMSTUDIO_URL,
        )
        try:
            yield
        finally:
            if proxy is not None:
                await proxy.aclose()


app = FastAPI(lifespan=lifespan)


@app.get("/healthz")
async def healthz() -> dict[str, Any]:
    """Liveness probe. Reports whether the agent's MCP session is up and
    which model is registered for the personal path."""
    return {
        "status": "ok",
        "personal_model": config.PERSONAL_MODEL,
        "anthropic_model": config.ANTHROPIC_MODEL,
        "lmstudio": config.LMSTUDIO_URL,
        "agent_ready": bool(agent and agent._sessions),
        "mcp_servers": sorted(agent._sessions.keys()) if agent else [],
        "tools": [t["name"] for t in (agent._tools_anthropic if agent else [])],
    }


@app.post("/v1/chat/completions")
async def chat_completions(request: Request) -> JSONResponse:
    body = await request.json()
    model = body.get("model")
    if model == config.PERSONAL_MODEL:
        if agent is None:
            return JSONResponse({"error": "agent not initialized"}, status_code=503)
        return JSONResponse(await agent.handle(body))
    if proxy is None:
        return JSONResponse({"error": "proxy not initialized"}, status_code=503)
    return JSONResponse(await proxy.forward(body))


def main() -> None:
    # Force stock asyncio. uvloop's subprocess transport closes the stdin
    # WriteUnixTransport before mcp's stdio_client task group can write
    # to it, producing BrokenResourceError on session.initialize().
    uvicorn.run(
        app,
        host=config.LISTEN_HOST,
        port=config.LISTEN_PORT,
        loop="asyncio",
        log_level=os.environ.get("OC_LOG_LEVEL", "info"),
    )


if __name__ == "__main__":
    sys.exit(main())
