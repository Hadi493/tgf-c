# 10 — Core API (library boundary for TUI + GUI)

Status: **draft / proposal (Phase 0)** · Owner: core · Depends on: 00, 01, 02, 03, 04, 05, 06-tui, 07, 08, 09
Baseline code: commit `b1f5b65` (master). Sections 2–4 are **as-built** (descriptive,
evidence-backed). Sections 5–9 are **proposed** and require approval before any code
changes. This file is the Phase 0 deliverable of the "real core API" task; no code has
been changed to produce it.

> The task prompt that commissioned this work describes a codebase that no longer exists
> (a monolithic `tgf.c` with ~30 `extern` globals). Section 2 reports every place the
> prompt and the draft GUI spec (09) contradict the actual tree. Per AGENTS.md these are
> reported, not silently "fixed".

---

## 1. Goal

Turn the forwarding pipeline into a small, thread-safe **core library** (`libtgf.a`) with
an opaque context, a lifecycle, an event queue (core→frontend) and a command queue
(frontend→core). The CLI and the ncurses dashboard become ordinary consumers. A GTK GUI
(spec 09) is a later, separate task and is **not** implemented here.

Hard invariants (from the task): no observable behaviour change to `./tgf` / `./tgf -d`
(same messages, order, dedupe, delay, window, sequential/backfill, restart); on-disk
formats stay compatible; C11 only; no GUI toolkit or new dependency in the core; secrets
(`api_hash`, phone, code, 2FA password) never reach logs/events/errors; no `exit()`/
`abort()`/`printf()` inside the library.

---

## 2. Prompt / spec vs repository — discrepancies (REPORT, do not auto-fix)

