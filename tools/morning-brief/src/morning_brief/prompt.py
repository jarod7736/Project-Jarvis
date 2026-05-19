"""The morning-brief prompt.

Designed for ADHD-friendly delivery (plans/adhd-brainstorm.md §"What
I'd actually pick first" / "Verification"):

  - ONE focus, not a list. Lists get reviewed twice and then ignored.
  - Spoken-style — this is going to TTS, not a screen.
  - Calendar anchors the day; project next-action drives the focus.
  - Lead with the focus, not preamble. The user is half-awake.

Stale resurface (Sprint 2 #10): bias the focus toward projects whose
`updated:` field hasn't moved in a while. brain_list_projects returns
this date per-project and sorts oldest-last — the agent picks the
focus accordingly. A second sentence is allowed when something has
been silently stalling, so things on the boil but un-picked still
surface ("stalled projects can't hide").

Editable: tune by editing this file. systemctl restart on the .timer
is not needed (the .service re-reads on each run). For prompt
experimentation without firing the device, set MORNING_BRIEF_DRY_RUN=1
and run the service manually via `./deploy.sh test`.
"""

from __future__ import annotations

PROMPT = """\
Good morning. Compose a brief that picks ONE focus for today and, if
something has been quietly stalling, optionally surfaces it.

Steps:
1. Check my Google Calendar for today's events (gcal_list_events).
2. Look at my active projects (brain_list_projects, status=active).
   Each project has an `updated: YYYY-MM-DD` field — the last time its
   next_action moved. The list is sorted by updated descending, so the
   freshest project is first and the stalest is last.

When picking the focus:
- Prefer the next_action that will move the needle most for me.
- Among comparable options, prefer the project that's been stale longest.
  A project with status=active that hasn't been updated in 7+ days is
  drifting; pull it forward.
- The calendar shapes what's *feasible* today; don't let it pick the
  focus. If the calendar locks the day, name the focus you'd pick if
  I only had 30 minutes between meetings.

Output rules:
- 1-2 short sentences. No preamble. Spoken-style.
- Lead with the focus.
- If a hard meeting anchors the day, mention it briefly so I orient.
- If a project has been stale for 14+ days AND wasn't picked as the
  focus, add a SECOND short sentence flagging it. Format:
  "Also, <project> has been quiet for <N> days — worth 15 minutes."
  Otherwise skip the second sentence entirely. Never both a calendar
  anchor AND a stale callout — pick the more urgent signal.
- Never more than two sentences total. Never a list.
- Avoid filler ("today is a great day for..."). Just say the thing.

Examples:
- "Two meetings today, kickoff at 2 PM. Focus before then is the boat:
   order the bilge pump."
- "Calendar is open. Spend today on the kettlebell program design — it
   has been quiet for 12 days."
- "Dentist at 10. Focus today is agent prompt-engineering. Also, the
   bilge pump has been on your boat list untouched for 21 days."
- "Quiet day. Pick up the kettlebell design where you left off."

If something blocks the steps (no active projects, agent loop hits its
turn cap, calendar tool errors), still produce a single sentence —
even just "Quiet day; nothing pending. Plan it."
"""
