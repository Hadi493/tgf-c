# 06 — CLI, Status Output, Dashboard

Status: baseline · Owner: `src/ui.c`, `src/main.c` · Depends on: 00, 01

## 1. argv behavior

`argv` is validated at startup (`src/main.c:25-28`): `argc` must be 1, or 2
with `argv[1] == "-d"`. Anything else prints `Usage: <argv0> [-d]` to stderr
and exits **1** (TD-14 addressed).

| Invocation | Dashboard | Debug lines | Status output |
|---|---|---|---|
| `./tgf` | yes, after full resolution | no | stored for dashboard, else stderr |
| `./tgf -d` | no | yes | stderr |
| anything else | — | — | `Usage: … [-d]` to stderr, exit 1 |

`ui_init` (`src/main.c:31`) selects the ncurses sink when `argv[1] == NULL`,
else the stderr sink; debug is exactly `-d`.

There is no `--help` and no config-path override. Unknown arguments are
rejected with a usage line (TD-14 addressed).

## 2. Output channels

Both stdout and stderr are unbuffered (`setvbuf(..., _IONBF)`,
`src/main.c:29-30`).

| Message | Stream |
|---|---|
| Auth prompts (`Phone`, `Code`, `2FA password`) | stdout |
| `✓ Authorized!` | stdout |
| Debug resolution progress | stdout |
| Debug `FORWARDED` lines | stdout |
| ncurses dashboard | stdout via ncurses |
| Status/error lines when no dashboard is active | stderr |
| Fatal startup errors | stderr |

### 2.1 Status channel

`ui_status(msg)` (`src/ui.c:104-108`):

- copies into the `Runtime` status slot `rt.status_msg[256]` with `snprintf`
  (truncation possible);
- if `dashboard_active` is false, also prints `msg\n` to stderr.

`dashboard_active` becomes true only when ncurses initializes, which happens
after `authorized` in dashboard mode. Consequences:

- Auth and early startup errors go to stderr.
- After the dashboard starts, status messages are **not** printed to stderr;
  they sit in the single slot until the next refresh.
- `print_dashboard` writes the slot then clears it (`src/ui.c:64-67`). A message
  lives at most ~1 refresh interval.
- If several status messages occur between refreshes, only the **last** is
  shown. Errors during resolution are therefore easy to miss: the screen
  flashes one line for under a second, then returns to `Status ... ACTIVE`.
- In dashboard mode there is no persistent error log. In `-d` mode every
  `ui_status` line is preserved on stderr.

## 3. Dashboard

### 3.1 Lifecycle

Initialized on the first loop iteration where `argv[1] == NULL && authorized
&& dest_resolved && source_count >= 1 && !resolve_active`
(`src/loop.c:86-97`). The full-resolution gate (Step 4.1) ensures no
dashboard starts on any fatal resolution path:

```
initscr(); cbreak(); noecho(); nodelay(stdscr, TRUE); curs_set(0);
dashboard_active = true;
atexit(ncurses_cleanup);   // endwin()
```

Refresh at most once per second: `ui_dashboard_draw` (`src/ui.c:91-98`) gates
redraws with a `time(NULL)` delta. Each redraw calls `clear()` and repaints the
whole screen (`print_dashboard`, `src/ui.c:32-51`). Events still update dashboard
state as they arrive (`src/main.c:94`); several events in one tick are coalesced
into the next 1 Hz redraw.

`TGF_EV_SOURCE_STATE` is the one event that must reach the dashboard *before*
the dashboard opens. Resolution emits it while `dash_begun` is still false
(`src/resolve.c:21-32,115,123`), ahead of the `TGF_PIPE_RUNNING` event that
opens ncurses (`src/resolve.c:70-76`). `main.c` therefore forwards
`SOURCE_STATE` to `ui_dashboard_event` unconditionally, not gated on
`dash_begun`; all other kinds stay gated so early `TGF_EV_LOG` lines still go
to stderr only (§2.1). `ui_dashboard_event` only writes module-static state and
makes no curses calls, so pre-`begin` delivery is safe. Without this, the final
source count is drained and dropped and `Source Channels` stays `0`.

`print_dashboard` runs only after destination and source resolution complete;
during resolution the process is in plain stderr mode even with no argv.

### 3.2 Layout

Rows are written sequentially from the top:

```
           -->TGF - TELEGRAM FEED MONITOR-->
       ---------------------------------------------
       Target Channel ........ <dest_channel>
       Source Channels ....... <source_count>
       Forward Delay ......... <forward_delay_sec>sec
       Status ................ ACTIVE
       ---------------------------------------------
       FORWARDED from <src_name> -> <dest_channel>
       ---------------------------------------------
       <status_msg>            (only when non-empty)
       [TGF] Running (PID: <pid>)
       [TGF] Stop with: Ctrl+C | kill <pid> | pkill tgf
```

Observations:

- `Status` is the literal string `ACTIVE`; it does not reflect resolution,
  queue, or error state.
