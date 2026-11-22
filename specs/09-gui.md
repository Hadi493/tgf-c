# 09 — GUI

Status: draft (reconciled with spec 10)
Depends on: 00-architecture, 01-runtime-and-auth, 02-polling-and-dedupe, 03-forwarding, 04-sequential-backfill, 05-config-and-storage, 06-tui, 07-build-and-platform, 10-core-api
Design reference: the "TGF GUI" canvas (`specs/TGF GUI.html`: Dashboard, Sources, Settings, Sign-in artboards)

> Reconciled with specs 00–08 and 10-core-api. Module layout reflects the `src/` split
> (no `tgf.c`). The process threading model adopts the 1-worker-thread architecture
> specified in spec 10 §6. Requirement IDs (`GUI-nnn`) remain stable.

Keywords MUST, SHOULD and MAY are used in the RFC 2119 sense. Requirement IDs (`GUI-nnn`)
are stable so `acceptance.md` can reference them.

---

## 1. Purpose and scope

A desktop frontend for tgf. It replaces the terminal dashboard and hand-editing of
`config.json` with a window, without changing what tgf does.

In scope:
- Live monitoring of the forwarding pipeline (what the TUI shows today).
- Managing source channels and the destination.
- Editing every field of `config.json`.
- First-run Telegram sign-in (phone, code, 2FA).

Non-goals (v1):
- Browsing or reading Telegram content. The GUI never renders message bodies or media.
- Multiple accounts or multiple config profiles.
- System tray, notifications, autostart. (niri has no tray by default; do not depend on one.)
- Live hot-reload of config into a running pipeline (see GUI-052).
- Windows and macOS. Target is Linux, Wayland first.
- Replacing the CLI. `./tgf` and `./tgf -d` MUST behave exactly as before.

## 2. Toolkit decision

**GTK4 + libadwaita**, used through their C API.

| Requirement | Why GTK4/libadwaita fits |
|---|---|
| Project is C, no C++ | Native GObject C API. No binding layer. |
| Wayland-native | First-class Wayland client, fractional scaling, client-side decorations. |
| Dashboard widgets | `GtkListView` (sources, queue, log), `GtkSwitch`, `AdwEntryRow`, `AdwSpinRow`, `AdwViewStack`/sidebar. |
| TDLib has its own thread | `g_idle_add` / `GAsyncQueue` give a clean worker-to-UI handoff. |

Accepted costs:
- GTK is a system dependency, so the GUI binary is **not** self-contained like `tgf`.
  Mitigation: the GUI is an optional build target (GUI-040) and the CLI stays dependency-free.
- Larger binary and startup than the CLI.

Rejected alternatives: Dear ImGui via cimgui (C++ underneath, immediate-mode text input is
weak), Qt (C++), raylib/raygui and Clay (pure C and light, but text input, list virtualisation
and accessibility would have to be built by hand; revisit if GTK becomes a burden).

Minimum versions: target the current Arch packages for `gtk4` and `libadwaita`. Pin the exact
minimums in 07-build-and-platform once verified, and do not use APIs newer than those.

## 3. Architecture

### 3.1 Process and thread model

One process, three kinds of thread:

1. **UI thread** runs the GTK main loop. It is the only thread allowed to call GTK/GLib UI API.
2. **TDLib receive thread** (owned by the core) runs the `td_receive` loop.
3. **Core worker** runs forwarding logic, dedupe, backfill and the delay timer, if separate
   from the receive thread in the existing design.

- GUI-001: Code in the core MUST NOT include `gtk/gtk.h` or call any GTK function.
- GUI-002: The UI thread MUST NOT call TDLib and MUST NOT block on core work. It sends
  commands and receives events.
- GUI-003: All core-to-UI communication goes through one thread-safe event queue drained on
  the UI thread (a `GSource` or `g_idle_add` wake-up). Events are heap-allocated by the core
  and freed by the UI after dispatch.
- GUI-004: All UI-to-core communication is a command enum plus payload pushed on a
  thread-safe command queue. Commands are fire-and-forget; results come back as events.