| # | Prompt or spec 09 claims | Reality at `b1f5b65` | Evidence |
|---|---|---|---|
| D1 | "`tgf.h` declares ~30 `extern` globals (`api_id`, `api_hash`, `source_channels`, `fwd_queue`, `seq_tracker[]`, `pending_req`, `keep_running`, `status_msg[256]`)" | **Zero `extern` globals.** `grep -rn extern src/` is empty. All state lives in one `Runtime` struct, held as `static Runtime rt` in `main.c`. `pending_req` was removed in Step 3. | `src/tgf.h:123-151`, `src/main.c:8-13`; 08 TD-10 already notes "the current header has no `extern`s" |
| D2 | "no context struct, no init or shutdown" | `Runtime` **is** the context struct; `main()` performs init and shutdown inline. What is missing is a *reusable, opaque, thread-safe API* around it — not the struct. | `src/main.c:24-70` |
| D3 | "Read `tgf.c`" / 09 §3.2 "existing code is built around `tgf.c`, `forward.c`, `history.c`, `tracker.c`" | No `tgf.c`. The monolith was split into `src/main.c`, `auth.c`, `resolve.c`, `poll.c`, `transport.c`, `ui.c`, `config.c`, `tracker.c`, `history.c`, `forward.c`. | `ls src/`; 00 §4 module table |
| D4 | Phase 3 "move globals into `tgf_ctx` … delete the `extern` globals from `tgf.h`; a grep should find nothing" | Already nothing to delete. The real Phase 3 work is wrapping `Runtime` in an opaque public `tgf_ctx`, adding threads/events/commands, and moving the `ui.c` file-statics into a consumer. | D1; `src/ui.c:12-91` |
| D5 | Phase 1 "add a `./nob test` target" | Exists: 76 tests / 308 checks, ASan+UBSan, link-time stubs. | `nob.c:80-97`, `tests/test_main.c` |
| D6 | Phase 1 "put TDLib behind a transport interface (struct of function pointers create/send/receive/execute/destroy)" | **Partial.** Only `transport_send`/`transport_destroy` are wrapped (plain functions, not a vtable). `td_json_client_create`/`receive`/`destroy` are called directly in `main.c`. No `execute` wrapper. No fake transport. | `src/transport.c:6-24`, `src/main.c:92,109,169` |
| D7 | Phase 1 "fake transport that replays scripted JSON updates … one end-to-end run" | Tests stub `transport_send`/`destroy` and call handlers directly with crafted `cJSON`. There is **no scripted `receive` replay and no loop-level end-to-end test**. `captured_*` records only the last send. | `tests/test_main.c:12-67` |
| D8 | Phase 1 "add `./nob asan` and `./nob tsan`" | `./nob sanitize` exists (ASan+UBSan, full binary). No `asan`/`tsan` named targets; **no ThreadSanitizer anywhere**. No `libtgf.a` target. | `nob.c:99-155,161-182` |
| D9 | 09 §6 / GUI-050/051 config save atomic, 0600, preserve unknown keys | `config.c` is **load-only**. Unknown keys are dropped (`cJSON_Delete(root)` after extracting known fields). No save, no `fsync`/`rename`, no mode setting. | `src/config.c:9-80` |
| D10 | 09 §3.3/3.4 events + commands; GUI-010 TUI reads only events | No event/command API. Feedback is a one-slot `status_msg[256]` + a `Ui` vtable driven by explicit `ui_refresh(dest, src_name, count, delay)` calls; the dashboard reads `ui_rt->status_msg` directly (a private data path). | `src/ui.c:64-66,104-123`, `src/loop.c:86-97` |
| D11 | 09 §3.1 three thread kinds; GUI-002 UI must not block on core | **One app thread.** `td_json_client_receive(client, 1.0)` blocks ≤1 s; all logic runs in that loop. Auth uses blocking `fgets(stdin)` inside the handler, stalling the loop. `keep_running` is `volatile bool`, not atomic. TDLib has its own internal threads. | `src/loop.c:61-102`, `src/auth.c:35-58`, `src/tgf.h:148` |
| D12 | 09 §12 item 8 / GUI-074 instance lock on `tdlib_db/` "may already exist" | No app-level lock. TDLib locks `db.sqlite` internally; a second client on the same dir fails/corrupts ungracefully (observed in a prior session). No clean detection or message. | `src/auth.c:24` (`database_directory:"tdlib_db"`); no lock file in tree |
| D13 | specs `06-tui.md`, `08-tech-debt`, `09-gui.md` referenced by the prompt | These exist but are **uncommitted**: `06-ui-and-cli.md`→`06-tui.md` (pure rename), `99-tech-debt`→`08-tech-debt` (renamed **and edited**), new `09-gui.md` (blind draft) + `TGF GUI.html` mockup. `specs/README.md` and `AGENTS.md` still point at the **old** names. | `git status`; `diff` old99 vs 08; README:12,31,33,55; AGENTS:17,41,43,51,145 |
| D14 | 09 is authoritative for GUI requirements | 09 header: "Written without access to specs 00-08 … inferred from the README and file names." Its §3.2 (`tgf.c`) and §14 open questions are factually wrong/answerable now. It is a useful requirements source (event/command lists, GUI-nnn IDs) but must be reconciled, not trusted as-built. | `specs/09-gui.md:7-9,80,342-349` |

**Net effect:** the globals→struct migration the prompt scopes as Phase 3 is already done
(Steps 1–3 + 4.1–4.6, see 08). The genuinely missing pieces are: a full transport vtable +
fake + loop-level e2e (Phase 1), an opaque thread-safe public API with events/commands/
lifecycle/config-save (Phases 2–3), event-only consumers (Phase 4), and the GUI capability
decisions (Phase 5). The plan below is re-scoped onto that reality.

---

## 3. As-built architecture (what exists today)

### 3.1 State ownership

One `static Runtime rt` (`src/main.c:8-13`) owns everything. `Runtime` (`src/tgf.h:123-151`)
contains `Config cfg` plus liveness, resolution, history, tracker, forward queue/in-flight,
and UI fields. **No module except `main.c` and `ui.c` has file-scope mutable state**
(`grep '^static' src/*.c`): `group_batch` is pure; history/tracker/forward/config/resolve/
poll/auth operate through `Runtime *` / `HistoryStore *` / `TrackerStore *` arguments.

