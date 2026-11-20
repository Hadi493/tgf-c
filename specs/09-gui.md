# 09 — GUI

Status: draft
Depends on: 00-architecture, 01-runtime-and-auth, 02-polling-and-dedupe, 03-forwarding, 04-sequential-backfill, 05-config-and-storage, 06-tui, 07-build-and-platform
Design reference: the "TGF GUI" canvas (Dashboard, Sources, Settings, Sign-in artboards)

> Written without access to specs 00-08. Every reference to an existing module, struct or
> behaviour below is inferred from the README and file names. Section 12 lists what must be
> reconciled before implementation starts.

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

### 3.4 Command API (UI → core)

`SUBMIT_PHONE`, `SUBMIT_CODE`, `SUBMIT_PASSWORD`, `PAUSE`, `RESUME`,
`ADD_SOURCE(handle)`, `REMOVE_SOURCE(id)`, `SET_SOURCE_ENABLED(id, bool)`,
`RESCAN_SOURCE(id)`, `APPLY_CONFIG(config)`, `LOGOUT`, `SHUTDOWN`.

## 4. Data model on the UI side

- GUI-020: Sources, queue and log are `GListStore`s of small GObject wrappers, bound to
  `GtkListView` with a `GtkSignalListItemFactory`. Rows update in place on `SOURCE_STATE`;
  the list is not rebuilt.
- GUI-021: The log store is a bounded ring. Default cap 2000 lines; oldest entries drop
  first. Memory use MUST NOT grow with uptime.
- GUI-022: The queue view shows at most the next 5 items; the total comes from the event.
- GUI-023: Counters (forwarded today, skipped, queued) are derived from events in the core
  and sent as totals, not recomputed in the UI by counting log lines.

## 5. Screens

Layout follows the design canvas: fixed sidebar (Dashboard, Sources, Settings, Session) plus
a content area, as an `AdwNavigationSplitView` or equivalent. Default window 1280×800,
minimum 960×640.

### 5.1 Dashboard

- GUI-100: Header shows the pipeline state (Forwarding / Paused) and a single Pause/Resume
  button, 44px minimum height.
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

### 5.4 Sign-in

Shown on first run, or whenever the core reports an auth state that needs input.

- GUI-130: Steps map one-to-one to TDLib authorization states (section 7). The UI never
  advances on its own; it follows `AUTH_STATE` events.
- GUI-131: The password field is a password entry. The password is passed to the core and
  zeroed in UI memory straight after submit (GUI-061).
- GUI-132: Wrong code or password shows the error on the same step and keeps the entered
  value cleared. It never silently restarts the flow.
- GUI-133: On success, show the session location (`tdlib_db/`) and a single action to open
  the dashboard.

## 6. Config mapping

The GUI reads and writes the existing `config.json` schema unchanged.

| Field | Widget | Validation | Default |
|---|---|---|---|
| `api_id` | Entry | positive integer | none (required) |
| `api_hash` | Password entry | 32 hex characters, non-empty | none (required) |
| `source_channels` | Sources screen | each `@name` or `t.me/name`, unique | `[]` |
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

## 11. Build and layout

- GUI-040: `./nob gui` builds `tgf-gui`. `./nob` with no arguments builds exactly what it
  builds today and MUST NOT require GTK.
- GUI-041: GTK flags come from `pkg-config --cflags --libs gtk4 libadwaita-1`. If
  `pkg-config` cannot find them, `./nob gui` fails with a message naming the Arch packages.
- GUI-042: Proposed source layout:

```
core/        libtgf: tdlib glue, forward, backfill, history, tracker, config
tui/         terminal frontend (event consumer)
gui/
  main.c         GtkApplication, lifecycle, core start/stop
  bridge.c       event queue, command queue, GSource wiring
  models.c       GObject wrappers + GListStore
  win_main.c     window, sidebar, navigation
  page_dashboard.c  page_sources.c  page_settings.c  page_signin.c
  tgf.gresource.xml / .ui   (optional: Blueprint or XML UI files)
```

- GUI-043: UI MAY be built in code or with `.ui` XML templates. Pick one and use it for all
  pages; do not mix.

## 12. Core changes this spec requires

The mockup shows behaviour that tgf does not obviously have today. Each item needs a decision
before the GUI that depends on it can be built.

| # | Needed capability | Used by | Notes |
|---|---|---|---|
| 1 | Library boundary and event/command API (3.2–3.4) | everything | Largest change. Do first. |
| 2 | Pause / resume of forwarding | Dashboard header | Core must stop pulling from the queue without losing state. |
| 3 | Per-source enable/disable | Sources switch | `config.json` has no per-source flag. Options: add `disabled_channels: []`, or allow `source_channels` entries to be objects. Preserve backward compatibility with the plain string array. |
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

## 14. Open questions

- Does `tgf.c` already separate the TDLib receive loop from forwarding logic, or is it
  single-threaded with a poll loop? That decides how many threads section 3.1 really has.
- Is `tdlib_db/` locked today? (GUI-074)
- Should disabled sources stay in `source_channels` (marked disabled) or move to a separate
  list? (section 12, item 3)
- Is the history file format stable enough to show a History view in a later version?
- Keep the TUI long-term, or deprecate it once the GUI covers headless use? (Headless servers
  still need the CLI, so the recommendation is to keep it.)
