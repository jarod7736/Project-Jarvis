"""One-time OAuth consent CLI.

Run this once on a machine with a browser (your laptop, your phone if you
SSH-tunnel the loopback port, …) to mint the refresh token the server
needs. Subsequent restarts of google-mcp on lobsterboy don't re-prompt —
the refresh token rotates automatically.

Typical use::

    # On the host that will run google-mcp (lobsterboy), with port-forward
    # from your laptop so the browser can reach the loopback redirect:
    ssh -L 8090:localhost:8090 lobsterboy
    cd ~/Project-Jarvis/tools/google-mcp
    .venv/bin/python -m google_mcp.oauth_cli --port 8090

    # Or all-local: run on your laptop where the browser opens
    # automatically, then scp the resulting token file to lobsterboy.
    .venv/bin/python -m google_mcp.oauth_cli
    scp ~/.config/oc-personal/google-token.json lobsterboy:.config/oc-personal/

The CLI writes to ``TOKEN_PATH`` (default
``~/.config/oc-personal/google-token.json``) with mode 0600.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from google_auth_oauthlib.flow import InstalledAppFlow

from . import config


def _ensure_credentials_file(path: Path) -> None:
    if path.exists():
        return
    sys.stderr.write(
        f"No client-secrets file at {path}.\n\n"
        f"To generate one:\n"
        f"  1. https://console.cloud.google.com/apis/credentials\n"
        f"  2. Create a project (any name).\n"
        f"  3. APIs & Services → Library → enable Gmail API + Google Calendar API.\n"
        f"  4. OAuth consent screen → External → add yourself as a test user.\n"
        f"  5. Credentials → Create credentials → OAuth client ID → Desktop app.\n"
        f"  6. Download the JSON and save it as {path} (mode 0600).\n"
    )
    raise SystemExit(2)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "--port",
        type=int,
        default=0,
        help=(
            "Loopback port for the OAuth redirect. 0 picks a free port (works "
            "when running locally with a browser). Use a fixed port like 8090 "
            "if you're SSH-forwarding from a laptop to a headless host."
        ),
    )
    parser.add_argument(
        "--credentials",
        type=Path,
        default=config.CREDENTIALS_PATH,
        help=f"Path to client-secrets JSON. Default: {config.CREDENTIALS_PATH}",
    )
    parser.add_argument(
        "--token",
        type=Path,
        default=config.TOKEN_PATH,
        help=f"Path to write the resulting token JSON. Default: {config.TOKEN_PATH}",
    )
    args = parser.parse_args()

    _ensure_credentials_file(args.credentials)

    flow = InstalledAppFlow.from_client_secrets_file(str(args.credentials), config.SCOPES)
    # run_local_server spins up a temporary HTTP server on 127.0.0.1:<port>
    # to catch the redirect. Browser opens automatically if one is reachable;
    # otherwise the URL is printed and you open it manually.
    creds = flow.run_local_server(port=args.port, open_browser=False)

    args.token.parent.mkdir(parents=True, exist_ok=True)
    args.token.write_text(creds.to_json(), encoding="utf-8")
    try:
        args.token.chmod(0o600)
    except OSError:
        pass

    print(f"OK: wrote token to {args.token}")
    data = json.loads(creds.to_json())
    print("scopes:", data.get("scopes"))
    print("expiry:", data.get("expiry"))


if __name__ == "__main__":
    main()