### 3.2 Core / frontend split

The existing code is built around `tgf.c`, `forward.c`, `history.c` and `tracker.c`. To share
logic between TUI and GUI, the pipeline moves behind a small library boundary (proposed
names, to be reconciled with 00-architecture):

```
libtgf (core)         tgf (CLI + TUI)       tgf-gui (GTK4)
  tdlib glue            main(), -d flag       GtkApplication
  forward / backfill    TUI renderer          event queue -> GListStore models
  history / tracker     consumes events       sends commands
  config load/save
  event + command API
```

- GUI-010: The TUI MUST be reimplemented as a consumer of the same event API. It MUST NOT
  keep a private data path to the core. (This guarantees both frontends show the same truth.)
- GUI-011: The core exposes events as plain C structs with no GLib types, so the TUI does not
  need GLib.
- GUI-012: No global mutable state shared between core and frontends except through the two
  queues.

### 3.3 Event API (core → UI)

| Event | Payload | Drives |
|---|---|---|
| `AUTH_STATE` | TDLib auth state, optional error | Sign-in screen |
| `CONNECTION_STATE` | connecting / updating / ready / lost | Sidebar status card |
| `SOURCE_STATE` | source id, state (live, scanning, no_access, disabled), last activity time, backfill progress 0..100, last seen message id | Sources table, detail panel |
| `FORWARDED` | source, message id, kind (text, album×N, reply+parent), dest | Log, counters |
| `SKIPPED` | source, message id, reason (duplicate, outside_window) | Log, counters |
| `QUEUE` | ordered list of next N items plus total length | "Up next", queue stat |
| `NEXT_FORWARD_IN` | milliseconds | Countdown stat |
| `PIPELINE_STATE` | running / paused | Header status pill |
| `LOG` | level, timestamp, text | Live log |
| `ERROR` | scope (source / auth / pipeline), code, text | Banners, row states |

The frontend bridge first applies an event to its UI state, then delivers that same borrowed
event to page controllers, and only then frees its payload. The main window owns the page
controllers for its lifetime; page constructors initialise caller-provided controllers rather
than returning independently allocated state.

### 3.4 Command API (UI → core)

`SUBMIT_PHONE`, `SUBMIT_CODE`, `SUBMIT_PASSWORD`, `PAUSE`, `RESUME`,
`ADD_SOURCE(handle)`, `REMOVE_SOURCE(handle)`, `SET_SOURCE_ENABLED(handle, bool)`,
`RESCAN_SOURCE(handle)`, `APPLY_CONFIG(config)`, `LOGOUT`, `SHUTDOWN`.

Source commands are handle-addressed because a source disabled in `config.json` is never
resolved and has no chat id. The core normalizes `@name`, `name` and `t.me/name`.

## 4. Data model on the UI side

- GUI-020: Sources, queue and log are `GListStore`s of small GObject wrappers, bound to
  `GtkListView` with a `GtkSignalListItemFactory`. Rows update in place on `SOURCE_STATE`;
  the list is not rebuilt.
- GUI-021: The log store is a bounded ring. Default cap 2000 lines; oldest entries drop
  first. Memory use MUST NOT grow with uptime.
- GUI-022: The queue view shows at most the next 5 items; the total comes from the event.
- GUI-023: Counters (forwarded today, skipped, queued) are derived from events in the core
  and sent as totals, not recomputed in the UI by counting log lines.
- GUI-024: Page controllers are owned by the main window. An event is dispatched after the
  bridge has updated its state and before the bridge releases the event payload.

## 5. Screens

Layout follows the design canvas: fixed sidebar (Dashboard, Sources, Settings, Session) plus
a content area, as an `AdwNavigationSplitView` or equivalent. Default window 1280×800,
minimum 960×640.

### 5.1 Dashboard

- GUI-100: Header shows the pipeline state (Forwarding / Paused) and a single Pause/Resume
  button, 40 px high per the prototype (GUI-036).
- GUI-101: Four stat cards: forwarded today, in queue, skipped as duplicates, next forward in.
  The countdown shows `paused` while paused.
