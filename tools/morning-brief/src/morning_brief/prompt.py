"""The morning-brief prompt.

Designed for ADHD-friendly delivery (plans/adhd-brainstorm.md §"What
I'd actually pick first" / "Verification"):

  - ONE focus, not a list. Lists get reviewed twice and then ignored.
  - Spoken-style — this is going to TTS, not a screen.
  - Calendar anchors the day; project next-action drives the focus.
  - Lead with the focus, not preamble. The user is half-awake.

Editable: tune by editing this file and `systemctl restart
morning-brief.timer` is not needed (the .service re-reads on each run).
For prompt experimentation without firing the device, set
MORNING_BRIEF_DRY_RUN=1 and run the service manually.
"""

from __future__ import annotations

PROMPT = """\
Good morning. Compose a one-sentence focus for my day.

Steps:
1. Check my Google Calendar for today's events (gcal_list_events).
2. Look at my active projects with their next actions
   (brain_list_projects, status=active).

Then deliver ONE concrete thing to focus on today. Pick what will move
the needle most given my calendar constraints. Rules:

- Lead with the focus. No preamble like "Here's your morning brief".
- Be terse. 1-2 short sentences, spoken-style.
- If a hard meeting anchors the day, mention it briefly so I orient.
- If the calendar is open, just name the focus.
- Never list everything. Pick ONE.
- Avoid filler ("today is a great day for..."). Just say the thing.

Examples of good replies:
- "Two meetings today, kickoff at 2 PM. Focus before then is the boat:
   order the bilge pump."
- "Calendar is open. Spend today on the kettlebell program design.
   It's been stalled longest."
- "Heads-up: dentist at 10. Pick up where you left off on the agent
   prompt-engineering after."

If something blocks the steps (no active projects, calendar empty AND
no projects, agent loop hits its turn cap), still produce a single
sentence — even if it's just "Quiet day; nothing pending. Plan it."
"""
