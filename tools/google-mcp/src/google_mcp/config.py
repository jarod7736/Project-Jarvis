"""Configuration. Overridable via environment variables.

OpenClaw's oc-personal-runner spawns this server as a stdio child with
``GOOGLE_CREDENTIALS_PATH`` and ``GOOGLE_TOKEN_PATH`` in the spawn env —
see ``tools/oc-personal-runner/systemd/oc-personal.service``.
"""

from __future__ import annotations

import os
from pathlib import Path


def _expand(path: str) -> Path:
    return Path(os.path.expanduser(path))


# OAuth 2.0 desktop-app client secrets, downloaded from Google Cloud Console.
# This file is sensitive but not as sensitive as the token: it identifies the
# *application*, not the user. Mode 0600 still recommended.
CREDENTIALS_PATH = _expand(os.environ.get(
    "GOOGLE_CREDENTIALS_PATH",
    "~/.config/oc-personal/google-credentials.json",
))

# User token (access + refresh) produced by the consent flow. Sensitive —
# anyone with this file can read/draft email and create calendar events as
# the authorized account. Mode 0600.
TOKEN_PATH = _expand(os.environ.get(
    "GOOGLE_TOKEN_PATH",
    "~/.config/oc-personal/google-token.json",
))

# Scopes requested at consent time. The token is bound to this exact set —
# changes here require re-running the consent flow.
#
#   calendar.events       list + create events on calendars the user owns
#   gmail.readonly        list + read messages (inbox, threads, search)
#   gmail.compose         create drafts. NOTE: this scope ALSO permits
#                         sending; we deliberately do NOT expose a send tool
#                         to enforce a "human reviews in Gmail before send"
#                         policy. The safety boundary lives in our tool
#                         surface, not in the scope.
SCOPES = [
    "https://www.googleapis.com/auth/calendar.events",
    "https://www.googleapis.com/auth/gmail.readonly",
    "https://www.googleapis.com/auth/gmail.compose",
]