- GUI-102: Sources table with columns channel, status, backfill progress, last message.
  Status is conveyed by text and a dot, never by colour alone.
- GUI-103: "Up next" lists the next queue items in forwarding order and labels the order
  (old → new or new → old) from `sequence_direction`.
- GUI-104: Live log is monospace, newest first, with filters All / Forwarded / Skipped /
  Warnings. Filtering is client-side over the ring buffer.
- GUI-105: Clicking a source row navigates to Sources with that row selected.

### 5.2 Sources

- GUI-110: Destination field is shown at the top. `@me` is explained inline as Saved Messages.
- GUI-111: Add source: one entry accepting `@name` or `t.me/name`. The core resolves it
  through TDLib; the UI shows a pending row until `SOURCE_STATE` arrives, and an inline error
  if resolution fails. Duplicates MUST be rejected.
- GUI-112: Each row has a select action and an enable switch. The switch is a real switch
  control with an accessible name that includes the channel.
- GUI-113: Detail panel for the selected source: last seen message id, forwarded count,
  skipped-as-duplicate count, history window, last activity, and a context note.
- GUI-114: "Rescan from window start" clears that source's backfill cursor and rescans.
  It MUST ask for confirmation, because it can re-queue many messages (dedupe still prevents
  re-sends of already-forwarded ones).
- GUI-115: "Remove source" removes it from config and stops scanning. It MUST confirm and MUST
  NOT delete anything from `history.txt`.
- GUI-116: A source the account cannot read shows "No access" with the guidance to join the
  channel in Telegram, then rescan.

### 5.3 Settings

One control per config field. See section 6 for the exact mapping. The right-hand pane shows a
live preview of the file that would be written.

- GUI-120: Controls are bound to a working copy of the config. Nothing touches disk until
  "Save to config.json".
- GUI-121: "Revert" restores the working copy from disk.
- GUI-122: The Order control is disabled (and visibly dimmed) when sequential forwarding is off.
- GUI-123: The preview MUST mask `api_hash` and MUST NOT show the real value anywhere in the
  UI.
- GUI-124: Invalid input blocks Save and shows the reason next to the field.

### 5.4 Session / Sign-in

Accessible at all times via "Session" in the sidebar.

- GUI-130: When unauthorized or on first run, shows the step-by-step sign-in wizard following
  `AUTH_STATE` events (phone, code, 2FA password).
- GUI-131: When authenticated, displays current session status (`tdlib_db/` location, connection
  state, and a "Log out" action requiring confirmation that informs the user `tdlib_db/` will be cleared).
- GUI-132: The password field is a password entry. The password is passed to the core and
  zeroed in UI memory straight after submit (GUI-061).
- GUI-133: Wrong code or password shows the error on the same step and keeps the entered
  value cleared. It never silently restarts the flow.
- GUI-134: On successful authentication, automatically switches the view to Dashboard or provides
  an "Open Dashboard" button.

## 6. Config mapping

The GUI reads and writes the existing `config.json` schema unchanged.

| Field | Widget | Validation | Default |
|---|---|---|---|
| `api_id` | Entry | positive integer | none (required) |
| `api_hash` | Password entry | 32 hex characters, non-empty | none (required) |
| `source_channels` | Sources screen | each `@name` or `t.me/name`, unique | `[]` |
| `disabled_channels` | Sources enable switch | array of `@name` / `t.me/name` matching `source_channels` | `[]` |
| `dest_channel` | Entry (Sources screen) | `@me` or `@name` | `@me` |
| `history_file` | Not exposed in v1 | path, kept as is | `history.txt` |
| `forward_delay_sec` | Spin row | integer 1..3600 | 1 |
| `enable_sequential_forwarding` | Switch | boolean | false |
| `sequence_direction` | Segmented control | `asc` or `desc` | `asc` |
| `history_window_hours` | Preset buttons 6 / 24 / 72 / All plus custom | integer ≥ 0, `0` = all | 24 |

- GUI-050: Unknown keys in `config.json` MUST be preserved on save.
- GUI-051: Save is atomic: write a temp file in the same directory, `fsync`, `rename` over
  the original. A crash mid-save MUST leave the old file intact.
