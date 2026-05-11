# ADHD + AI: Design-Space Brainstorm

## Context

You have ADHD and trouble with: tracking projects (specifically stalled WIP + can't-prioritize-among-active), remembering TODOs, writing things down, using the calendar, and phone calls (especially transactional ones: medical/insurance, customer service, service providers).

You asked for a brainstorm, not a build plan. So this doc surveys the design space, names patterns that fit your specific failure modes, and lists buildable options with rough costs. **No commitments — this is a menu to react to, not a phased roadmap.**

The key insight: **you already have ~70% of the infrastructure built.** What's missing is mostly the *proactive* side and a handful of integrations. The expensive parts (voice capture, persistent searchable memory, an agent with tool access) are done.

## What you already have

| Capability | Where it lives | Status |
|---|---|---|
| Voice capture, zero friction | Jarvis device (`journal_note` intent) | Live, just deployed |
| Persistent searchable memory | 2ndBrain wiki (Obsidian + GitHub) | Live |
| Voice-asked knowledge retrieval | `brain_search` via `oc-personal` | Live |
| Backend agent with MCP tool access | lobsterboy `oc-personal-runner` | Live |
| Home control / sensor reads | HA REST API | Live |
| Personal calendar / email / drive | Gmail/GDrive/GCal MCPs already authenticated against personal Google account | Available, not yet wired into oc-personal-runner |
| Work calendar / email | Outlook tenant, MDM-locked | Walled off — see "When work data is walled off" |
| Anthropic API on lobsterboy | Yes | Available |

The big gap: **Jarvis is reactive — it can only speak when spoken to.** Reminders, morning briefs, "did you forget X?" pings — none of that exists yet. Adding it is mostly plumbing (MQTT publish from lobsterboy → CoreS3 subscribes → speak), not new science. Phase 7 already lists MQTT.

## Design principles for an ADHD-friendly stack

1. **Zero-friction capture.** If it takes more than 3 seconds, it doesn't happen. Voice notes via Jarvis already nail this.
2. **Active push, not passive review.** Lists you "review every morning" get reviewed twice and then ignored forever. Reminders and digests must come to you (TTS via Jarvis, push to phone) — not wait for you to ask.
3. **One-shot prioritization.** Don't make you pick from a list — surface ONE thing. "Today, work on the boat for 30 minutes" beats "you have 14 active projects, choose."
4. **External brain owns the timeline.** Calendars, deadlines, next-actions live in the system, not in your head. The system tells you when to act.
5. **Resurface windows.** Captured ≠ remembered. A daily/weekly resurface pass closes the loop between "I wrote it down" and "I did something about it."
6. **Make the avoided thing easier than the avoidance.** Drafting an email is easier than making a call if the draft is 80% done before you see it. The system should pre-draft.

## Notification priority tiers

You said you want a priority knob: some reminders should interrupt verbally, others shouldn't. Proposed three tiers — adjust freely:

| Tier | Channel | Use cases |
|---|---|---|
| **High** (interrupt) | Jarvis TTS push immediately + iOS push notification | Pre-meeting reminders within 15 min; medication; "leave now"; high-stakes follow-ups (insurance reply due today) |
| **Medium** (next-idle) | Jarvis TTS push next time the device is idle, or rolled into the next morning brief; no iOS push | Project next-action nudges; stale-WIP resurface; Friday digest items |
| **Low** (silent) | Logged to wiki only — visible in morning brief if relevant; no push of any kind | Captured-but-unprocessed notes; ambient observations; long-tail follow-ups |

Voice can override at create time: *"Hey Jarvis, high-priority reminder: call the vet at 2 PM"* sets tier per-item. Default for new captures is **medium**. iOS push uses Pushover (cheap, simple, works well with Apple) or native iOS Shortcuts via webhook — both fine, Pushover is faster to wire.

## Wiki organization — what 2ndBrain already has, and what it's missing

I read through `tools/brain-mcp/src/brain_mcp/*.py` since that's the surface that talks to your 2ndBrain vault. Here's the actual shape today:

**What's already there:**
- **Folders:** `raw/notes/` (voice captures land here), `raw/articles/`, `raw/pdfs/`, plus a flat `wiki/` for everything else; root-level `index.md` (canonical page index) and `log.md`.
- **Frontmatter (minimal but useful):** `source`, `captured_at`, `type`, `updated`. The `updated:` field is already consumed by `brain_lint` for stale-candidate detection — that's a freebie.
- **MCP tools:** `brain_search` (keyword + filename scoring), `brain_capture` (writes a dated note to `raw/notes/`, commits, pushes), `brain_lint` (orphans, broken links, stale candidates, missing index entries), `brain_ingest_status` (what's in `raw/` not yet referenced from `index.md`).
- **File naming:** ISO-timestamped slugs for captures; basename wikilinks (`[[Boat]]`) for everything else.

**What's missing for ADHD project tracking specifically:**

1. **A queryable "this is a project" marker.** Either a `wiki/projects/` subfolder (basename wikilinks still resolve fine) or a `type: project` frontmatter field. The latter fits your existing flat-`wiki/` design better — recommend that.
2. **Project-tracking frontmatter** on project pages: `status: active|backlog|done|abandoned`, `next_action: <imperative phrase>`, `priority: high|medium|low`. Reuse the existing `updated:` for staleness — already wired into `brain_lint`.
3. **Two new brain-mcp tools:**
   - `brain_set_next_action(page, action)` — edits frontmatter on the named page, bumps `updated:`, commits.
   - `brain_list_projects(status?)` — filters notes where `type: project` and optionally `status: active`. Powers the morning brief and "what's next on the boat" queries.
4. **Auto-bump `updated:`** on every brain-mcp write (capture, set_next_action, future ingest). Closes the loop on staleness without requiring you to remember to touch the field.

**Net assessment:** the substrate is ~80% there. Sprint 0 is therefore not "scaffold a wiki from scratch" — it's "extend brain-mcp with three frontmatter conventions, two new tools, and an auto-bump rule, then run a one-time pass to convert your existing project-shaped pages to the new schema." Roughly **half a day of Python work in `tools/brain-mcp/`** plus a short batch edit of existing pages. Folder reorg is optional; the `type: project` frontmatter approach is non-disruptive.

## Pain points → tooling matches

### Stalled WIP — "next step on the boat is fuzzy"

**Pattern:** every project page gets a `next_action:` frontmatter field. The system aggressively maintains it.

- **Voice ask:** *"Hey Jarvis, what's my next step on the boat?"* → `personal_query` → `brain_search` for `wiki/projects/boat.md` → reads the `next_action` field → speaks it.
- **Auto-resurface:** lobsterboy daily cron: scan `wiki/projects/*` for `status: active` pages whose `next_action` hasn't been updated in N days. Push a digest via MQTT → Jarvis speaks: *"Two projects are stalled: boat (last update 12 days ago), and Phase 7 enclosure (last update 9 days ago). Want to set a next step on either?"*
- **Voice-set:** *"Hey Jarvis, next step on the boat is order the bilge pump"* → new intent `project_update` → agent edits the frontmatter and commits via brain-mcp. (New tool: `brain_set_next_action(project, action)`).

**Why this works for ADHD:** the question "what's next?" gets answered for you. You don't have to remember, prioritize, or re-derive. The system holds the next step.

### Can't prioritize among active threads

**Pattern:** daily morning brief picks ONE thing. Not three. Not a list. One.

- **Morning brief intent:** *"Hey Jarvis, brief me"* (or scheduled MQTT push at 8 AM) → agent reads `wiki/projects/*` for active projects, considers `next_action`, recent activity, any calendar deadlines today/this week, then picks one and speaks 2 sentences: *"Focus this morning: boat bilge pump install. You ordered it Tuesday, it should be in. Anything else?"*
- **"Make me decide" path:** when you protest, agent gives top 3. *"OK, options: boat, Phase 7 enclosure, or replying to Dr. Smith. Which?"* You pick by voice.
- **Drift detection:** if your captured raw/notes for the week show 0 mentions of the surfaced project, agent flags it next morning: *"You haven't touched the boat all week. Demote to backlog or keep active?"*

**Why this works:** ADHD struggle isn't lack of options, it's TOO many options. Surfacing one removes the choice paralysis.

### Phone calls (transactional)

**Pattern: tiered escalation, AI picks the tier.**

```
1. Web portal (patient portal, customer portal)   ← AI fills it
2. Web form / contact form                         ← AI fills it
3. Email                                           ← AI drafts; you send
4. SMS / text                                      ← AI drafts; you send
5. AI-drafted phone script                         ← you call, AI in your ear
6. AI makes the call autonomously                  ← Bland.ai / Vapi / OpenAI Realtime
```

For each call you'd otherwise dread:

- **You voice it:** *"Hey Jarvis, I need to schedule a vet appointment for the cat."*
- **Agent assesses:** searches the vet's website for an online booking form (or remembers from your wiki). If form exists → fills it from your stored personal data, presents for confirmation. If not → drafts SMS or email. If neither → drafts a 5-bullet phone script with expected branches. If you've authorized autonomous calls → uses Bland.ai to place the call, reads your script, then summarizes outcome via TTS.
- **Outcome lands in 2ndBrain:** `raw/notes/2026-05-09-vet-appointment.md` — date, what was scheduled, follow-ups.

Specifically for your top 3 call categories:

- **Medical/insurance** — almost all have patient portals now (MyChart, Cigna One Guide, etc). High-leverage to wire your insurer + main doctors as known portals in your wiki; agent always checks portal first. Prior-authorizations and claim-status questions are increasingly chat-based.
- **Customer service** — companies-that-pretend-to-hide-their-email is a known dark pattern. Many can be reached via Twitter DM or escalated through tools like GetHuman. For account/subscription changes, AI-makes-the-call (Bland.ai) is genuinely good for narrow tasks: *"call Comcast, cancel my service, get a confirmation number."*
- **Service providers** — most plumbers/contractors text. AI-drafted SMS is basically a solved problem with Twilio's API + an agent that knows your address and the problem.

**Autonomous calling tradeoffs:**
- Pros: removes the dread completely; can run during business hours while you sleep.
- Cons: legal in some states only; misrepresentation risk; debugging a botched call is annoying; you still need to verify the outcome.
- Sensible split: drafts/avoidance for 90% of cases; autonomous only for narrow, well-defined, low-stakes tasks (appointment scheduling, simple cancellations).

### Forgetting things you wrote down

**Pattern: scheduled resurface, not user-initiated review.**

- **Daily brief includes captures:** *"Yesterday you noted: pick up dry cleaning, call the plumber, the boat needs a new bilge pump. Want me to process any of these?"*
- **Weekly Friday digest:** unprocessed `raw/notes/*` from the week → AI summarizes themes, asks if any should become projects, drafts ingestion plan for the laptop `/brain-ingest` skill to run.
- **Forgetting-the-forgotten:** every 30 days, agent scans the wiki for entries with frontmatter `followup_by:` in the past — speaks the list at the morning brief.

### Calendar drift

**Pattern: calendar pushes you, you don't push the calendar.**

This subsection applies to your *personal* calendar (Google) only — your work calendar (Outlook) is walled off, see the next section. Good news: you've already enabled Gmail, Google Drive, and Google Calendar MCPs against your personal Google account, so wiring them into oc-personal-runner is trivial (option #4 / #5 below).

- **Pre-event TTS:** lobsterboy cron polls calendar via MCP. 1 hour before each event, MQTT publish → Jarvis speaks: *"Heads up: 9 AM dentist, leave by 8:30."*
- **Morning brief includes today's events.** Already a freebie if you build the brief.
- **Voice ask:** *"Hey Jarvis, what's on my calendar today?"* → new intent `calendar_query` → agent calls Calendar MCP via oc-personal-runner.
- **Voice add:** *"Hey Jarvis, add dinner with Sarah Thursday at 7"* → new intent `calendar_add` → agent calls Calendar MCP `create_event` and confirms via TTS.

### Initial planning never happens (sub-failure of stalled WIP)

**Pattern: AI bootstraps the project file from a 30-second voice dump.**

- *"Hey Jarvis, new project: renovate the back porch. Need to figure out permits, materials, contractor, timeline."*
- Agent creates `wiki/projects/back-porch-reno.md` with frontmatter (`status: active`, `created: <date>`, `next_action: "decide if permit is needed"`), captures the dump as the body, optionally asks 2-3 clarifying questions during the same voice turn.
- Now there's a thing to come back to. The hardest part — starting — is over in 30 seconds.

## When work data is walled off (your constraint)

You named this directly: work calendar / email / Teams are locked down and can't sync to any device I can reach. Specifically: **personal is Google (MCPs already authenticated), work is Outlook (MDM-locked tenant)**. Personal-side data is effectively a solved problem; work data is the only walled garden. This is the most common ADHD-corporate friction in the world — the rich data structure of your *work* day lives behind a tenant boundary that won't let an outside agent in. It eats several options above for *work events specifically*.

### What's lost vs. what still works

- **Lost without a bridge:** pre-meeting TTS for work meetings, voice-ask "what's on my calendar today" for work, AI scan of work email for action items, voice "add work meeting" to anything queryable later.
- **Still works unchanged:** personal calendar/email (medical, family, vendors), voice capture of work things you say out loud, project tracking (your wiki holds whatever you tell it), phone-call assistance, morning brief on personal items.

### Bridges to consider

1. **EOD voice dump (and MOD captures, since Jarvis travels with you).** 3-5 minutes at end of each workday: speak tomorrow's calendar, today's follow-ups, blockers, "saying-it-so-I-don't-forget" stuff. Agent structures it overnight into shadow projects + shadow calendar entries inside your wiki. The voice channel is the one thing corporate IT can't lock down — it sidesteps every tenant boundary because the boundary can't reach your mouth. Because Jarvis travels with you, the same channel works for mid-of-day captures (*"Hey Jarvis, note that David moved the standup to 11"*) and immediately-after-meeting captures (*"Hey Jarvis, follow-up from the Q3 meeting: send Sarah the deck by Friday"*). Tradeoff: check your workplace's audio-recording / device policy before keeping Jarvis live on your desk.

2. **Shadow calendar on a personal device.** Apple Calendar or Google Calendar mirrors a sanitized version of your work week. Titles only ("Mtg with Vendor"), no PII. You maintain it manually, or — better — AI maintains it from your EOD dump. Pre-event TTS works again because lobsterboy can read the shadow calendar even though it can't read Outlook.

3. **Phone-photo OCR.** Snap your Outlook week-view with your personal phone, drop the photo somewhere reachable (personal Drive, Photos, Signal-to-yourself). New tool `brain_ocr_capture` reads the photo via Claude vision, extracts events, populates the shadow calendar. Lossy if Outlook redesigns, but real.

4. **OWA at home on a personal browser.** Many tenants allow webmail from any device because the corporate policy is "no SYNCING to MDM-unmanaged devices," not "no LOOKING from any browser." Open it once at start of day, dictate the day's meetings to Jarvis. ~60 seconds.

5. **`.ics` publish from Outlook (cheapest real bridge if your tenant allows it).** Outlook has a "Publish calendar" feature that emits a private `.ics` URL anyone with the URL can subscribe to. Many tenants block this, but some allow it for read-only sharing with family. If yours allows it: lobsterboy subscribes directly to the `.ics` and feeds it into the same Calendar pipeline as your personal calendar. No shadow, no dump, no photos. Worth a 5-minute check — it might just work and obviate most of the other bridges.

6. **Microsoft Authenticator next-meeting widget.** Some tenants surface "your next meeting" through the Authenticator app you're already using for MFA, without granting personal-device email sync. Pure read-only glimpse, but enough for "do I have a meeting in the next 30 minutes." Tenant-config dependent.

7. **Resign yourself.** Treat work as a separate sandbox. AI does everything else: personal projects, errands, evenings, weekends, phone calls, the wiki. Sometimes the bridge-maintenance overhead exceeds the gain. Naming this as a legitimate answer because for some people it really is.

### Skylight calendar — related but probably not a bridge

You mentioned the family Skylight calendar that aggregates everyone's calendars. Two angles:

- **As a data source for lobsterboy:** Skylight has no public API I'm aware of. It pulls calendars *in* (Google, iCloud, Outlook.com personal, `.ics` feeds, Cozi); it doesn't push *out* to third parties. So lobsterboy can't query Skylight directly. The good news: anything Skylight is pulling is also pullable by lobsterboy via the same sources. Whatever your family setup feeds Skylight, the agent can feed off the same upstream.
- **As a reverse signal:** because Skylight is a *display* the family sees, it's already doing some of what we want — your spouse/kids walking past the kitchen see the day. That doesn't replace Jarvis's TTS push but it does mean some calendar drift is already mitigated for shared events.
- **Worth checking:** is your *work* calendar already showing up on the family Skylight via a published `.ics`? If yes, you've already crossed the air gap — lobsterboy can subscribe to the same feed (see bridge #5). If no, it almost certainly means your tenant blocks `.ics` publishing, which closes that option.

### What I'd actually pick

**Option 5 (`.ics` publish) is free if it works.** Try it first — five minutes in Outlook settings. If your tenant allows it, the work-walled-garden problem ~mostly evaporates.

**Otherwise: Option 1 (EOD voice dump) + Option 2 (shadow calendar) as the unified path.** Reuses everything you already have. Add one new intent (`eod_dump`) that captures tomorrow's items into a fresh raw note. Overnight cron on lobsterboy runs the agent over that note: parses out meeting-shaped lines and adds them to your shadow Google/Apple Calendar via Calendar MCP; parses follow-ups into project pages; parses blockers into a "stuck on" wiki section. Morning brief now has work context — not because we cracked the tenant, but because *you* told the system the night before.

Because Jarvis travels with you, the dump pattern is far cheaper than EOD-only would be on a static device — you also get mid-meeting and post-meeting captures into the same raw-note inbox, and the overnight pass cleans them all up together.

Roughly half a day of work on top of what's already shipped. The voice channel is the bridge, and it's the only bridge corporate can't lock.

If the dump pattern proves itself, **Option 3 (photo OCR)** is a nice backup for days you're too cooked to dictate. Snap the week-view Sunday night, agent fills the shadow calendar.

### What this changes upstream

Calendar drift section above still applies, but to your *personal* calendar only — medical, family, errands, vendor visits. Work meetings get to the shadow calendar via the `.ics` (if available), or the dump, or the photo path — and from there participate in pre-event TTS exactly like any other calendar item.

### Open questions for this section

- **Can your work Outlook publish a `.ics` URL?** Five-minute check. Outlook on the web → Settings → Calendar → Shared calendars → Publish a calendar → see if the option is greyed out. If it works: that's the bridge.
- Workplace audio-recording policy is worth a brief gut-check since Jarvis travels with you (some corp offices and client sites don't allow always-on mics, even wake-word ones). Power-on-the-go is fine — Jarvis has a battery; tether to USB-C when at the desk, run on battery when out.

## Concrete options menu

Numbered to make picking easy. Pick 1-3 to talk about.

1. **Reverse channel (MQTT → Jarvis TTS + Pushover → iPhone/iPad).** The foundational unlock for everything proactive. Phase 7 has MQTT listed; the firmware likely already subscribes to `jarvis/command`. Pair it with a Pushover account (~$5 one-time, native Apple notifications) so high-priority items also reach you when you've stepped away from the desk. The notification-tier router decides which channel: Jarvis TTS only, Pushover only, both, or silent log. ~1-2 days of firmware + lobsterboy cron work, plus an hour for Pushover wiring.

2. **Morning brief intent + scheduled push.** Once #1 exists. Scheduled 8 AM via cron; reads calendar + active projects + recent captures; speaks ONE focus. ~half a day on lobsterboy.

3. **Voice ask/set for project next-action.** Wires Jarvis intents (`project_status`, `project_update`) to the brain-mcp tools shipped in #13. Closes the stalled-WIP gap from the voice side. ~half a day across firmware intents and system prompt (assumes #13 has landed).

4. **Calendar MCP wired into oc-personal-runner.** Add personal Google Calendar MCP to the agent's tool list alongside brain-mcp. MCPs are already authenticated; this is a config + system-prompt change. Unlocks "what's today" / "add X to calendar" / pre-event reminders via #1. **~1 hour given the auth is done.**

5. **Gmail MCP wired into oc-personal-runner.** Same shape as #4 — auth already done on the personal Gmail side. Voice: "Hey Jarvis, what's in my inbox" / "Draft a reply to Dr. Smith saying I'll reschedule." **~1 hour.**

6. **Web-form / portal filler.** Bigger chunk. Probably a new tool that uses Playwright/Browser MCP to drive web forms with your stored personal data. The "fill the portal" tier of the call-avoidance ladder. ~few days.

7. **AI-drafts-call-script tool.** Small. Voice: "I need to call Cigna about a claim." → agent uses your wiki context + general knowledge to produce a 5-bullet script with expected branches. Speaks it; also drops a `raw/notes/scripts/` file. ~half a day.

8. **AI-makes-call (Bland.ai or equivalent).** Larger commitment — billing setup, account, prompt-engineering the outbound call agent, summary back to your wiki. ~few days minimum, plus per-call cost. Best to validate the script-tool first before climbing this rung.

9. **Friday digest.** Scheduled weekly LLM pass over the week's captures, projects, and calendar. Emails you a recap + suggested next-actions per project. ~half a day.

10. **Resurface stale items.** Daily scan for `wiki/projects/*` with stale `next_action`, plus old `followup_by:` items. Folds into the morning brief once #2 exists. Trivial after #1 and #3.

11. **EOD voice dump + overnight structuring.** The work walled-garden bridge. New intent `eod_dump` captures the dictation; nightly lobsterboy cron runs the agent over it; meetings go to shadow calendar (via #4), follow-ups go to project pages (via #3), blockers go to a "stuck on" wiki section. Bridges your work day into your personal AI stack without violating any corporate boundary. ~half a day once #4 and #3 exist.

12. **Phone-photo OCR for the shadow calendar.** Backup capture path for days you're too tired for an EOD dump. New tool `brain_ocr_capture` reads a snapshot from your iPhone (via Photos or iCloud Drive) using Claude's vision, extracts events, posts them to the shadow calendar. ~half a day.

13. **Extend brain-mcp for project tracking.** Add `type: project` + `status` + `next_action` + `priority` frontmatter conventions; ship two new tools (`brain_set_next_action`, `brain_list_projects`); auto-bump `updated:` on all writes; one-time batch edit of existing project-shaped pages. Half-day in `tools/brain-mcp/`. Prereq for #2, #3, #9, #10, #11.

14. **Notification priority router.** Small piece of lobsterboy logic that routes proactive messages based on the tier (`high` / `medium` / `low`) — high-priority hits Jarvis TTS + Pushover immediately; medium queues to the next morning brief or idle-window TTS; low logs silently to wiki. Voice can override per-item at create time. ~half a day, depends on #1.

## What I'd actually pick first

A 3-sprint sequence, not a 6-month thing. Each sprint ends with something usable.

**Step zero (today, 5 minutes, zero code):** check whether your work Outlook can publish a private `.ics` URL (settings → calendar → shared calendars → publish). If yes, that bridge is free and the work-walled-off problem mostly evaporates. If no, you fall back to the dump pattern (#11) in sprint 2.

**Sprint 0 — Extend brain-mcp for project tracking (#13, half-day, do this before sprint 1).** Add the `type: project` + `status` + `next_action` + `priority` frontmatter convention to your 2ndBrain vault; ship two new MCP tools (`brain_set_next_action`, `brain_list_projects`); make every brain-mcp write auto-bump `updated:`. Run a one-time batch edit over your existing project-shaped pages to add the new fields. Sprint 1 onward depends on `brain_list_projects` returning real data.

**Sprint 1 — Foundation (1 week).** #1 (reverse channel: MQTT publish from lobsterboy → Jarvis speaks + Pushover to iPhone) + #14 (priority router) + #4 (personal GCal MCP wired into oc-personal-runner) + #5 (personal Gmail MCP wired). Auth is already done on #4 and #5, so the lift is mostly system-prompt + config. Pair the reverse channel with the priority router from day one so you don't ship a too-chatty firehose. After this sprint Jarvis can talk first (with priority awareness), knows your personal calendar, and can read/draft personal email. Step-function ADHD win.

**Sprint 2 — Daily anchor (1 week).** #2 (morning brief, scheduled 8 AM, picks ONE focus) + #3 (project next-action: voice ask + voice set + frontmatter maintenance) + #10 (stale resurface, folded into the morning brief). End of this sprint: your day starts with a one-sentence focus, you can ask "what's next on the boat" anytime, and stalled projects can't hide. This is where the ADHD impact gets visceral.

**Sprint 3 — Work bridge (half-week).** Either (a) wire up the `.ics` feed from step zero into the morning brief, or (b) ship #11 (EOD voice dump → overnight structuring into shadow calendar via the personal GCal MCP). After this sprint, your work day participates in tomorrow's brief — either because Outlook was kind enough to give you an `.ics`, or because *you* told the system the night before.

**Later (un-sprinted):** #7 (call script tool) when you next dread a call; #6 (web-form filler) and #8 (autonomous calls) only after #7 proves itself. #9 (Friday digest) is a nice-to-have whenever.

## Confirmed assumptions and remaining decisions

**Confirmed (your answers):**
- You work from a home office (rarely in corp office), but Jarvis is portable and travels with you. → Mid-day captures viable; mobile contexts (in the car, at the corp office on visits, errands) are also voice-reachable. Jarvis runs on battery when away from the desk.
- Apple ecosystem (iPhone + iPad). → Pushover for push notifications, Apple Calendar as personal calendar layer, iMessage for SMS-equivalent in option #7, native Shortcuts as fallback.
- Reminders OK with priority tiers. → Three-tier system designed in "Notification priority tiers" above.
- Existing Obsidian structure lives in the 2ndBrain repo. → Sprint 0 reduces to a gap-analysis pass against what's already there, not a from-scratch scaffold; see "Wiki organization" section.
- Personal Gmail/GDrive/GCal MCPs already authenticated. Work is Outlook (walled). → Options #4 and #5 are ~1 hour each; work bridge needs sprint 3.

**Still open:**
- **Can your work Outlook publish a `.ics` URL?** Five-minute check on Outlook web settings. Determines sprint 3 path.
- **How chatty is too chatty?** The priority tiers give us the dial, but you'll have to tune medium-vs-low for yourself over the first week or two of use. Plan: start conservative (most things → low), promote to medium when you wish you'd been told.
- **Bland.ai appetite.** Defer until #7 has run a few cycles. Names the question for later.
- **Privacy ack.** Wiring Calendar/Gmail MCPs into oc-personal-runner means Claude (via Anthropic API) sees more of your data. You already accepted this for the wiki; naming it again because the volume goes up significantly with email.

## Verification (what "this is working" looks like)

For each thing you decide to build, the cheap test:

- **Reverse channel works:** lobsterboy → MQTT publish → Jarvis speaks within ~3 sec, doesn't interrupt active voice loops, no false positives.
- **Morning brief works:** you start the day with a clear ONE thing to focus on. You feel less paralyzed at the morning desk-arrival moment. Subjective; track it for a week.
- **Project next-action works:** the dreaded "what was I doing on the boat?" question goes from 5 minutes of digging to a one-sentence answer.
- **Call script works:** you make a call you'd been avoiding for a week or more. Score in your `wiki/personal/health-care-log.md` or wherever.
- **Calendar push works:** you stop showing up late or missing events. Hardest to measure; rolling 2-week comparison.

The honest measure: if after a month you find yourself talking to Jarvis without thinking about it, and projects move forward without you remembering to push them — it's working. If you're still opening Notion or Things or whatever you used before, the friction isn't low enough yet.
