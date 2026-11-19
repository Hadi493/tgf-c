# 00 — Architecture

Status: baseline · Owner: whole system · Depends on: none

## 1. Purpose

TGF-C monitors a fixed set of public Telegram channels and forwards their
messages to one destination chat, with deduplication, an age cutoff, and an
optional collect-then-forward ("sequential") mode. It is a single-process,
single-threaded CLI program backed by flat files.

## 2. Stack

| Concern | Choice | Evidence |
|---|---|---|
| Language | C, compiled as one binary | `nob.c:220-235` |
| Telegram API | TDLib JSON interface (`libtdjson`) | `src/transport.c:3-4`, `src/main.c:87-89` |
| JSON | vendored cJSON | `src/tgf.h:10`, `cJSON.c` |
| Terminal UI | ncurses, disabled with `TGF_NOGUI` or any argv | `src/ui.c:29-89`, `src/main.c:128-142` |
| Build | `nob` (self-rebuilding C build script) | `nob.c` |
| Storage | `config.json`, `history.txt`, `seq_tracker.txt`, `tdlib_db/` | 05 |
| Tests / CI | `./nob test` (POSIX) + GitHub Actions (`.github/workflows/ci.yml`) | `nob.c:80-97`, TD-17 |

TDLib is pinned as a git submodule at `v1.8.0-10143-ga17f87c4c`
(`.gitmodules`, `lib/td`).

## 3. Component view

```
                       ┌────────────────────────────────────┐
                       │             tgf (process)          │
                       │                                    │
 config.json ─────────►│ config loader ─► Runtime           │
                       │                                    │
 stdin ───────────────►│ auth prompts ─► transport_send           │
                       │                                    │
                       │ main loop (1 s tick)               │
                       │  ├ decode @type / @extra           │
                       │  ├ resolve (dest, sources)         │
                       │  ├ poll every 5 s ─► getChatHistory│
                       │  ├ history handler                 │
                       │  │   ├ cutoff + dedupe             │
                       │  │   └ grouping ─► forward queue   │
                       │  ├ process_fwd_queue (1 job/tick)  │
                       │  ├ dashboard refresh (1 Hz)        │
                       │  └ prune every 20 min              │
                       │                                    │
                       │ history hash set ─► history.txt    │
                       │ seq tracker ─────► seq_tracker.txt │
                       └───────────┬────────────────────────┘
                                   │ JSON strings
                                   ▼
                            libtdjson / TDLib
                                   │
                                   ▼
                            Telegram servers
```

## 4. Module responsibilities

| File | Lines | Responsibility | Owns state |
|---|---|---|---|
| `src/main.c` | 164 | main loop, dispatch, shutdown, config adapter | static `Runtime` (`cfg`, dedupe, tracker, queue, status) |
| `src/auth.c` | 72 | auth state machine + prompts | — |
| `src/resolve.c` | 59 | destination/source resolution | — |
| `src/poll.c` | 150 | poll request + history handler | — |
| `src/transport.c` | 29 | TDLib JSON send | — |
| `src/ui.c` | 130 | status sink + dashboard | `ui_rt`, ncurses |
| `src/config.c` | 89 | `config.json` parsing and defaults | `Config` values only |
| `src/config.h` | 20 | `Config` struct, `config_load`, `config_free` | — |
| `src/tgf.h` | 204 | shared types, constants, `Runtime` struct, declarations | — |
| `src/tracker.c` | 139 | sequential scan state + in-memory pending buffer | `seq_tracker[]` |
| `src/history.c` | 127 | dedupe hash set, persistence, pruning | `history_set`, `history_time` |
| `src/forward.c` | 164 | grouping, forward queue, forward response | `fwd_queue` |
| `nob.c` | 237 | build orchestration, TDLib bootstrap, test target | — |
| `cJSON.*`, `nob.h`, `lib/td` | — | vendored dependencies | — |

`src/forward.c`, `src/tracker.c`, `src/history.c`, `src/config.c`, and
`src/resolve.c` are unit-tested by `tests/test_main.c` with stubbed
`transport_send`/`transport_destroy`/`ui_status`/`ui_oom`/`ui_forwarded`.
`group_batch` is pure (no `Runtime`). Only `src/ui.c` keeps a file-scope
`Runtime *` (`ui_rt`) for the status slot and `atexit` cleanup.

## 5. Execution model

- **One thread.** All TDLib traffic is the blocking JSON client:
  `td_json_client_send` and `td_json_client_receive(client, 1.0)`
  (`src/transport.c:6-29`, `src/main.c:103`).