- GUI-052: v1 applies config by restarting the pipeline inside the process
  (`APPLY_CONFIG`). Backfill cursors and history survive the restart (they are persisted
  already). Live hot-reload is out of scope.
- GUI-053: The GUI MUST work if `config.json` does not exist: open Settings in an empty
  state, create the file on first save.
- GUI-054: If `config.json` fails to parse, show the parse error and offer to open it in the
  user's editor. The GUI MUST NOT overwrite a file it could not parse.

## 7. Auth state machine

| TDLib state | Screen state |
|---|---|
| `authorizationStateWaitTdlibParameters` | Handled by core, no UI |
| `authorizationStateWaitPhoneNumber` | Step 1, phone |
| `authorizationStateWaitCode` | Step 2, code |
| `authorizationStateWaitPassword` | Step 3, 2FA password |
| `authorizationStateReady` | Success, then Dashboard |
| `authorizationStateLoggingOut` / `Closed` | Return to Step 1 |

- GUI-060: If a cached session exists, the app starts directly on the Dashboard and the
  sign-in screen never appears.
- GUI-061: Phone number, code and password are never written to logs, the log view, config,
  or any file. Password buffers are zeroed after use.
- GUI-062: "Log out" lives under Session, requires confirmation, and tells the user that
  `tdlib_db/` will be cleared.

## 8. Error and edge states

- GUI-070: Connection lost shows a persistent banner and the sidebar card changes state. The
  pipeline state is not changed by the UI; the core decides.
- GUI-071: A rate-limit response from Telegram shows as a warning log line and in the header
  ("Rate limited, resuming in mm:ss"). The countdown comes from the core.
- GUI-072: Empty states: no sources (prompt to add one), empty log ("Waiting for messages"),
  empty queue ("Nothing queued").
- GUI-073: Closing the window while the pipeline runs MUST stop the core cleanly
  (`SHUTDOWN`, join threads, flush history) before exit. Hard-kill must not corrupt
  `history.txt` (see 02-polling-and-dedupe for the existing write guarantee).
- GUI-074: Only one instance may run against a given `tdlib_db/`. A second instance (CLI or
  GUI) MUST detect the lock and show a clear message.

## 9. Security and privacy

- GUI-080: Created or modified `config.json` has mode `0600`.
- GUI-081: `api_hash` is only ever shown masked. No "reveal" button in v1.
- GUI-082: The GUI makes no network connection of its own. All traffic is TDLib's.
- GUI-083: No telemetry, no update checks.

## 10. Platform, appearance, accessibility

- GUI-090: Application ID is a valid reverse-DNS id (proposed `io.github.hadi493.tgf`) and is
  set so niri window rules and `.desktop` matching work.
- GUI-091: Use libadwaita client-side decorations and follow the system colour scheme through
  `AdwStyleManager`. The mockup is dark; light must also work.
- GUI-092: Fonts: system UI font for text, system monospace for ids, handles, log and JSON.
- GUI-093: Every interactive element is reachable by keyboard with a visible focus ring.
  Suggested shortcuts: `Ctrl+1..4` switch pages, `Ctrl+S` save in Settings, `Space` toggles
  Pause/Resume on the Dashboard when no entry has focus.
- GUI-094: Icon-only buttons have accessible names. Status is never colour-only.
- GUI-095: Minimum contrast 4.5:1 for text. Respect the system reduced-motion setting.
- GUI-096: HiDPI: no hard-coded pixel sizes for icons; use symbolic icons and GTK units.
- GUI-097: The initial visual port uses the prototype's fixed left sidebar with product identity,
  page navigation and connection card. Dashboard content groups metrics, sources, queue and log
  into cards while using libadwaita theme colours so the same hierarchy works in light and dark
  system themes.
