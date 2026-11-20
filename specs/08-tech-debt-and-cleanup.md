# 08 — Technical Debt and Cleanup

Status: baseline register · Owner: maintainers · Depends on: 00–07

This file is both a defect register and the source of the Phase 3 plan. It
separates **structural debt** (can be paid down without changing observable
behavior) from **behavior defects** (require a spec amendment before the code
changes). The IDs are referenced from the feature specs; do not renumber them.

## 1. Priority definitions

| Priority | Meaning |
|---|---|
| **P0** | Can lose/duplicate user data, exhaust memory, or leave the service silently dead in normal use |
| **P1** | Breaks a normal workflow, hides failures from the operator, or blocks safe change |
| **P2** | Correctness/UX/documentation rough edge; low blast radius |

## 2. Register

### P0

#### TD-01 — Dedupe is recorded at enqueue, not delivery — **PARTIALLY ADDRESSED (Step 4.3)**
- **Evidence (pre-fix):** no retry; the error path dropped the job, so any
  failed `forwardMessages` lost the message permanently.
- **Spec clauses:** 02 §3.4, 03 §7.
- **Resolution:** at-least-once delivery (03 §7): one in-flight job, bounded
  retry (3 attempts), flood-wait pause, permanent-drop and timeout statuses,
  stale-response guard. Marking stays at enqueue; the un-mark-on-failure
  option was rejected (re-exposure race + probe-safe remove complexity).
- **Residual:** restart discards queue + in-flight (TD-03, Step 4.4);
  re-queue OOM drops the job (TD-19); permanent failures (400/403) are
  dropped by design with a status line.

#### TD-02 — Backfill pending buffer is RAM-only — **ADDRESSED (Step 4.4b)**
- **Evidence (pre-fix):** pending lived only in RAM; `scan_from` was persisted
  per page but collected messages were not.
- **Impact:** kill mid-backfill lost messages newer than the resume cursor;
  kill after the last page caused a full re-scan (duplicates).
- **Resolution:** append-only journal `seq_pending.txt` (05 §4.3), replayed
  into the RAM buffers at startup (sequential mode), compacted on flush.
  04 §2.4/§3.6/§4/§5 amended. Residual: no `fsync`, so a crash can lose the
  last unflushed appends (same window as `history.txt`).

#### TD-03 — Forward queue is RAM-only and dropped on shutdown — **ADDRESSED (Step 4.4a)**
- **Evidence (pre-fix):** queue lived only in RAM; shutdown dropped it while
  ids were already in `history.txt`.
- **Resolution:** queue + in-flight persisted to `fwd_queue.txt` (tmp+rename)
  after every mutation; loaded at startup (05 §5). Restart resumes delivery;
  the in-flight job is re-queued (at-least-once duplicate possible, 03 §7).
  01 §6 and 03 §5/§7 amended.

#### TD-04 — Hash deletion creates probe-chain holes — **ADDRESSED (Step 4.2)**
- **Evidence (pre-fix):** prune NULLed slots in a linear-probing table,
  breaking lookup chains for colliding survivors.
- **Impact:** after pruning, some live keys became unreachable → duplicate
  forwards.
- **Resolution:** prune rehashes survivors into a fresh same-size table via
  `history_rehash` (`src/history.c:119`); 02 §3.5/§3.6 amended; the
  characterization test now asserts survivors stay reachable. Rehash OOM
  leaves holes until the next successful prune (reported via `ui_oom`).

#### TD-05 — Failed resolution is invisible and permanent — **ADDRESSED in Step 4.1**
- **Evidence:** `src/main.c:23-74` (no retry), `src/main.c:127` (gate just stays
  false), `src/ui.c:59` (literal `ACTIVE`).
- **Impact:** a typo in `dest_channel` yields one ≤1 s status line and a
  process that looks healthy forever. Same for an all-failed source set.
- **Fix gate:** fail-fast with a clear exit, or a retry state with a visible
  error state in the UI; amend 01 §3/§4 and 06 §3.