- **One tick ≈ 1 s.** `receive` blocks up to one second, so every periodic task
  is scheduled by comparing `time(NULL)` deltas in the loop.
- **No callbacks.** Responses are matched to requests only by the `@extra`
  string embedded at send time (`request_extra`, `src/tgf.h:90-105`). The tags
  are: `auth_params`, `auth_phone`, `auth_code`, `auth_pw`, `getme`,
  `resolve_dest`, `resolve_<index>`, `history_<chat_id>`, `fwd_<chat_id>_<id>`,
  `fwd_<chat_id>_batch`.
- **Dispatch table.** `main` branches on `@type`; `on_response` decodes the
  extra into a typed `RequestId` (`request_id_from_extra`, `src/main.c:23-36`)
  and selects a handler via `handler_for` (`src/main.c:39-56`). Exact tags
  (`resolve_dest`, `getme`) are matched before prefix tags (`resolve_`).
- **Single parsing for responses.** `main` parses each response once and passes
  the `cJSON *root` to handlers, deleting it after the handler returns
  (`src/main.c:105-116`). The auth handler still receives the raw string and
  uses `strstr` (`src/auth.c:19`).
- **Blocking auth.** Phone/code/2FA prompts use `fgets(stdin)` inside the
  auth handler (`src/auth.c:35-58`); the loop cannot tick while waiting.

## 6. Runtime state

### 6.1 Configuration (immutable after load)

`Config` (`src/config.h:4-15`) is loaded once by `config_load` and stored as
`rt.cfg`; no module keeps a copy.

| Field | Type | Meaning |
|---|---|---|
| `api_id`, `api_hash` | int, char* | Telegram app credentials |
| `source_channels`, `num_sources` | char**, int | configured source names, raw strings |
| `dest_channel` | char* | configured destination name |
| `history_file` | char* | dedupe log path (CWD-relative) |
| `forward_delay_sec` | int | minimum spacing between forward jobs |
| `history_window_hours` | int | message-age cutoff and dedupe validity window |
| `enable_sequential_forwarding` | int (0/1) | backfill + ordering mode |
| `sequence_direction` | int (0 asc, 1 desc) | sort order for sequential flushes |

### 6.2 Liveness / resolution

All fields live in `Runtime` (`src/tgf.h:120-144`).

| Field | Meaning | Set by |
|---|---|---|
| `authorized` | TDLib reached `authorizationStateReady` | `src/auth.c:60` |
| `dest_resolved`, `dest_chat_id` | destination chat id known | `src/resolve.c:40-47` |
| `source_chat_ids[]`, `source_count` | resolved source ids, count of successes | `src/resolve.c:49-59` |
| `src_name` | display name of the last source whose forward **response succeeded** | `src/forward.c:154-164` |
| `resolve_index`, `resolve_active` | source-resolution cursor / in-flight flag | `src/resolve.c:6-30` |

### 6.3 Work and storage

`static Runtime rt` (`src/main.c:9-13`) owns every field below.

| Field | Meaning |
|---|---|
| `history` (`HistoryStore`: `set`, `time`, `set_size`, `count`) | open-addressing dedupe table + enqueue timestamps |
| `tracker` (`TrackerStore`: `entries[MAX_SEQ_TRACKER]`, `count`) | per-source sequential state and pending buffer |
| `fwd_queue`, `fwd_queue_count`, `fwd_queue_cap`, `fwd_queue_head` | ring-buffer FIFO of `ForwardJob` (heap `ids` per job) |
| `fwd_last_time` | timestamp of last dispatch attempt (double seconds) |
| `keep_running`, `dashboard_active`, `status_msg[256]` | loop flag, UI mode, one-slot status message |

## 7. State machine

```
                    ┌────────────┐
   start ──────────►│  STARTING  │ load config, client, files
                    └─────┬──────┘
                          │ authorizationState updates
                          ▼
                    ┌────────────┐  phone/code/2FA prompts
                    │    AUTH    │◄──────────────┐
                    └─────┬──────┘               │ error (no retry logic)
                          │ Ready                │
                          ▼                      │
                    ┌────────────┐  resolve dest (getMe/createPrivateChat
                    │  RESOLVING │  or searchPublicChat), then sources
                    └─────┬──────┘  sequentially
                          │ dest_resolved && source_count >= 1 && !resolve_active
                          ▼
                    ┌────────────┐  poll every 5 s, forward 1 job/tick
                    │   ACTIVE   │
                    └─────┬──────┘
                          │ SIGINT / SIGTERM
                          ▼
                    ┌────────────┐  free, endwin, destroy client
                    │  SHUTDOWN  │  (queue and pending buffers are dropped)
                    └────────────┘
```