- GUI-098: The dashboard card hierarchy is ported from the prototype's Dashboard artboard as
  built by `src/gui/`. The sidebar shows `tgf`, `v1.3.1`, the four page buttons and the
  connection card. The dashboard header shows the page title, a `N sources →` button that
  navigates to Sources, the destination handle and the pipeline pill with the Pause/Resume
  button (GUI-100). Four cards use the prototype labels: `Forwarded today`, `In queue
  (sequential)`, `Skipped as duplicates`, `Next forward in`; the countdown shows `paused`
  while paused. The Sources card has the column headings Channel / Status / Backfill / Last
  message; status is a dot plus text, backfill is a percentage, and clicking a row navigates
  to Sources with that row selected (GUI-102, GUI-105). The `Up next` card labels the order
  `old → new` or `new → old` from `sequence_direction` and lists at most five items as
  `#n` + source + first message id, bounded by the core's five-item burst (GUI-022); its
  empty state is `Nothing queued`. The `Live log` card has client-side filter buttons
  All / Forwarded / Skipped / Warnings over the bounded ring, newest first and monospace,
  with the empty state `Waiting for messages` (GUI-104, GUI-072). Log lines are stamped with
  the UI-thread arrival time because core log events carry no timestamp. Forwarded, skipped
  and error events are mirrored into the ring with filter categories; counters still come
  from the bridge totals, never from counting log lines (GUI-023).
- GUI-099: A `TGF_EV_QUEUE` burst starts with a total-only event (`id == 0`) and is followed by
  at most five item events. The bridge clears its queue snapshot on the total event and
  appends at most five item events, so the view cannot accumulate stale items across ticks
  (GUI-022).
- GUI-035: The sidebar shows the product logo from `tgf.png` in the process CWD at 48 px,
  beside the `tgf` / `v1.3.1` wordmark, falling back to a symbolic image when the file is
  missing. This supersedes the prototype's 40 px at the user's request; the logo's blue
  (`#6cb6ff`) remains the application accent (GUI-037).
- GUI-036: The prototype's measurements are the layout contract and supersede the generic
  control heights in GUI-100/GUI-093: 232 px sidebar with 20 px / 14 px padding and 24 px
  section gap, 48 px logo beside the wordmark (GUI-035), 44 px nav items (9 px radius), 64 px page
  header, 24 px content padding, 20 px card gap, 12 px card radius, 92 px stat cards with
  28 px monospace values, 40 px Pause/Resume, 44 px buttons and inputs (9 px radius),
  48 px sign-in fields and a 460 px sign-in card, 80 px settings rows, 84 px destination
  card, 56 px source rows, 30 px log filter chips and 22 px queue kind chips.
- GUI-037: The prototype's literal palette and typefaces are used for the app surfaces:
  canvas `#14171b`, sidebar `#101316`, card `#1b1f24`, log card `#0f1215`, raised row
  `#181c21`, input `#14171b`, borders `#262b31` / `#343b43`, text `#e3e6e8`, secondary
  `#c4cacf`, muted `#9aa3ab`, log body `#d6dadd`, accent `#6cb6ff`, running `#7fd18b`
  on `#1c2b22`, paused/warning `#e5b567` on `#2e2619`, danger `#f08a8a`, and active nav
  `#232a31`. UI text uses Inter; ids, handles, log, JSON and numeric values use
  JetBrains Mono. This overrides the Adwaita-variable mapping in GUI-091/GUI-097; the
  layout and status-dot-plus-text rule remain unchanged.
- GUI-038: Visual defects found in the rendered pages are fixed without changing the
  artboard measurements. Settings descriptions wrap inside their 200 px label column
  instead of being ellipsized, the sequential switch's checked state uses the prototype
  blue with a dark knob, the four sidebar items carry symbolic icons beside their labels
  as in the prototype, and an `Idle` pipeline pill uses the neutral raised colour and
  secondary text instead of the running green.
- GUI-039: The Settings toggles use the prototype's geometry where GTK can express it:
  Order is a segmented group with 3 px padding/gap, 11 px group radius, 38 px inner
  buttons and 8 px button radius; History window keeps its separate 44 px chips with a
  64 px minimum width. The sequential switch keeps GTK's native 26 px-high track,
  coloured with the prototype blue and a dark knob, because forcing the mockup's 44 px
  track width makes the widget oval rather than pill-shaped.