- **Resolution:** Step 4.1 — destination resolution failure now fatal (exit 1); all-sources-failed and empty-sources now fatal (exit 1); failed sources summary printed. Lost in the RequestId seam rewrite; restored 2026-11-19 with clean client destroy (`transport_destroy`); verified live (SC-01-03/04/05, SC-06-05).

#### TD-25 — Forward-job representation and queue shape — **ADDRESSED**
- **Evidence (pre-fix):** fixed `ids[256]` per job ≈ 2 KiB; whole-array shift
  per pop; one job per tick.
- **Impact:** 10k queued single messages ≈ 20 MB and O(n²) shifting; 450k
  (45 sources × 10k) ≈ 0.9 GB, likely OOM before the queue drains.
- **Resolution:** ring buffer with head index and heap `ids` sized to `count`
  (`src/tgf.h:37-42`, `src/forward.c:3-20`, `130-152`); FIFO, delay gate,
  one-per-tick, and the 256-id cap preserved. 03 §5 refreshed; no observable
  pacing change.

### P1

#### TD-06 — No flood-wait / backoff / rate-limit handling — **ADDRESSED (Step 4.3)**
- **Evidence (pre-fix):** no `retry_after` or `429` inspection anywhere.
- **Impact:** Telegram rate limits turned into permanent loss via TD-01.
- **Resolution:** 429 parses `retry after N` (default 5 s, min 1 s), pauses
  all dispatch until `now + N`, re-queues without consuming retry budget;
  pause is uncapped by design (03 §7). Other errors use the bounded retry
  budget.

#### TD-07 — Source resolution failures are silent beyond one line — **ADDRESSED in Step 4.1**
- **Evidence:** `src/main.c:58-74`; failed sources keep `chat_id = 0` and are
  skipped by `src/poll.c:9-10`.
- **Impact:** operator cannot tell which sources are dead; no retry; no summary.
- **Fix gate:** decide fail-fast vs. degrade-with-warning; amend 01 §4.
- **Resolution:** Step 4.1 — failed sources summary printed; all-failed or empty sources now fatal (exit 1); partial failures continue with warning summary. Restored 2026-11-19 (see TD-05); summary format pinned in 01 §4.

#### TD-08 — Auth failures are not handled
- **Evidence:** `src/auth.c:18-72` handles only four states; `on_error` only
  prints (`src/main.c:58-74`).
- **Impact:** wrong code/password, QR login, or device confirmation leaves the
  process idle with no retry or guidance.
- **Fix gate:** re-prompt/abort policy; amend 01 §2.

#### TD-09 — Fixed caps and silent truncation — **PARTIALLY ADDRESSED (Step 4.5)**
- **Evidence:** `MAX_SEQ_TRACKER 128` (`src/tgf.h:16`), `MAX_FWD_IDS 256`
  (`src/tgf.h:33`), reply/album arrays 256 (`src/forward.c`), 8 KiB request
  buffer (`src/transport.c:10`), 10k page with no pagination in live mode
  (`src/tgf.h:14`, `src/poll.c:13-14`), `num_sources` unbounded.
- **Resolution:** album grouping splits >256-member albums into multiple
  ascending jobs (stack overflow fixed, no loss); `transport_send` no longer
  sends truncated JSON (warns and returns); tracker-full and history-page-full
  warnings added (04 §2.5, 02 §1.2).
- **Residual:** >255 reply children still marked-not-forwarded (rare); live
  mode has no pagination (>10k between polls missed, warned); the 128-chat
  tracker cap and 256-id job cap remain hard limits. Pagination/dynamic
  structures are Step 5.

#### TD-10 — Global mutable state and a god header
- **Evidence:** 30 `extern` declarations in `tgf.h` before Step 1b
  (historical; the current header has no `extern`s);
  every module read and wrote them; `tgf.c` owned all behavior. Resolved for
  the config slice (Step 1a), the runtime slice (Step 1b), the remaining seams
  (Steps 1c–1f), and the Step 2 split into `src/`; the header is still one
  `src/tgf.h`.