| Module | Reads (Runtime/args) | Writes | File I/O | Direct TDLib |
|---|---|---|---|---|
| `main.c` | cfg, authorized, dest_resolved, source_count, resolve_active, keep_running | rt lifecycle, dispatch | — | create/receive/destroy (direct) |
| `auth.c` | cfg.api_id/api_hash/dest_channel | authorized | — | via `transport_send`; **blocking `fgets`/`printf`** |
| `resolve.c` | cfg.source_channels/dest_channel | dest_resolved, dest_chat_id, source_chat_ids[], source_count, resolve_index/active | — | via transport |
| `poll.c` | cfg window/sequential/ordering, tracker, history, source_chat_ids | tracker pending/scan_from; history+queue via `process_msgs` | — | via transport |
| `history.c` | cfg.history_file/window | HistoryStore set/time/count | `history.txt` (+`.tmp`) | — |
| `tracker.c` | — | TrackerStore entries/pending | `seq_tracker.txt`, `seq_pending.txt` (+`.tmp`) | — |
| `forward.c` | cfg.delay/dest_chat_id, history, tracker | fwd_queue ring, inflight, flood, src_name; history mark; tracker set | `fwd_queue.txt` (+`.tmp`) | via transport |
| `transport.c` | — (`(void)rt`) | — | — | `td_json_client_send`/`destroy` |
| `config.c` | — | Config values | `config.json` (**load only**) | — |
| `ui.c` | `ui_rt->status_msg`, `dashboard_active` | status_msg, dashboard_active; ncurses screen | — | — |

### 3.2 Data flow (unchanged from 00 §8)

`getChatHistory` response → (sequential && !backfill_done ? append pending : filter
id≠0 / not-in-history / date≥cutoff) → `process_msgs` (optional sort → group replies/albums
→ `history_add` at enqueue → `tracker_set` → `fwd_queue_push`) → `process_fwd_queue`
(one job/tick, delay-gated, one in flight, retry/flood/timeout) → `forwardMessages` →
response confirms / error retries or drops. Dedupe mark happens **before** send (00 §9.2).

### 3.3 Threading reality

- **One application thread.** The loop (`src/loop.c:61-102`) calls
  `td_json_client_receive(client, 1.0)` (blocks ≤1 s), dispatches by `@type`/`@extra`,
  then runs periodic tasks by `time(NULL)` deltas: poll every 5 s, dashboard 1 Hz,
  `process_fwd_queue` every tick, prune every 1200 s.
- TDLib runs its own internal threads; the app only sees the JSON receive loop.
- `keep_running` is `volatile bool` written by `sig_handler` (`src/main.c:15-18`). Not a
  C11 atomic; signal-safety is informal.
- **Auth blocks the loop**: `fgets(stdin)` for phone/code/2FA inside `auth_handle`
  (`src/auth.c:35-58`). No tick happens while waiting. Prompts use `printf` directly.
- No callbacks; request/response matched only by the `@extra` tag (`request_extra`,
  `src/tgf.h:93-108`; decoder `request_id_from_extra`, `src/loop.c:8-21`, exact tags
  before prefixes).

### 3.4 Transport seam (partial)

`transport_send(Runtime*, void *client, type, payload, RequestId)` builds the JSON into an
8 KiB buffer, warns + drops on overflow, calls `td_json_client_send`
(`src/transport.c:6-24`). `transport_destroy` wraps `td_json_client_destroy`.
`create`/`receive`/`execute` are **not** behind the seam. Tests substitute `transport_send`/
`destroy` at link time (`tests/test_main.c:55-67`); there is no fake `receive`.

### 3.5 UI sink

`Ui` vtable `{status, forwarded, redraw}` (`src/tgf.h:161-165`); `ui_init` picks stderr or
ncurses (`src/ui.c:93-102`). `ui_status` writes `rt->status_msg` then calls the vtable.
The dashboard is redrawn by explicit `ui_refresh(...)` from the loop and reads
`ui_rt->status_msg` directly (`src/ui.c:64-66`). File-statics: `ui_rt`, `ui_debug`, `ui`,
`ncurses_ready`, `action_msg_row`. This is the private data path GUI-010 forbids.