- GUI-044: Buttons use natural widths from the prototype's padding instead of arbitrary
  fixed pixel widths: 18 px horizontal padding for Pause, Add source, Revert, Back and
  Log out, 22 px for Save to config.json, and 44/48 px control heights. The Sources
  detail actions Rescan and Remove are a vertical full-width pair with 10 px gap, and
  Session's Open dashboard is a full-width 48 px primary action. History window chips
  use 14 px horizontal padding so all four have the same 64 px width.
- GUI-046: An artboard-by-artboard comparison fixes the remaining visual differences.
  Dashboard's Sources card has an accent `Manage` link; source rows are 38 px high with a
  6 px backfill bar, a 12 px monospace percentage and a last-message column; the live log
  renders time, a fixed-width coloured tag and the message as separate spans. Settings
  rows have 1 px `#262b31` separators, a `#181c21` action bar with a top border, and a
  `#0f1215` preview card with a 46 px header; the delay value is 16 px. Session drops the
  extra page header so the 460 px card is centred in the main area, uses a 24 px title,
  16 px monospace entries and a bordered privacy note. The Sources artboard already
  matches its markup.
- GUI-047: The Dashboard `Manage` control is a flat 13 px accent-coloured text link with
  no button frame, background, border or padding, matching the prototype's `<a>`; hover
  lightens the accent with `color-mix()` and keyboard focus stays visible.
- GUI-050: The GUI defaults to GTK's cairo GSK renderer by setting `GSK_RENDERER=cairo`
  before the application is created, unless the user already set that variable. This avoids
  loading Mesa's software-GL/LLVM stack in headless or software-rendered sessions; measured
  resident memory of the GTK surface drops from ~211 MB to ~141 MB. The visual layout is
  unchanged because the UI uses only boxes, labels, lists and CSS colours.
- GUI-049: Backfill progress is a real core value, not a UI placeholder. The core emits
  `SOURCE_STATE.progress` whenever it changes for a resolved source: 100 when the backfill
  is complete, otherwise the percentage of the message-id span already scanned. When
  sequential forwarding is off no progress event is emitted, because those sources have no
  backfill. The Sources detail panel shows that value as a `Backfill` row, and
  the Dashboard source table continues to render it as a 6 px bar plus percentage (GUI-102,
  GUI-113). No storage format changes.
- GUI-048: The UI drains core events from a 50 ms timeout GSource rather than a continuously
  rescheduled idle callback, so an empty queue cannot spin the GTK main loop. Auth-scoped
  `ERROR` events are forwarded to the sign-in page as well as the dashboard: the current
  step shows the error and clears its field, restoring GUI-133 when no further `AUTH_STATE`
  follows the failure.
- GUI-045: The Sources table's header and its rows share one column layout. The
  `Status`, `Forwarded` and `On` headers sit exactly over the cells beneath them
  (identical x and width), and the header band and the row boxes start at the
  same left inset, so the table reads as four aligned columns. The enable switch
  in `On` keeps GTK's native track height instead of stretching to the 56 px row
  height: it is centred vertically and left-aligned in its cell, as GUI-039 does
  for the sequential switch. This restores the alignment promised by GUI-106 and
  the row height fixed by GUI-036.
- GUI-106: The Sources artboard is built as specified: a destination card explaining
  `@me` / `@name`, an add-source entry with a 44 px Add button, a Channel / Status /
  Forwarded / On table, and a detail panel for the selected source showing last-seen id,
  forwarded and skipped-as-duplicate counters, history window, last activity and a
  context note. Rescan and Remove each ask for confirmation; a source with `No access`
  shows the guidance to join the channel in Telegram and rescan (GUI-110..116).
- GUI-125: The Settings artboard is built as specified: every row has a label and a
  one-line description; delay uses 44 px −/+ buttons around a monospace value, the
  sequential switch is a real switch, Order is a two-way segmented control that is
  visibly disabled when sequential forwarding is off, and History window is a
  6 h / 24 h / 72 h / All preset group. The live preview is a monospace JSON card that
  masks `api_hash`, Revert restores the on-disk config, Save validates a positive
  `api_id` and a 32-character hex `api_hash`, and errors appear beside the field
  (GUI-120..124).