- **Impact:** no module can be tested alone; changes have unbounded blast
  radius.
- **Structural fix:** `Config` and `Runtime` structs passed explicitly,
  module-private state, header split per module. No behavior change.

#### TD-12 — Errors are strings; reporting policy is ad hoc
- **Evidence:** every failure is `ui_status("…")`; exits mix `fprintf` and
  silent returns (`src/config.c:9-80`, `src/main.c:154-170`); one-slot `status_msg`
  (`src/tgf.h:150`, `src/ui.c:104-108`).
- **Impact:** no error taxonomy, no counters, no reliable operator feedback,
  lost messages between refreshes.
- **Structural fix:** typed result codes + one reporting sink; preserve the
  exact current text/streams until a UI spec changes them.

#### TD-17 — Test coverage is partial; CI added
- **Evidence:** `tests/test_main.c` + `./nob test` cover grouping/order/caps,
  dedupe add/has/expire/prune (including TD-04 chain repair and TD-29
  malformed-line skip), tracker round-trip
  and capacity, dispatch tagging/pacing, the config table from 05 §2, and
  resolution cursor/summary/handler behavior, history-handler filtering, and
  backfill completion heuristics A/B + reset-without-done (76 tests,
  308 checks). Missing: UI and main-loop dispatch
  (`request_id_from_extra` is static in `src/main.c`; a
  `resolve_dest` misrouting regression there was caught only by live
  verification). The test and sanitize targets are POSIX-only.
- **Impact:** the remaining seams (UI, dispatch) still cannot be
  regression-tested automatically.
- **Structural fix (remaining):** dispatch tests via a
  seam for `request_id_from_extra`, UI tests, and a Windows story for the test
  target. Done: ASan/UBSan CI job (`.github/workflows/ci.yml`), `./nob sanitize`,
  backfill/handler tests.

#### TD-19 — OOM and silent-failure paths — **ADDRESSED**
- **Evidence:** `src/history.c:12` (rehash failure returns without resizing),
  `src/history.c:96-97` (drop), `src/forward.c:7` (job dropped),
  `src/tracker.c:116` (pending drop), `src/tracker.c:26` (save failure ignored),
  `src/config.c:19-22` (empty config exits silently).
- **Impact:** partial state, potential full-table probe loop, silent loss.
- **Resolution:** `ui_oom` (`src/ui.c:110-113`) routes every OOM status through
  `ui_status` with the exact prior texts; history/tracker/forward/poll use it.
  `src/config.c` keeps its `fprintf(stderr)` contract. The rehash-failure
  probe risk and silent save failures remain (behavior unchanged when memory
  is available).

#### TD-20 — Credential files and permissions
- **Evidence:** `config.json`, `config.bak` contain a live API hash; created
  with umask defaults; both gitignored (`.gitignore:5,7`).
- **Impact:** readable by other local users; duplicated secret on disk.
- **Fix:** document `chmod 600`, remove `config.bak`, consider key rotation;
  specs must never contain the values.

#### TD-27 — History rewrite is not crash-safe — **ADDRESSED (Step 4.4a)**
- **Evidence (pre-fix):** prune opened the file with `"w"` and rewrote it.
- **Impact:** crash/power loss during prune left a truncated or empty dedupe
  log → duplicates.
- **Resolution:** prune writes `<history_file>.tmp` + `rename`
  (`src/history.c:104-133`); 02 §3.5 and 05 §3.1 amended. Behavior-preserving
  for normal operation.

#### TD-29 — Uninitialized key on corrupt history lines — **ADDRESSED (Step 4.2)**
- **Evidence (pre-fix):** `sscanf` could write nothing for a whitespace-only
  line and the uninitialized `key` was hashed.