There is no enum. The `ACTIVE` gate is evaluated each tick at `src/main.c:121`.

## 8. Primary data flow

```
TDLib getChatHistory response (newest first, ≤ MSG_LIMIT)
        │
        ├─ [sequential && !backfill_done] ──► append to pending buffer (04)
        │
        └─ otherwise ──► filter: id != 0, not in history set, date ≥ cutoff
                              │
                              ▼
                        process_msgs
                              ├─ optional sort (sequential only)
                              ├─ group replies (depth 1) / albums
                              ├─ history_add(each message)   ← mark at enqueue
                              ├─ seq_tracker_set (sequential only)
                              └─ fwd_queue_push
                                        │
                                        ▼  one job per tick, delay-gated
                                 forwardMessages(dest, src, ids)
                                        │
                                        ▼
                                 response: update src_name / UI
                                 error: status line only, no retry
```

## 9. Invariants (MUST)

1. The process writes to Telegram only through `transport_send` (`src/transport.c:6`).
2. A message is added to the dedupe set **before** its forward request is sent;
   delivery success is not required (`src/forward.c:123`).
3. `forwardMessages` jobs are dispatched at most once per loop tick
   (`src/forward.c:130-152`).
4. `source_count` counts successful resolutions only; failed sources are never
   polled.
5. Polling starts only when `authorized && dest_resolved && source_count >= 1
   && !resolve_active`.
6. `seq_tracker` is loaded only when sequential mode is enabled (`src/main.c:95`);
   the file is written only by sequential paths and `flush_pending`.
7. Every runtime path is relative to the process CWD.
8. The dedupe key is `<chat_id>_<message_id>` (`src/history.c:62`, `87`).
9. Messages are forwarded from the source chat id to the configured
   destination chat id in one `forwardMessages` call per job
   (`src/forward.c:144`).
10. There is no retry, backoff, or flood-wait logic anywhere.

## 10. Boundaries (current and target)

| Boundary | Current reality | Target for refactor |
|---|---|---|
| TDLib | `transport_send`/`transport_destroy` + typed `RequestId` (`src/transport.c`, `src/tgf.h:70-105`) | achieved; handler registry in `src/main.c:39-49` |
| Storage | `history_store_*` / `tracker_store_*` APIs; direct `fopen`/`rename` inside | one storage module per file (APIs exist, files not split) |
| Config | `Config` built by `config_load`, stored as `rt.cfg` via a thin `load_config` adapter (`src/main.c:74-76`) | achieved |
| UI | `Ui` vtable (`ui_init`, stderr/ncurses impls, `src/ui.c:91-123`) | achieved; dashboard state model still open (TD-23) |
| Domain | `group_batch` pure; queue takes `Runtime *`; clock is `time(NULL)` inside `process_fwd_queue` | injected clock (open) |

Until the Phase 3 refactor, new code MUST stay inside the owning module and
MUST NOT add new globals to `src/tgf.h` without a spec change (TD-10).

## 11. Known architectural limitations

- No durable work queue; a restart loses queued forwards and pending backfill
  (TD-02, TD-03).
- Feedback is one-slot and lossy: several status messages between dashboard
  refreshes collapse to the last (06).
- No error taxonomy: every failure is a string (TD-12).
- No metrics, no request accounting.
- Fixed capacities: 128 tracked chats, 256 ids per job, 8 KB request buffer,
  10,000 messages per history page (`src/tgf.h:13-39`, `src/transport.c:10`).
  The forward queue itself grows dynamically (ring buffer, heap id payloads).

## 12. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-00-01 | Start with valid config in the repo root | Auth prompts (first run) or immediate resolution, then dashboard |
| SC-00-02 | Start from another directory | Fresh `tdlib_db/` and empty history are created there; old state is not used |
| SC-00-03 | Send SIGINT while running | Process exits within ~1 s with code 0; terminal is restored |
| SC-00-04 | Inspect process during steady state | Exactly one thread; no sockets other than TDLib's own |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c` citations and the §6 state model to the working-tree `Runtime`/`Config` refactor |
| 2026-11-17 | Step 2: split `tgf.c` into `src/main.c`/`auth.c`/`resolve.c`/`poll.c`/`transport.c`/`ui.c`; refreshed citations and module table |
| 2026-11-19 | As-built refresh: Step 1c–1f seams (storage APIs, RequestId registry, pure group_batch, Ui vtable), Step 3 structural work (ring-buffer queue TD-25, ui_oom TD-19, dead-code removal TD-24, parse-once), Step 4.1 restoration; removed pending_req; refreshed citations and module table |
