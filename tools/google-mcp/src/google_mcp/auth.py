"""OAuth token loading + refresh for the long-running server.

The server reads ``TOKEN_PATH`` at every API call (lazy + cached). If the
access token is stale, ``google.auth.transport.requests.Request`` refreshes
it via the refresh token and we rewrite the file. If the refresh fails
(refresh token revoked, etc.), tools return an error string that surfaces
to the agent ("Google auth has expired — re-run google-mcp-auth.").

We never perform the interactive consent flow at server runtime — that's
the ``oauth_cli`` module's job. A running MCP server has no terminal and
no browser; bootstrapping must happen out of band.
"""

from __future__ import annotations

import json
import logging
from typing import Any

from google.auth.transport.requests import Request
from google.oauth2.credentials import Credentials

from . import config

log = logging.getLogger(__name__)


class AuthError(RuntimeError):
    """Raised when no usable token is available. Caller should surface to user."""


_cached: Credentials | None = None


def _load_credentials() -> Credentials:
    if not config.TOKEN_PATH.exists():
        raise AuthError(
            f"No token at {config.TOKEN_PATH}. Run "
            f"`python -m google_mcp.oauth_cli` (or `google-mcp-auth`) once on "
            f"a machine with a browser to mint one."
        )
    creds = Credentials.from_authorized_user_file(str(config.TOKEN_PATH), config.SCOPES)
    return creds


def _persist(creds: Credentials) -> None:
    data: dict[str, Any] = json.loads(creds.to_json())
    config.TOKEN_PATH.write_text(json.dumps(data, indent=2), encoding="utf-8")
    # Best-effort tightening; OAuth tokens should not be world-readable.
    try:
        config.TOKEN_PATH.chmod(0o600)
    except OSError:
        pass


def get_credentials() -> Credentials:
    """Return a valid Credentials object, refreshing if needed."""
    global _cached
    if _cached is None:
        _cached = _load_credentials()

    if not _cached.valid:
        if _cached.expired and _cached.refresh_token:
            try:
                _cached.refresh(Request())
            except Exception as exc:  # noqa: BLE001 — surface any auth failure
                _cached = None
                raise AuthError(
                    f"Token refresh failed ({exc}). Re-run google-mcp-auth."
                ) from exc
            _persist(_cached)
        else:
            raise AuthError(
                "Token is invalid and has no refresh_token. Re-run google-mcp-auth."
            )

    return _cached
