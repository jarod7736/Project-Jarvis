"""Gmail tool implementations.

Read freely; drafts only on the write side — no send tool. The
``gmail.compose`` scope technically permits sending, but we don't expose
that on the MCP surface. Users review drafts in the Gmail UI before
sending.
"""

from __future__ import annotations

import base64
import json
from email.message import EmailMessage

from googleapiclient.discovery import build
from googleapiclient.errors import HttpError

from . import auth


def _service():
    return build("gmail", "v1", credentials=auth.get_credentials(), cache_discovery=False)


def list_unread(max_results: int = 10) -> str:
    """Return subjects + from + snippet for the most recent unread messages."""
    try:
        auth.get_credentials()
    except auth.AuthError as exc:
        return f"Auth error: {exc}"

    svc = _service()
    try:
        listing = svc.users().messages().list(
            userId="me",
            labelIds=["INBOX", "UNREAD"],
            maxResults=max_results,
        ).execute()
    except HttpError as exc:
        return f"Gmail API error: {exc}"

    messages = []
    for ref in listing.get("messages", []):
        try:
            msg = svc.users().messages().get(
                userId="me",
                id=ref["id"],
                format="metadata",
                metadataHeaders=["From", "Subject", "Date"],
            ).execute()
        except HttpError as exc:
            messages.append({"id": ref["id"], "error": str(exc)})
            continue
        headers = {h["name"]: h["value"] for h in msg.get("payload", {}).get("headers", [])}
        messages.append({
            "id": ref["id"],
            "thread_id": msg.get("threadId"),
            "from": headers.get("From"),
            "subject": headers.get("Subject"),
            "date": headers.get("Date"),
            "snippet": msg.get("snippet"),
        })

    return json.dumps({"count": len(messages), "messages": messages}, indent=2)


def search(query: str, max_results: int = 10) -> str:
    """Search the user's mail with Gmail's query syntax.

    Examples: ``from:dr.smith@example.com``, ``subject:invoice newer_than:7d``,
    ``has:attachment``. See https://support.google.com/mail/answer/7190.
    """
    if not query.strip():
        return "Error: query is empty."

    try:
        auth.get_credentials()
    except auth.AuthError as exc:
        return f"Auth error: {exc}"

    svc = _service()
    try:
        listing = svc.users().messages().list(
            userId="me", q=query, maxResults=max_results,
        ).execute()
    except HttpError as exc:
        return f"Gmail API error: {exc}"

    messages = []
    for ref in listing.get("messages", []):
        msg = svc.users().messages().get(
            userId="me",
            id=ref["id"],
            format="metadata",
            metadataHeaders=["From", "Subject", "Date"],
        ).execute()
        headers = {h["name"]: h["value"] for h in msg.get("payload", {}).get("headers", [])}
        messages.append({
            "id": ref["id"],
            "thread_id": msg.get("threadId"),
            "from": headers.get("From"),
            "subject": headers.get("Subject"),
            "date": headers.get("Date"),
            "snippet": msg.get("snippet"),
        })

    return json.dumps({"query": query, "count": len(messages), "messages": messages}, indent=2)


def get_thread(thread_id: str) -> str:
    """Return the messages in a thread, oldest first."""
    if not thread_id.strip():
        return "Error: thread_id is empty."

    try:
        auth.get_credentials()
    except auth.AuthError as exc:
        return f"Auth error: {exc}"

    try:
        thread = _service().users().threads().get(userId="me", id=thread_id).execute()
    except HttpError as exc:
        return f"Gmail API error: {exc}"

    messages = []
    for msg in thread.get("messages", []):
        headers = {h["name"]: h["value"] for h in msg.get("payload", {}).get("headers", [])}
        body = _extract_text(msg.get("payload", {}))
        messages.append({
            "id": msg.get("id"),
            "from": headers.get("From"),
            "to": headers.get("To"),
            "subject": headers.get("Subject"),
            "date": headers.get("Date"),
            "snippet": msg.get("snippet"),
            "body": body[:4000],  # cap so big threads don't blow the agent's context
        })
    return json.dumps({"id": thread_id, "messages": messages}, indent=2)


def create_draft(to: str, subject: str, body: str) -> str:
    """Create a Gmail draft addressed to ``to``. Does NOT send.

    Voice-friendly safety boundary: the agent can compose, but the human
    reviews and clicks Send from the Gmail UI. ``to`` is a single email
    address or a comma-separated list.
    """
    if not to.strip():
        return "Error: to is empty."
    if not subject.strip():
        return "Error: subject is empty."

    try:
        auth.get_credentials()
    except auth.AuthError as exc:
        return f"Auth error: {exc}"

    msg = EmailMessage()
    msg["To"] = to
    msg["Subject"] = subject
    msg.set_content(body)
    raw = base64.urlsafe_b64encode(msg.as_bytes()).decode("utf-8")

    try:
        draft = _service().users().drafts().create(
            userId="me",
            body={"message": {"raw": raw}},
        ).execute()
    except HttpError as exc:
        return f"Gmail API error: {exc}"

    return json.dumps({
        "draft_id": draft.get("id"),
        "message_id": draft.get("message", {}).get("id"),
        "to": to,
        "subject": subject,
        "review_url": "https://mail.google.com/mail/u/0/#drafts",
    }, indent=2)


def _extract_text(payload: dict) -> str:
    """Best-effort plaintext extraction from a Gmail message payload."""
    mime_type = payload.get("mimeType", "")
    if mime_type.startswith("text/"):
        data = payload.get("body", {}).get("data")
        if data:
            return base64.urlsafe_b64decode(data).decode("utf-8", errors="replace")
    for part in payload.get("parts", []) or []:
        text = _extract_text(part)
        if text:
            return text
    return ""
