"""Configuration. Everything overridable via environment variables.

The MCP server is invoked over stdio by OpenClaw's agent loop; OpenClaw is
responsible for setting these in the spawn environment. Defaults are tuned
for the lobsterboy deploy.
"""

from __future__ import annotations

import os
from pathlib import Path

# Working clone of jarod7736/2ndBrain on the host running this server.
VAULT_PATH = Path(os.environ.get("BRAIN_VAULT_PATH", "/srv/2ndbrain"))

# Git remote and branch for capture write-back.
GITHUB_REMOTE = os.environ.get("BRAIN_REMOTE", "origin")
GITHUB_BRANCH = os.environ.get("BRAIN_BRANCH", "main")

# Whether brain_capture should commit + push after writing. Set to "0" for
# local testing against a vault that isn't a real git working copy.
PUSH_AFTER_CAPTURE = os.environ.get("BRAIN_PUSH", "1") == "1"

# Whether brain_capture should pull --ff-only before writing. Off by default
# because the systemd timer pulls every 5 min anyway; turning this on adds
# ~300ms to every voice note for marginal freshness benefit.
PULL_BEFORE_CAPTURE = os.environ.get("BRAIN_PULL_BEFORE_CAPTURE", "0") == "1"

# Derived paths.
RAW_DIR = VAULT_PATH / "raw"
RAW_NOTES_DIR = RAW_DIR / "notes"
RAW_ARTICLES_DIR = RAW_DIR / "articles"
RAW_PDFS_DIR = RAW_DIR / "pdfs"
WIKI_DIR = VAULT_PATH / "wiki"
INDEX_MD = VAULT_PATH / "index.md"
LOG_MD = VAULT_PATH / "log.md"
