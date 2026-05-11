"""Google Calendar tool implementations.

Time inputs/outputs are ISO 8601. The Google Calendar API accepts both
date-only (``2026-05-12``) and full timestamps (``2026-05-12T14:30:00-07:00``);
we pass strings through. When the caller omits a time window, default to
"events from now through 24 hours" — the common voice-query horizon.
"""

from __future__ import annotations

import json
from datetime import datetime, timedelta, timezone

from googleapiclient.discovery import build
from googleapiclient.errors import HttpError

from . import auth


def _service():
    return build("calendar", "v3", credentials=auth.get_credentials(), cache_discovery=False)


def _now_iso_z() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _hours_from_now(hours: int) -> str:
    return (datetime.now(timezone.utc) + timedelta(hours=hours)).strftime("%Y-%m-%dT%H:%M:%SZ")


def list_events(
    time_min: str | None = None,
    time_max: str | None = None,
    max_results: int = 10,
    calendar: str = "primary",
) -> str:
    """Return upcoming events on the named calendar in a time window.

    Defaults span [now, now+24h]. Use explicit ISO 8601 timestamps for other
    windows. ``calendar`` is the calendar ID — "primary" is the user's main
    calendar. Other IDs look like ``family@group.calendar.google.com``.
    """
    try:
        creds_ok = auth.get_credentials()
    except auth.AuthError as exc:
        return f"Auth error: {exc}"

    tmin = time_min or _now_iso_z()
    tmax = time_max or _hours_from_now(24)
    try:
        resp = _service().events().list(
            calendarId=calendar,
            timeMin=tmin,
            timeMax=tmax,
            maxResults=max_results,
            singleEvents=True,
            orderBy="startTime",
        ).execute()
    except HttpError as exc:
        return f"Calendar API error: {exc}"

    items = []
    for ev in resp.get("items", []):
        start = ev.get("start", {})
        end = ev.get("end", {})
        items.append({
            "id": ev.get("id"),
            "summary": ev.get("summary", "(no title)"),
            "start": start.get("dateTime") or start.get("date"),
            "end": end.get("dateTime") or end.get("date"),
            "location": ev.get("location"),
            "attendees": [a.get("email") for a in ev.get("attendees", []) if a.get("email")],
        })
    return json.dumps({
        "calendar": calendar,
        "time_min": tmin,
        "time_max": tmax,
        "count": len(items),
        "events": items,
    }, indent=2)


def create_event(
    summary: str,
    start: str,
    end: str,
    location: str | None = None,
    description: str | None = None,
    calendar: str = "primary",
) -> str:
    """Create an event. ``start`` and ``end`` are ISO 8601.

    Returns the event ID and the URL the user can click to see/edit it in
    Google Calendar. Does not invite attendees — keeping the tool narrow
    so voice can't accidentally fire emails at people.
    """
    if not summary.strip():
        return "Error: summary is empty — refusing to create a titleless event."

    try:
        auth.get_credentials()
    except auth.AuthError as exc:
        return f"Auth error: {exc}"

    body: dict[str, object] = {
        "summary": summary,
        "start": _time_field(start),
        "end": _time_field(end),
    }
    if location:
        body["location"] = location
    if description:
        body["description"] = description

    try:
        ev = _service().events().insert(calendarId=calendar, body=body).execute()
    except HttpError as exc:
        return f"Calendar API error: {exc}"

    return json.dumps({
        "id": ev.get("id"),
        "summary": ev.get("summary"),
        "start": ev.get("start"),
        "end": ev.get("end"),
        "htmlLink": ev.get("htmlLink"),
    }, indent=2)


def _time_field(value: str) -> dict[str, str]:
    """Render a user-supplied ISO time string into the API's start/end shape.

    Date-only strings (``2026-05-12``) become all-day events via the ``date``
    field. Anything else goes through as ``dateTime``.
    """
    value = value.strip()
    if len(value) == 10 and value[4] == "-" and value[7] == "-":
        return {"date": value}
    return {"dateTime": value}
