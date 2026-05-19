"""oc-personal-runner — agent runner that gives Jarvis's `oc-personal` model
a real implementation.

Phase 8 of Project Jarvis. Runs on lobsterboy. Listens on /v1/chat/completions
in OpenAI-compat shape:
  - model="oc-personal" -> agent loop with Claude + brain-mcp tools
  - any other model      -> proxied to an OpenAI-compat backend at
                            config.BACKEND_URL (Ollama by default)

See `tools/oc-personal-runner/src/oc_personal/server.py` for deployment notes.
"""

__version__ = "0.1.0"