- **Impact:** undefined behavior on a corrupted or hand-edited file.
- **Resolution:** load skips lines where `sscanf` returns < 1
  (`src/history.c:42`); 02 §3.2 and 05 §3.1 amended; behavior change only for
  invalid input.

### P2

#### TD-11 — Dedupe key mismatch between read and write
- **Evidence:** read uses the message’s `chat_id` (`src/poll.c:137`), write uses
  `src_chat_id` (`src/forward.c:174`).
- **Impact:** none for channel posts; incorrect dedupe if TDLib ever returns a
  differing chat id.
- **Fix gate:** normalize on `src_chat_id`; amend 02 §2.2.

#### TD-13 — Unreconciled boundaries
- **Evidence:** album grouping is per batch (`src/forward.c:124-149`); cutoff uses
  message date (`src/poll.c:138`) while dedupe expiry uses enqueue time
  (`src/history.c:69`).
- **Impact:** split albums; a message can be inside the window yet have an
  expired dedupe entry.
- **Fix gate:** define the intended unit of grouping and the single time basis.

#### TD-14 — Conflated feature flag and permissive CLI — **ADDRESSED (Step 4.6)**
- **Evidence (pre-fix):** `enable_sequential_forwarding` controlled both
  backfill and sorting; any argv disabled the dashboard and only `-d` enabled
  debug, so typos silently changed UI mode.
- **Resolution:** new `enable_ordering` config key (05 §2) enables sorting
  without backfill (04 §1, 03 §2); `main` validates argv (only no-arg or `-d`)
  and prints `Usage: … [-d]` + exit 1 otherwise (06 §1).
- **Residual:** the dashboard state model is still TD-23.

#### TD-15 — Unhandled TDLib auth states
- **Evidence:** no branches for QR, registration, other-device confirmation,
  logout, closed (`src/auth.c:18-72`).
- **Impact:** inert process on those flows.
- **Fix gate:** explicit handling or a clear fatal error; amend 01 §2.3.

#### TD-16 — cJSON numeric precision and key casing
- **Evidence:** `cJSON_GetInt64` casts `valuedouble` (`src/tgf.h:110-116`);
  `cJSON_GetObjectItem` is case-insensitive (`cJSON.c:1983-1986`).
- **Impact:** `media_album_id` above 2^53 loses precision; config keys match
  unexpectedly.
- **Fix gate:** parse ids from raw text or patch cJSON; make key lookup
  case-sensitive; amend 05 §2.

#### TD-18 — Documentation and packaging drift — **ADDRESSED**
- **Evidence (pre-fix):** README said GPLv2 while `LICENSE` is GPLv3;
  “no system deps” ignored ncurses/CMake; `config.json.example` was invalid
  JSON (comment, unquoted `api_id`) and omitted `history_window_hours`.
- **Impact:** first-run failure and wrong expectations.
- **Resolution:** README license/deps/requirements/contributing corrected;
  example is valid JSON with all keys; 05 §2/§7/§8 amended. Version badge
  left at 1.3.1 — bumping/tagging is a release action (CONTRIBUTING).

#### TD-21 — Windows build probe and path assumptions
- **Evidence:** `nob.c:50-55` redirects to `/dev/null` under `cmd.exe`;
  `check_system_td` lists only Linux paths (`nob.c:25-30`); rpath uses
  `$ORIGIN` (`nob.c:213`).
- **Impact:** Windows/macOS builds are unverified and may fail before
  compiling.
- **Fix:** platform-specific redirect, library discovery, and loader-path
  flags; document supported platforms in 07.

#### TD-22 — Reply parent duplication
- **Evidence:** the parent id is injected into the child job but the parent is
  not marked grouped (`src/forward.c:101`, `116-123`).
- **Impact:** duplicate parent in the destination when the parent is in the
  same batch or was forwarded earlier.
- **Fix gate:** mark the parent grouped (changes historical behavior) or
  dedupe forwarded ids; amend 03 §3.

