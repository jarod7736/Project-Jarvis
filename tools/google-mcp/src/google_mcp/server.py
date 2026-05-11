"""MCP server entry point: Gmail + Calendar.

Spawned over stdio by oc-personal-runner. Reads ``GOOGLE_TOKEN_PATH`` lazily
on the first tool call rather than at import time, so a missing token
surfaces as a clear per-call error rather than killing the whole server at
startup (which is opaque to the agent loop).

Surface intentionally narrow:

  gcal_list_events       read events in a time window
  gcal_create_event      add an event (no attendee invites)
  gmail_list_unread      inbox unread metadata
  gmail_search           Gmail query-syntax search
  gmail_get_thread       full thread bodies
  gmail_create_draft     compose a draft (no send)

Anything riskier (delete, send, label-modify, full-content edit) is
deliberately omitted — voice-driven mistakes on those would be loud.
"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from . import gcal, gmail


mcp = FastMCP("google")


@mcp.tool()
def gcal_list_events(
    time_min: str | None = None,
    time_max: str | None = None,
    max_results: int = 10,
    calendar: str = "primary",
) -> str:
    """List upcoming Google Calendar events on a calendar in a time window.

    Args:
        time_min: ISO 8601 start of the window. Default: now.
        time_max: ISO 8601 end of the window. Default: 24 hours from now.
        max_results: Cap on returned events. Default 10.
        calendar: Calendar ID. "primary" is the user's main calendar.
    """
    return gcal.list_events(
        time_min=time_min, time_max=time_max,
        max_results=max_results, calendar=calendar,
    )


@mcp.tool()
def gcal_create_event(
    summary: str,
    start: str,
    end: str,
    location: str | None = None,
    description: str | None = None,
    calendar: str = "primary",
) -> str:
    """Create a Google Calendar event.

    Args:
        summary: Event title.
        start: ISO 8601 start. Date-only (``2026-05-12``) creates an all-day
            event; full ``2026-05-12T14:30:00-07:00`` creates a timed event.
        end: ISO 8601 end. Same format rules as start.
        location: Optional free-text location.
        description: Optional free-text description.
        calendar: Calendar ID. Default "primary".

    Does NOT invite attendees — voice-friendly safety. To invite people,
    edit the event in the Calendar UI after it's created.
    """
    return gcal.create_event(
        summary=summary, start=start, end=end,
        location=location, description=description, calendar=calendar,
    )


@mcp.tool()
def gmail_list_unread(max_results: int = 10) -> str:
    """List the most recent unread messages in the inbox.

    Returns from / subject / date / snippet for each. Useful for "what's in
    my inbox" queries.
    """
    return gmail.list_unread(max_results=max_results)


@mcp.tool()
def gmail_search(query: str, max_results: int = 10) -> str:
    """Search the user's mail using Gmail's query syntax.

    Examples: ``from:dr.smith@example.com``, ``subject:invoice newer_than:7d``,
    ``has:attachment``. See https://support.google.com/mail/answer/7190.
    """
    return gmail.search(query=query, max_results=max_results)


@mcp.tool()
def gmail_get_thread(thread_id: str) -> str:
    """Return the messages in a thread (oldest first) with bodies.

    Bodies are capped at 4 KB each so a long thread doesn't blow out the
    agent's context. Use ``thread_id`` from a previous list/search result.
    """
    return gmail.get_thread(thread_id=thread_id)


@mcp.tool()
def gmail_create_draft(to: str, subject: str, body: str) -> str:
    """Create a Gmail draft. Does NOT send.

    Voice-driven safety: the agent composes, the human reviews + sends from
    the Gmail UI. Returns ``draft_id`` and a ``review_url`` for the drafts
    folder.

    Args:
        to: Single email address or comma-separated list.
        subject: Draft subject.
        body: Plaintext body. No HTML.
    """
    return gmail.create_draft(to=to, subject=subject, body=body)


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