- GUI-135: The Session artboard is built as specified: a three-segment progress bar,
  labelled steps for phone, code and 2FA password, 48 px entries and buttons, Back and
  Next actions, a session-status card showing `tdlib_db/`, a confirmed Log out action,
  an `Open dashboard` action after success, and the TDLib privacy note. An auth error
  stays on the current step with its value cleared (GUI-130..134).

## 11. Build and layout

- GUI-040: `./nob gui` builds `tgf-gui`, and `./nob run-gui` builds and launches it. `./nob` with no arguments builds exactly what it
  builds today and MUST NOT require GTK.
- GUI-041: GTK flags come from `pkg-config --cflags --libs gtk4 libadwaita-1`. If
  `pkg-config` cannot find them, `./nob gui` fails with a message naming the Arch packages.
- GUI-042: Source layout:

```
src/gui/
  main.c            GtkApplication, lifecycle, core start/stop (or mock)
  bridge.c          event queue, command queue, GSource wiring
  models.c          GObject wrappers + GListStore
  win_main.c        window, sidebar, navigation split view
  page_dashboard.c  dashboard page, metric cards, queue list, live log
  page_sources.c    sources list, enable switches, detail panel
  page_settings.c   settings rows, live JSON preview, revert/save
  page_signin.c     step-by-step auth wizard (phone, code, 2FA)
```

- GUI-043: UI MUST be constructed programmatically in pure C code using GTK4 and libadwaita
  widget APIs (no XML `.ui` templates or `.gresource.xml`). This preserves single-step
  compilation in `nob.c` and compile-time type verification.

## 12. Core changes this spec requires

The mockup shows behaviour that tgf does not obviously have today. Each item needs a decision
before the GUI that depends on it can be built.

| # | Needed capability | Used by | Notes |
|---|---|---|---|
| 1 | Library boundary and event/command API (3.2–3.4) | everything | Largest change. Do first. |
| 2 | Pause / resume of forwarding | Dashboard header | Core must stop pulling from the queue without losing state. |
| 3 | Per-source enable/disable | Sources switch | Persisted via optional `"disabled_channels": ["@channel"]` array in `config.json` (preserves backward compatibility with string array in `source_channels`). |
| 4 | Per-source counters and last-seen id | Sources detail | Must be persisted by the tracker, or reset on restart and labelled "since start". |
| 5 | Per-source rescan | Sources detail | Clear one cursor in the backfill state file. |
| 6 | Resolve-and-validate a channel on add | Add source | TDLib public-chat lookup. |
| 7 | Rate-limit countdown exposed as an event | Header | If TDLib wait time is already handled internally, surface it. |
| 8 | Instance lock on `tdlib_db/` | GUI-074 | May already exist. |

## 13. Acceptance criteria

Add to `acceptance.md`:

1. `./nob` builds without GTK installed; `./tgf` and `./tgf -d` behave as before. (GUI-040)
2. Fresh checkout, no config: GUI opens Settings, saving creates a valid `config.json` with
   mode 0600. (GUI-053, GUI-080)
3. Fresh session: sign-in completes through phone, code and 2FA, and a restart skips sign-in.
   (GUI-130, GUI-060)
4. A wrong code keeps the user on step 2 with an error. (GUI-132)
5. Same config and same sources produce the same forwarded set and order in GUI and CLI
   runs. (GUI-010)
6. Run for 8 hours with a busy source: log memory stays bounded and the UI stays responsive
   (no frame stalls caused by core work). (GUI-021, GUI-002)
7. Kill the process during a save: `config.json` is either the old or the new file, never
   truncated. (GUI-051)
8. Kill the process mid-forward: on restart nothing is forwarded twice. (GUI-073)
9. Edit a field, press Revert: UI matches disk. Press Save: disk matches the preview.
   (GUI-120, GUI-121)