#### TD-23 — Dashboard UX
- **Evidence:** literal `ACTIVE` (`src/ui.c:59`), last-source-only line
  (`src/ui.c:62`, `63`), one status slot (`src/ui.c:64-67`), no resize handling, full
  `clear()` every second.
- **Impact:** misleading liveness, lost errors, poor small-terminal behavior.
- **Fix gate:** UI spec (state model, log region, resize).

#### TD-24 — Dead and vestigial code — **PARTIALLY ADDRESSED**
- **Evidence (remaining):** `last_msg_id`/`last_date` written, never read
  (`src/tracker.c:33-52`); `config_free` (`src/config.c:82-92`) used only by
  tests while production frees the same fields by hand (`src/main.c:164-168`);
  vestigial `(void)msg_id; (void)date;` in `tracker_store_set`
  (`src/tracker.c:34`).
- **Removed:** `pending_req`, the `on_update` auth path, the unreachable
  handler error branches, and the normal-path `batch_oldest_id` write.
- **Impact:** misleading readers, dead state, harder refactor.
- **Structural fix (remaining):** remove the rest after confirming routing; no
  behavior change.

#### TD-26 — Blocking auth input and signal handling quality
- **Evidence:** `fgets` inside handlers (`src/auth.c:35-58`); `signal()` not
  `sigaction` (`src/main.c:102-103`); up to 1 s shutdown latency.
- **Impact:** no cancellation, slow shutdown, prompt can outlive SIGINT.
- **Structural fix:** input step decoupled from the receive loop; signal-safe
  flag; same user-visible prompts.

#### TD-28 — CWD-relative, hardcoded paths
- **Evidence:** `config.json` (`src/main.c:89`), `tdlib_db`/`tdlib_files`
  (`src/auth.c:24-25`), `seq_tracker.txt` (`src/tracker.c:4`), default
  `history.txt` (`src/config.c:46`).
- **Impact:** running from another directory creates a second profile; no
  service-friendly layout.
- **Fix gate:** path resolution/`--config` feature spec.

## 3. Structural refactor plan (Phase 3)

Rules: each step compiles, passes the lint gate, and preserves every scenario
in 00–07. No behavior fixes ride along. Any spec edit in this phase is a sign
the step is too large.

### Step 0 — Freeze (done)
- `./nob test` added (`tests/test_main.c`, ASan/UBSan, temp CWD, no TDLib).
- Covered behavior, locked in as assertions:
  - `process_msgs`: wire order, sequential asc/desc, reply chains and parent
    duplication, album grouping/ordering/split, child and queue caps, enqueue
    marking;
  - `history.c`: add/has, window expiry, prune with probe-chain repair
    (TD-04 fixed), malformed-line skip at load (TD-29 fixed);
  - `tracker.c`: 3/4/5-field load, save round-trip, 128-entry cap, pending
    append/flush;
  - dispatch: `forwardMessages` payload, single/batch tags, delay gate;
  - resolution: `resolve_next` cursor/requests, `resolve_source_handle`,
    partial-failure WARNING summary, `resolve_dest_handle`, `getme_handle`
    (fatal exit paths are manual-only, SC-01-03/04/05);
  - history handler: normal-path filters (id-zero/dedupe/cutoff), wire order,
    empty no-op, sequential status; backfill collection/paging, echo skip,
    no-dedupe collection, completion A/B, reset-without-done, total-zero,
    straddle.
- Done (2026-11-19): backfill completion heuristics (04 §3.4–3.5) and the
  history handler are locked in by the characterization suite (`src/poll.c`
  is linked into `tests/tgf_tests`).
- `specs/acceptance.md` added: 60-scenario manual checklist for TDLib, UI,
  file, and restart behavior that unit tests cannot cover.
- Changing any existing assertion is a behavior change: amend the owning spec
  in the same change.