### 3.6 Build & tests

`nob.c`: default build (`-O3`, links `tdjson` + `ncurses`, all `src/*.c` + `cJSON.c`);
`clean`; `test` (ASan+UBSan, links tracker/history/forward/config/resolve/poll + cJSON,
stubs transport/ui/main/auth); `sanitize` (full binary, ASan+UBSan). `TGF_NOGUI` drops
ncurses (Windows path). No `asan`/`tsan`/`libtgf`/`gui` targets. CI: `.github/workflows/ci.yml`.

---

## 4. Code-vs-spec drift to repair (with approval)

- **README/AGENTS stale names** (D13): point at `06-tui.md` and `08-tech-debt-and-cleanup.md`,
  add `09-gui.md` and this `10-core-api.md` to the spec map. The reorg is uncommitted; it
  should be committed as the Phase 0 baseline together with these fixes.
- **09 blind-draft errors** (D14): §3.2 `tgf.c` premise and §14 open questions are answered
  by §3 above (single-threaded poll loop; no `tgf.c`; `tdlib_db` not app-locked). Reconcile
  09 with a note rather than rewriting its requirements.
- **00 §4 line counts** slightly stale (main.c 164→171, tgf.h 204→215, etc.). Refresh
  citations when Phase 3 moves code (per the citation-refresh rule).
- 08 TD-10 already records the globals as historical; TD-23 (dashboard state model) is the
  open UX item this work supersedes.

---

## 5. Proposed API — `core/tgf_core.h` (Phase 2 preview, NOT yet implemented)

Self-contained public header: includes only `<stdint.h>`, `<stdbool.h>`, `<stddef.h>`.
**Must not** include `tgf.h`, `config.h`, `cJSON.h`, ncurses or GTK. A 30-line consumer
compiles against it alone.

```c
#define TGF_API_VERSION 1

typedef struct tgf_ctx tgf_ctx;            /* opaque; all state inside */

typedef enum { TGF_OK=0, TGF_ERR_INVALID, TGF_ERR_IO, TGF_ERR_OOM,
               TGF_ERR_STATE, TGF_ERR_LOCKED, TGF_ERR_PARSE } tgf_status;

/* Config: same fields as config.json (05 §2), plus unknown-key preservation. */
typedef struct { int api_id; char *api_hash; char *dest_channel;
    char **source_channels; int num_sources;
    char **disabled_channels; int num_disabled;   /* 05 §2 */
    char *history_file;
    int forward_delay_sec, history_window_hours;
    int enable_sequential_forwarding, enable_ordering, sequence_direction;
} tgf_config;

tgf_status tgf_config_load(const char *path, tgf_config *out);   /* validates 05 §2 */
tgf_status tgf_config_save(const char *path, const tgf_config *);/* tmp+fsync+rename, 0600, unknown keys kept */
const char *tgf_config_error(void);                              /* last load failure, startup-only */
void       tgf_config_free(tgf_config *);

/* Transport vtable (Phase 1): real TDLib impl + fake impl. */
typedef struct { void *(*create)(void);
    void (*send)(void *client, const char *json);
    const char *(*receive)(void *client, double timeout);
    char *(*execute)(void *client, const char *json);   /* sync, caller frees */
    void (*destroy)(void *client); } tgf_transport;
extern const tgf_transport tgf_transport_tdlib;        /* real */

/* Events (core→frontend), tagged plain structs, no GLib/ncurses types. */
typedef enum { TGF_EV_AUTH_STATE, TGF_EV_CONNECTION_STATE, TGF_EV_SOURCE_STATE,
    TGF_EV_FORWARDED, TGF_EV_SKIPPED, TGF_EV_QUEUE, TGF_EV_NEXT_FORWARD_IN,
    TGF_EV_PIPELINE_STATE, TGF_EV_LOG, TGF_EV_ERROR } tgf_event_kind;
typedef struct { tgf_event_kind kind; /* union of payloads per 09 §3.3 */ } tgf_event;

tgf_ctx   *tgf_create(const tgf_config *, const tgf_transport *);
tgf_status tgf_start(tgf_ctx *);          /* spawns ONE core thread (§6) */
void       tgf_stop(tgf_ctx *);           /* idempotent; joins the thread */
void       tgf_destroy(tgf_ctx *);

bool       tgf_next_event(tgf_ctx *, tgf_event *out);  /* non-blocking; false if empty */
int        tgf_event_fd(tgf_ctx *);       /* pollable eventfd/pipe; readable when queued */
void       tgf_event_free(tgf_event *);   /* consumer frees core-allocated payload */

/* Commands (frontend→core): thread-safe, non-blocking; results arrive as events. */
tgf_status tgf_submit_phone(tgf_ctx *, const char *);
tgf_status tgf_submit_code(tgf_ctx *, const char *);
tgf_status tgf_submit_password(tgf_ctx *, const char *);
tgf_status tgf_pause(tgf_ctx *);
tgf_status tgf_resume(tgf_ctx *);
tgf_status tgf_add_source(tgf_ctx *, const char *handle);
tgf_status tgf_remove_source(tgf_ctx *, long long id);
tgf_status tgf_set_source_enabled(tgf_ctx *, long long id, bool);
tgf_status tgf_rescan_source(tgf_ctx *, long long id);
/* Handle-addressed variants. A source disabled in config is never resolved,
 * so it has no chat id; frontends address it by its configured handle. */
tgf_status tgf_remove_source_by_handle(tgf_ctx *, const char *handle);
tgf_status tgf_set_source_enabled_by_handle(tgf_ctx *, const char *handle, bool);
tgf_status tgf_rescan_source_by_handle(tgf_ctx *, const char *handle);
tgf_status tgf_apply_config(tgf_ctx *, const tgf_config *);
tgf_status tgf_logout(tgf_ctx *);
```