10. Keyboard-only: sign in, add a source, change the delay, save, pause. (GUI-093)
11. `api_hash`, phone, code and password appear in no log, no preview and no file other than
    where the existing code already stores them. (GUI-061, GUI-123)
12. A second instance against the same `tdlib_db/` shows a clear error and does not start.
    (GUI-074)

## 14. Reconciled questions & remaining decisions

- **Threading model**: Resolved in spec 10 §6. There is no `tgf.c`; the core runs as **one worker thread** executing the tick loop (`src/loop.c`), with thread-safe event and command queues communicating with the GTK UI thread.
- **Instance lock on `tdlib_db/`** (GUI-074): No app-level lock exists today; TDLib locks `db.sqlite` internally. An explicit PID/lockfile mechanism is scheduled for spec 10 Phase 5.
- **Disabled sources**: Resolved. `disabled_channels: []` in `config.json` (05 §2, §12 item 3).
  Disabled entries stay in `source_channels`, are skipped during resolution and polling, and
  are toggled by handle through the core API (spec 10 §5).
- **History file format**: Format is stable (spec 05 §3), but v1 GUI does not expose history browsing.
- **TUI retention**: TUI is retained permanently (spec 10 Phase 4); it becomes an event consumer sharing `libtgf.a` with the GUI.

## 15. Amendment log

| Date | Change |
|---|---|
| 2026-11-20 | Draft created from UI canvas reference. |
| 2026-11-21 | Reconciled with specs 00–08 and 10-core-api: resolved threading model, noted module split, updated open questions. |
| 2026-11-21 | Resolved the disabled-source format (`disabled_channels`); source commands are handle-addressed so startup-disabled sources can be enabled. |
| 2026-11-21 | Implementation pass: `src/gui/` now builds as `tgf-gui` (`./nob gui`, `./nob run-gui`) and is verified headless with Xvfb against a scratch config. Deviations registered in 08 as TD-30..TD-33 (list rebuild, log cap, instance lock, `GtkEntry` instead of `AdwEntryRow`). |
| 2026-11-22 | Clarified GUI event lifetime and page-controller ownership (GUI-024) before repairing the prototype's event bridge. |
| 2026-11-22 | Added GUI-097 to record the first native port of the supplied HTML prototype's sidebar and dashboard card hierarchy. |
| 2026-11-22 | Specified the as-built Dashboard port from the prototype's Dashboard artboard (GUI-098) and the queue-burst reset (GUI-099) before implementing them. |
| 2026-11-22 | Added GUI-035/GUI-036 (logo and measured theme-aware styling) and GUI-106/GUI-125/GUI-135 (remaining three artboards) before completing the GUI. |
| 2026-11-22 | Replaced the Adwaita colour mapping with the prototype's literal palette and Inter/JetBrains Mono (GUI-037); corrected logo/nav/pause/sign-in measurements to the decoded artboards. |
| 2026-11-22 | Added GUI-038 for rendered defects: wrapped settings descriptions, blue checked switch, symbolic nav icons and a neutral Idle pill. |
| 2026-11-22 | Added GUI-039 for the prototype's exact switch and segmented-toggle geometry before resizing those controls. |
| 2026-11-22 | Added GUI-044 to replace arbitrary fixed button widths with the prototype's padded auto-width and full-width action layout. |
| 2026-11-22 | Added GUI-045 for the Sources table defect: headers misaligned with their cells and the enable switch stretched to an oval by the 56 px row. |
| 2026-11-22 | Added GUI-046 for the remaining Dashboard/Settings/Session artboard differences before the one-by-one fix pass. |
| 2026-11-22 | Added GUI-047 to make the Dashboard Manage control a flat text link like the prototype's anchor. |
| 2026-11-22 | GUI-035 logo raised to 48 px at the user's request; added GUI-048 for the event-dispatch busy loop and missing auth-error delivery. |
| 2026-11-22 | Added GUI-049 to emit real backfill progress from the core and show it in the Sources detail panel. |
| 2026-11-22 | Added GUI-050 to default to the cairo GSK renderer and avoid the software-GL memory cost. |