### Step 1 — Seams (no file moves yet)
1. **Done:** `Config` + `config_load(path, out)` in `config.c`/`config.h`;
   `tgf.c` keeps a thin `load_config` adapter that copies the values into the
   `Runtime` config. Messages, defaults, alloc/free rules, and exit behavior
   are unchanged; 12 config tests follow 05 §2.
2. **Done (Step 1b):** `Runtime` struct holding the current globals; `tgf.c`
   holds the single static `Runtime`, history/tracker/forward take
   `Runtime *`, and `set_status`/`transport_send` keep their signatures reading the
   static `Runtime`. No behavior change; tests hold a static `Runtime`.
3. **Done:** storage interfaces `history_store_*` (`load/add/has/prune`) and
   `tracker_store_*` (`load/save/get/set/pending`) with today’s formats.
4. **Done:** transport `transport_send(rt, client, type, payload, RequestId)`
   plus a handler registry (`handler_for`, `src/main.c:39-49`); `request_extra`
   translates `RequestId` back into the exact `@extra` strings on the wire
   (`src/tgf.h:93-108`). `transport_destroy` wraps client shutdown.
5. **Done:** pure grouping `group_batch(msgs, count, order) -> GroupResult`,
   reproducing qsort behavior, ascending intra-job ids, caps, and parent
   duplication.
6. **Done:** output sink `Ui` vtable (`ui_init`, `ui_status`, `ui_oom`,
   `ui_forwarded`, `ui_refresh`) with stderr and ncurses implementations.

### Step 2 — Split translation units
```
src/main.c        startup, loop, scheduling, shutdown
src/config.c      load/validate
src/auth.c        auth state machine + prompts
src/resolve.c     destination/source resolution
src/poll.c        poll request + history response
src/forward.c     grouping + queue (no globals)
src/history.c     dedupe store
src/tracker.c     sequential store
src/transport.c   TDLib JSON client wrapper
src/ui.c          status + dashboard
src/tgf.h         shared types only (Config, Runtime, ids)
```
Update `nob.c` source list; keep the same binary name and CLI.

**Done:** all ten modules plus `src/tgf.h` exist; `nob.c` compiles them with
`-I.`/`-Isrc`; `tests/test_main.c` stubs `transport_send`/`transport_destroy`/
`ui_status`/`ui_oom`/`ui_forwarded`. No behavior change; 308 checks pass.

### Step 3 — Structural cleanups (behavior-preserving) — **mostly done**
- Done: dead code removed (TD-24, partial — see register).
- Done: each response parsed once; handlers receive `cJSON *root` (auth still
  uses the raw string).
- Done: ring-buffer queue with head index and heap id payloads (TD-25),
  preserving FIFO order, pacing, and 256-id semantics.
- Done: `ui_oom` single failure path with the same status texts (TD-19).
- Done: `./nob sanitize` (ASan/UBSan) and `.github/workflows/ci.yml` (TD-17).
- Remaining: optional const-correctness/style cleanup of the cppcheck findings
  (07 §8); dispatch/UI tests and a Windows test story (TD-17).

### Step 4 — Behavior changes, one spec at a time
Order chosen by user risk:

1. **Done (Step 4.1, restored 2026-11-19):** resolution fail-fast/visible errors (TD-05, TD-07).
2. **Done (Step 4.2):** prune rehash + malformed-line rejection (TD-04,
   TD-29) — fixes duplicate forwards.
3. **Done (Step 4.3):** at-least-once delivery — in-flight slot, bounded
   retry, flood-wait pause (TD-01 partial, TD-06). Marking stays at enqueue;
   durability completes the guarantee in Step 4.4.
4. **Done (Step 4.4a/b):** durable queue + crash-safe history rewrite +
   durable pending journal (TD-03, TD-27, TD-02).
5. **Partially done (Step 4.5):** album split + truncation guard + cap
   warnings (TD-09). Reply-child excess and live pagination remain (Step 5).
6. **Partially done (Step 4.6):** flag/CLI split (TD-14 addressed).
   Dashboard states (TD-23) remain.