Rules: no `exit`/`abort` in the core (fatal paths return a status + emit `TGF_EV_ERROR`);
all logging via `TGF_EV_LOG`/`TGF_EV_ERROR` (no `printf`); secrets never placed in events
and secret buffers zeroed after use; `keep_running` becomes `atomic_bool`. Every function
gets a thread-safety + ownership comment. `tgf_event_fd` lets a frontend integrate with
`poll()` or a GLib `GSource` without busy-waiting.

The id-addressed source commands only work for sources that resolved to a chat id.
`disabled_channels` entries are skipped during resolution (05 §2), so a source disabled
at startup has id `0` and can only be enabled by handle. The handle variants normalize
`@name`/`name`/`t.me/name` exactly like `disabled_channels` matching.

---

## 6. Proposed threading model (decision — see §9 A)

**Recommendation: ONE core worker thread** running the existing tick loop verbatim
(receive → dispatch → poll/forward/prune), preserving order/dedupe/delay/backfill exactly.
Cross-thread surfaces are only: a mutex-protected **event queue** (core→frontend) with an
`eventfd` wake-up, and a mutex-protected **command queue** (frontend→core) drained at the
top of each tick. Auth becomes command-driven: the core emits `TGF_EV_AUTH_STATE` and waits
for `submit_phone/code/password`; the **CLI consumer** does the `fgets`/prompts, so the
core never blocks on stdin. This satisfies GUI-001/002/003/004/012 with the smallest
deviation from today's single-threaded semantics. 09 §3.1's "receive thread + separate
worker" is unnecessary complexity for v1; TDLib already threads internally.

This contradicts 00 §5 ("One thread") and the AGENTS "no threads without a spec" boundary;
spec 00 + this spec authorize it once approved. `tgf_stop` sets the atomic flag, wakes the
loop, joins the thread, flushes state, destroys the client — idempotent.

---

## 7. Phase 1 — safety net (before any refactor)

