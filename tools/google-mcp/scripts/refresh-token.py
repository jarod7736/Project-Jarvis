#!/usr/bin/env python3
"""Proactively refresh the Google OAuth token.

Run this on a schedule (every 5 days) to prevent refresh-token revocation.
Google revokes refresh tokens for unverified apps after 7 days of inactivity.
Calling get_credentials() forces an access-token refresh, which counts as
activity and resets the 7-day clock.

Exit 0 on success, 1 on any auth failure (alertable via cron mail).
"""
import sys

from google_mcp.auth import get_credentials


def main() -> None:
    try:
        creds = get_credentials()
        print(f"OK: token refreshed, expiry={creds.expiry}")
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