### Step 5 — Feature work
Only after the above: real-time updates, config paths, multi-destination,
filters — each with its own spec.

## 4. Behavior we must preserve until a spec says otherwise

These look like bugs but are the current contract. Removing them during the
structural refactor would violate Phase 2:

1. Dedupe at enqueue (TD-01).
2. Backfill bypasses dedupe (04 §3.2).
3. Reply parent included without marking/deduping it (TD-22).
4. Album grouping only within one batch (TD-13).
5. Batch ordering `asc`/`desc`, but ascending inside reply/album jobs (03 §2).
6. Silent config defaults and case-insensitive keys (05 §2).
7. One-job-per-tick pacing and integer-second delay (03 §5).
8. One-slot status line overwritten on the next refresh (06 §2.1).
9. CWD-relative paths (TD-28).
10. Backfill completion heuristics A/B and the “reset cursor without done”
    branch (04 §3.5).
11. ~~Literal `ACTIVE` and last-source-only dashboard (TD-23)~~ — amended in Step 4.1: dashboard doesn't start on resolution failure.
12. ~~Exit codes 0/1 and the exact status strings (06 §5)~~ — amended in Step 4.1: new exit-1 paths for resolution failures.

## 5. Definition of done for any debt item

- Owning spec amended first (or a new spec added).
- Build (`./nob`) and the narrow lint command pass.
- New/updated tests cover the changed clause, including the failure path.
- The relevant acceptance scenarios were run and their output recorded in the
  change description.
- If behavior changed, the before/after is visible in the spec diff.

## 6. Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline register created |
| 2026-11-17 | Refreshed `tgf.c` citations to the working-tree `Runtime` refactor; added `config_free` to TD-24; corrected the test count in TD-17 |
| 2026-11-17 | Step 2: split into `src/`; refreshed citations; marked Step 2 done |
| 2026-11-18 | TD-19 design: add `ui_oom(const char *msg)` in `src/ui.c` (declared in `src/tgf.h`) that calls `ui_status(msg)`; replace all `ui_status("OOM: ...")` calls in `src/forward.c`, `src/history.c`, `src/tracker.c`, `src/poll.c` with `ui_oom("OOM: ...")` preserving exact status text. `src/config.c` keeps its own `fprintf(stderr)` contract (tested separately). |
| 2026-11-19 | Step 3 marked mostly done (TD-19/TD-25 addressed, TD-24 partial, TD-17 CI + sanitize + resolve tests); Step 4.1 restored after loss in the seam rewrite (fatal resolution + clean client destroy + dashboard gate); fixed `resolve_dest` misrouting in `request_id_from_extra`; 42 tests / 175 checks; refreshed citations |
| 2026-11-19 | Step 4.2 done (TD-04 prune rehash, TD-29 load guard); TD-18 docs fixed; 02/05 amended first; tombstone-hole test assertion flipped to the fixed contract |
| 2026-11-19 | Step 3 leftover done: backfill completion + history-handler characterization tests (src/poll.c linked into the suite); 68 tests / 275 checks; no behavior change |
| 2026-11-19 | Step 4.3 done: at-least-once delivery (03 §5-§10, 02 §3.4, 00 §8/§9, 01 §6 amended first); TD-01 partial, TD-06 addressed; restart loss remains TD-03 |
| 2026-11-19 | Step 4.4a done: durable forward queue (05 §5) + crash-safe history prune (TD-03, TD-27 addressed); 05 sections renumbered; TD-02 remains (4.4b) |
| 2026-11-19 | Step 4.4b done: durable pending journal seq_pending.txt (05 §4.3, 04 amended); TD-02 addressed; Step 4.4 complete |
| 2026-11-19 | Step 4.5 done (partial): album split fixes stack overflow, truncation guard, tracker/page-full warnings (TD-09 partial); reply excess + pagination remain |
| 2026-11-19 | Step 4.6 done (partial): enable_ordering flag split + argv validation (TD-14 addressed); TD-23 dashboard states remain |