1. Upgrade the seam to the full `tgf_transport` vtable (create/send/receive/execute/destroy).
   Real impl wraps the existing `td_json_client_*`; route `main.c`'s direct create/receive/
   destroy through it. **No behaviour change.**
2. Add a **fake transport**: a scripted queue of JSON responses returned by `receive`, and a
   recorded log of everything `send` received. Drive the *whole loop* offline.
3. Add a loop-level **end-to-end test** on the fake: scripted messages in → expected
   `forwardMessages` out, correct order for `asc` and `desc`, dedupe, delay, window,
   sequential/backfill, restart-resume. Keep the existing 76 handler-level tests.
4. Add `./nob asan` (ASan+UBSan, alias of sanitize semantics), `./nob tsan` (ThreadSanitizer
   on the threaded core + e2e), and `./nob libtgf` (archive core objects). POSIX-only like
   the existing test target.
5. Record baseline counts in this file. Each step builds `-Wall -Wextra` clean, one commit.

Coverage targets (prompt): history load/add/has/prune + duplicates; tracker save/load/cursor;
forward batching (albums, reply+parent, `MAX_FWD_IDS`); config defaults/missing/bad-types/
unknown-keys; e2e asc+desc. Most handler-level cases already exist (extend, don't rebuild).

### Phase 1 baseline results (recorded 2026-11-20)

Implemented: full `Transport` vtable (`src/transport.h`), real impl split into
`src/transport_tdlib.c`, TDLib-free JSON builder in `src/transport.c` (routes via
`rt->tp`); dispatch + tick extracted from `main.c` into `src/loop.c` (`loop_tick`,
`LoopState`); fake transport (`tests/transport_fake.c`) + loop-level e2e
(`tests/test_e2e.c`); `./nob e2e`, `./nob asan`, `./nob tsan` targets.

Observed gates:
- `./nob` (real app, TDLib rebuilt): **clean, no warnings**; `tgf` binary produced.
- `./nob test`: **308 handler checks + 17 e2e checks, 0 failures** (ASan+UBSan).
  e2e covers asc, desc, dedupe-across-polls, delay gate, backfill asc — all driving
  the real `loop_tick` + real `transport_send` through the fake.
- `./nob tsan`: e2e **17 checks, 0 failures**, no ThreadSanitizer reports.
- `./nob asan`: app build clean.
- Narrow lint (`-Wall -Wextra -Wshadow -Wpedantic`) on main/loop/transport/transport_tdlib: clean.
- cppcheck: **no new errors/warnings** vs baseline.
- CLI validation (`./tgf badarg` → usage, exit 1): intact.

**Incident (resolved):** the first `test_e2e.c` lacked the `mkdtemp`+`chdir` CWD
isolation that `test_main.c` uses, so its `reset_state` deleted the repo's real
`config.json`, `history.txt`, `seq_tracker.txt`, `seq_pending.txt`, `fwd_queue.txt`.
`config.json` was restored from `config.bak` (mode 0600); `tdlib_db/` was untouched.
The four runtime state files had no backup and are lost (bounded impact: a one-time
re-forward of in-window messages and a backfill re-scan on next run; no message
destruction). Fixed by adding the same temp-CWD isolation to `test_e2e.c`; re-ran
`./nob test` and confirmed repo `config.json` survives and temp dirs are cleaned.

---

## 8. Phases 3–5 (re-scoped onto reality)

**Phase 3 — migrate one module at a time** (order: history → tracker → config → forward →
polling loop → auth). For each: fold its state into the opaque `tgf_ctx` (it already takes
`Runtime *`; the change is hiding `Runtime` behind the public opaque type and threading the
context), update callers, run tests + ASan + TSan + fake e2e, commit. Move `ui.c` statics
into the TUI consumer. Introduce the core thread + queues (§6). Make auth command-driven.
At the end: no `extern` globals (already true), core compiles without ncurses/GTK, secrets
never logged. *The prompt's "delete extern globals" step is a no-op verification, not work.*

**Phase 4 — consumers.** CLI `main()` becomes thin: parse `-d`, `tgf_config_load`,
`tgf_create`, `tgf_start`, drain events, SIGINT/SIGTERM→`tgf_stop`; first-run stdin prompts
driven by `TGF_EV_AUTH_STATE` + submit commands (identical user experience). ncurses TUI
rewritten to read **only events** (GUI-010), no core internals. `TGF_NOGUI` builds keep
working. `./nob` links `libtgf.a` + the TUI object (ncurses only in the TUI, not the core).

**Phase 5 — GUI capability decision gate (STOP AND ASK).** Pause/resume, per-source
counters + last-seen id, per-source rescan, add-source with resolution, rate-limit countdown
event: implement behind the API; report where the current design makes each hard.
**Per-source enable/disable** (09 §12 item 3): decided as
`disabled_channels: []` (05 §2). Disabled entries stay in `source_channels`, are skipped
during resolution and polling, and are addressed by handle when the frontend has no chat id.
**Instance lock** on `tdlib_db/` (D12): none exists app-level; propose one. These are product
decisions; do not guess.

---

## 9. Open decisions to confirm before Phase 1

- **A. Threading model:** one core worker thread (§6, recommended) vs 09's receive+worker
  split. Confirm one thread.
- **B. Spec reorg checkpoint:** commit the uncommitted `06-tui`/`08`/`09`/mockup reorg +
  README/AGENTS name fixes + this spec 10 as the Phase 0 baseline? Reconcile 09's blind-draft
  errors (§3.2/§14) with a note? (Another agent may be mid-edit — confirm ownership.)
- **C. Auth/secret handling:** move `fgets`/prompts out of the core into the CLI consumer;
  core emits auth-state events; zero phone/code/password buffers; route all core logging
  through events. Confirm this preserves the first-run UX you want.
- **D. Config save semantics:** adding `tgf_config_save` (atomic, 0600, preserve unknown
  keys) is new behaviour → amend 05. Unknown-key preservation requires retaining the parsed
  cJSON tree (or re-reading the file) in `tgf_config`. Confirm.
- **E. Phase 5 format/lock decisions** (per-source enable, instance lock) can wait until
  Phase 5, but they shape `tgf_config`/`SOURCE_STATE`; flag now, decide at the gate.

---

## 10. Risks & invariants to preserve

- **Behaviour parity is the hard gate.** The characterization suite (76/308) + the new
  fake-transport loop e2e (asc/desc, dedupe, delay, window, backfill, restart) must stay
  green through every phase. Thread introduction must not reorder forwards or change the
  one-job-per-tick / one-in-flight / mark-at-enqueue invariants (00 §9, 03 §7).
- **Blocking-auth → command-auth** changes the CLI interaction path, not forwarding. Keep
  prompts byte-identical from the user's side; never log secrets.
- **Durable formats** (`history.txt`, `seq_tracker.txt`, `seq_pending.txt`, `fwd_queue.txt`,
  `config.json`, `tdlib_db/`) stay compatible; an existing install keeps working. No `fsync`
  regression on the existing tmp+rename writes; config save adds `fsync`+0600 (new).
- **Fatal paths** currently `exit(1)` after `transport_destroy` (`src/loop.c:51-53`). The
  core must instead return a status + error event; the *consumer* decides to exit. Preserve
  clean client destroy before any exit (no `tdlib_db` corruption).
- **TSan + TDLib:** TDLib's internal threads may produce benign reports; scope TSan to the
  core's own queues/flag and the e2e, and document any TDLib-internal suppression rather
  than weakening core checks.
- **Live verification** needs the user's session lock free (their `tgf` holds `tdlib_db/`);
  use a scratch CWD with symlinked state dirs, never kill their service.

---

## 11. Acceptance criteria (maps to the task Definition of Done)

| ID | Criterion | Check |
|---|---|---|
| CA-1 | No `extern` globals in `tgf.h`; core compiles without ncurses/GTK | `grep extern src/tgf.h` empty; `./nob libtgf` builds with `-DTGF_NOGUI` and no curses link |
| CA-2 | `./nob test` passes incl. offline e2e for asc AND desc | test output counts; e2e asserts forward order both directions |
| CA-3 | ASan, UBSan, TSan clean on the e2e | `./nob asan`, `./nob tsan` run output |
| CA-4 | Manual real-Telegram run forwards correctly; restart re-forwards nothing | live scratch-CWD run + restart (session lock permitting) |
| CA-5 | spec 10 accurate; 00/05/06-tui/09 updated where the API changes them | citation audit; amendment logs |
| CA-6 | 30-line C program creates ctx, starts, prints events, includes only `tgf_core.h` | compile+run the snippet against `libtgf.a` |
| CA-7 | `./tgf` / `./tgf -d` behaviour unchanged; `TGF_NOGUI` builds work | characterization suite + manual CLI run + NOGUI build |
| CA-8 | Secrets absent from logs/events/errors/files beyond existing storage | grep events/log paths; review auth/config code |

---

## Amendment log

### Phase 2–4 completion (recorded 2026-11-20, commit a0991c0)

Implemented and observed:
- `core/tgf_core.h` — self-contained public API (opaque `tgf_ctx`, lifecycle,
  `tgf_config` load/save, 10 event kinds, 11 commands, pipe `tgf_event_fd`,
  `TGF_API_VERSION 1`). Includes only `<stdatomic.h> <stdbool.h> <stddef.h> <stdint.h>`.
- `src/core.c` — one worker thread running `loop_tick`; mutex event + command
  queues; non-blocking pipe wake; `atomic_bool running/paused`; config save
  (tmp + `fsync` + `rename`, mode 0600, unknown keys preserved); all commands.
- `auth.c` command-driven (emits `AUTH_STATE`, no `fgets` in the core);
  `auth_submit` wipes the payload. `ui_status`/`ui_forwarded` emit events.
- `ui.c` pure event-driven dashboard (no `Runtime` access; GUI-010).
- `main.c` thin consumer; `nob libtgf` builds `libtgf.a`; `./nob` links it.
- Gates: `./nob` clean; `./nob test` 308 handler + 17 e2e + 23 core checks,
  0 failures (ASan+UBSan); `./nob tsan` clean; cppcheck no new findings;
  30-line consumer including only `tgf_core.h` compiles/links/runs against
  `libtgf.a`; `grep extern src/tgf.h` = 0; live run forwards telegram -> @me.

Deviations / open (Phase 5 decision gate):
- Single context per process (`s_ctx` shortcut) — fine for CLI + GUI single instance.
- Per-source enable/disable persists through `disabled_channels` (05 §2); loaded-disabled
  sources are addressed by handle because they have no chat id.
- Instance lock on `tdlib_db/` NOT implemented (still the open GUI-074 item).
- `apply_config` re-resolves; full hot-reload semantics are Phase 5.

| Date | Change |
|---|---|
| 2026-11-20 | Phase 0 draft created: as-built audit (§3), prompt/spec discrepancy report (§2, D1–D14), proposed API/threading/migration (§5–8), open decisions (§9), risks (§10), acceptance (§11). No code changed. |
| 2026-11-21 | Core-API repair: `tgf_config_load` now validates the 05 §2 schema and reports `tgf_config_error()`; `disabled_channels` added; `tgf_apply_config` reallocates `source_chat_ids` and re-resolves the destination (heap overflow fixed); add/remove source are memory-safe and re-resolve only the new slot; `tgf_destroy` frees tracker pending buffers and clears the context shortcut; fatal paths no longer `exit()`/`printf()` inside the core (destination/source resolution failures become `TGF_EV_ERROR` + a clean stop); `SOURCE_STATE`, `CONNECTION_STATE` and aggregate `SKIPPED` events are emitted. |
| 2026-11-21 | Added handle-addressed `remove_source_by_handle` / `set_source_enabled_by_handle` / `rescan_source_by_handle` because a source disabled at startup never resolves and therefore has no chat id. `disabled_channels` confirmed as the per-source enable format. |