- `Source Channels` is `source_count` (successful resolutions), not
  `num_sources`; the value rides in the final `TGF_EV_SOURCE_STATE` emitted
  during resolution and is captured before the dashboard opens (§3.1).
- `FORWARDED from` is the **last source whose forward response succeeded**
  (`src_name`, default `"unknown"`), not a live per-message log
  (`src/forward.c:237-249`). `update_action_msg` (`src/ui.c:73-77`) is called immediately
  after each full redraw (`src/ui.c:83-86`), so it is currently redundant: the
  line changes only on the next 1 Hz refresh, not between refreshes.
- The status row disappears one refresh after it is printed.
- No terminal-size handling, no scrolling, no colors, no key input (nodelay is
  consumed by nothing).
- Long status text or a narrow terminal can wrap over subsequent rows.

### 3.3 Terminal requirements

ncurses requires a valid `TERM`. With `TERM` unset or unknown, `initscr()` may
print an error and terminate the process; TGF has no fallback and does not
check the return value. `TGF_NOGUI` at compile time removes the entire
dashboard and links no ncurses (07).

## 4. Debug mode

Enabled only by exactly `argv[1] == "-d"`.

| Event | Output |
|---|---|
| Source resolved | `\r  Resolving channels... <n>/<total>\n` (stdout) |
| Forward response success | ` FORWARDED from <src> → <dest>\n` (stdout, UTF-8 arrow) |
| Any `ui_status` line | stderr, one line each |

Messages themselves are never printed; only counts, cutoff, and source names.
`Backfill:`/`New:`/`History pruned` lines come from `ui_status` and are visible
in both modes (stderr without a dashboard).

## 5. Exit codes and signals

| Path | Code |
|---|---|
| SIGINT/SIGTERM → loop exit | 0 |
| Config load failure | 1 |
| `td_json_client_create` failure | 1 |
| `source_chat_ids` allocation failure | 1 |
| Destination resolution failure | 1 |
| All sources failed / empty source array | 1 |
| ncurses `initscr` failure | determined by ncurses (not handled) |

Signal handling only sets `keep_running`; shutdown latency is up to one receive
timeout (~1 s), plus any blocking auth prompt (01 §2.4).

## 6. Edge cases

| Case | Behavior |
|---|---|
| `./tgf -d` while unauthenticated | Prompts on stdout, errors on stderr, no dashboard |
| `./tgf` while unauthenticated | Same, dashboard starts only after Ready |
| Destination fails to resolve | `ERROR: Failed to resolve destination ...`, exit 1 (no dashboard) |
| No `TERM` in environment | ncurses error and process exit |
| Terminal resized | Layout not adjusted |
| Two errors in one second | Only the second is visible |
| Several events in one tick | State updates immediately; one redraw on the next 1 Hz tick |
| `SOURCE_STATE` before dashboard opens | Count captured pre-`begin`; first draw shows it instead of `0` |
| `./tgf foo` | No dashboard, no debug output, status to stderr |
| PID in the hint | Actual PID; `kill`/`pkill` commands are literal text |

## 7. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-06-01 | `./tgf` with a cached session | Dashboard appears; `Target Channel` matches config |
| SC-06-02 | `./tgf -d` | No dashboard; debug stdout + status stderr |
| SC-06-03 | `./tgf foo` | `Usage: … [-d]` to stderr, exit 1 |
| SC-06-04 | Forward one message in dashboard mode | `FORWARDED` source updates within ~1 s |
| SC-06-05 | Misconfigured destination | `ERROR: Failed to resolve destination ...`, exit 1 |
| SC-06-06 | Send SIGINT | Terminal restored, shell prompt returns, exit 0 |
| SC-06-07 | `env -u TERM ./tgf` | ncurses startup failure |
| SC-06-08 | Leave the dashboard running with no traffic | Full-screen redraw happens at most once per second; no continuous flicker |
| SC-06-09 | `./tgf` with N sources resolving successfully | First dashboard draw shows `Source Channels ....... N`, not `0` |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c` citations to the working-tree `Runtime` refactor |
| 2026-11-17 | Step 2: moved UI into `src/ui.c`; refreshed citations |
| 2026-11-17 | Step 4.1: Destination resolution failure now fatal (exit 1); all-sources-failed and empty-sources now fatal (exit 1); dashboard never starts on resolution failure |
| 2026-11-19 | Step 4.1 restoration: dashboard init gated on full resolution (no dashboard on fatal paths); ui_init sink selection documented; refreshed citations |
| 2026-11-19 | Step 4.6: argv validated (only no-arg or -d; else usage + exit 1); TD-14 addressed; SC-06-03 updated |
| 2026-11-22 | Restored the §3.1 1 Hz redraw throttle lost in the event-driven rewrite: `ui_dashboard_draw` now gates full redraws by `time(NULL)`; events still update state immediately. Added edge case and SC-06-08 |
| 2026-11-22 | Fixed `Source Channels` stuck at `0`: `main.c` now forwards `TGF_EV_SOURCE_STATE` to the dashboard before `dash_begun`, capturing the count emitted during resolution. Updated §3.1/§3.2, edge case, added SC-06-09 |
