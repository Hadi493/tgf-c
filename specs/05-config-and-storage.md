# 05 — Configuration and On-Disk State

Status: baseline · Owner: `src/config.c` (`config_load`), `src/history.c`, `src/tracker.c`, TDLib | Depends on: 00

## 1. Location rules

- `config.json` is opened at the **hardcoded path** `config.json` in the
  process CWD (`src/main.c:86`). There is no flag or environment override.
- `tdlib_db/` and `tdlib_files/` are hardcoded relative paths passed to TDLib
  (`src/auth.c:24-25`).
- `history_file` defaults to `history.txt`; a relative value is resolved
  against the CWD.
- `seq_tracker.txt` is a hardcoded relative path (`src/tracker.c:4`, `25`).
- `fwd_queue.txt` is a hardcoded relative path (§5, `src/forward.c`).
- `seq_pending.txt` is a hardcoded relative path (§4.3, `src/tracker.c`).
- Running from a different directory is equivalent to a fresh installation:
  empty dedupe set, empty tracker, new TDLib session.

No XDG/`$HOME` support exists.

## 2. `config.json` schema

Loaded by `config_load` (`src/config.c:9-77`) with cJSON. All keys are looked up
**case-insensitively** because cJSON’s `cJSON_GetObjectItem` is case-insensitive
(`cJSON.c:1983-1986`); duplicate keys resolve to the first match.

| Key | JSON type | Required | Default | Validation | On invalid |
|---|---|---|---|---|---|
| `api_id` | number | yes | — | `cJSON_IsNumber` | `Missing api_id`, exit 1 |
| `api_hash` | string | yes | — | `cJSON_IsString` | `Missing api_hash`, exit 1 |
| `dest_channel` | string | yes | — | `cJSON_IsString` | `Missing dest_channel`, exit 1 |
| `source_channels` | array of strings | yes | — | `cJSON_IsArray` | `Missing source_channels array`, exit 1 |
| `history_file` | string | no | `history.txt` | none | non-string → default, silently |
| `forward_delay_sec` | number | no | `1` | none | non-number → default, silently |
| `history_window_hours` | number | no | `24` | none | non-number → default, silently |
| `enable_sequential_forwarding` | **boolean** | no | `false` | `cJSON_IsBool` | non-bool (e.g. `"true"`) → false, silently |
| `sequence_direction` | string | no | `"asc"` | exact value `"desc"` | anything else → asc, silently |

Notes:

- `api_id` is cast from a `double` to `int`; fractional values are truncated.
- Numeric values elsewhere are cast from `double`; cJSON stores every number as
  `double`, so integers above 2^53 lose precision (TD-16). This affects only
  `api_id` in this schema.
- `source_channels` elements that are not strings become `""`; an empty
  username fails resolution and is skipped.
- No range checks: negative/zero delay disables spacing; negative/zero window
  disables cutoff and expiry; very large arrays are attempted with `calloc`.
- Unknown keys are ignored.
- `config.json.example` is valid JSON and includes `history_window_hours`
  (TD-18, fixed). `api_id` uses the numeric placeholder `123456`; replace it
  and `api_hash` with real credentials before running.
- Trailing text after a complete root value is ignored, because `cJSON_Parse`
  does not require the input to be fully consumed (`cJSON.c:1227-1230`).
  Comments are still rejected: a `//` inside a config exits with
  `Invalid JSON in config.json`.

### 2.1 Error messages

| Condition | Message | Exit |
|---|---|---|
| file missing or unopenable | `Cannot open config.json` | 1 |
| empty file | none (returns -1) | 1 |
| parse failure | `Invalid JSON in config.json` | 1 |
| missing required field | `Missing <field>` | 1 |
| allocation failure | `OOM` (some early allocations return silently) | 1 |

All go to stderr.

### 2.2 Example

```json
{
  "api_id": 123456,
  "api_hash": "…",
  "source_channels": ["@channel1", "@channel2"],
  "dest_channel": "@me",
  "history_file": "history.txt",
  "forward_delay_sec": 1,
  "enable_sequential_forwarding": true,
  "sequence_direction": "asc",
  "history_window_hours": 24
}
```

## 3. `history.txt` — dedupe log

### 3.1 Format

```
<chat_id>_<message_id> <enqueue_unix_seconds>\n
```

Example: `-1001394595107_46548385792 1794488266`.

- One line per newly recorded message.
- Appended with `fprintf` and `fclose` per message (`src/history.c:100-101`). No
  `fsync`, no buffering across messages.
- Timestamp is when the message was **enqueued for forwarding**, not when it
  was delivered.
- Rewritten by pruning via `<history_file>.tmp` + `rename`
  (`src/history.c:104-133`), so a crash during rewrite leaves the previous
  file intact (TD-27, addressed).
- Load requires only a whitespace-delimited key; a missing timestamp becomes 0.
  Lines longer than 127 bytes are split by `fgets`, producing garbage entries.
- A line with no parsable token (e.g. only spaces) is skipped at load
  (`src/history.c:42`); the key buffer is never hashed uninitialized
  (TD-29, fixed).
- Missing file is normal. A missing directory makes appends fail silently
  (dedupe then works only in memory).

### 3.2 Lifetime

- Entries are valid for `history_window_hours` after enqueue (02 §3.3).
- `0`/negative window makes entries permanent and disables pruning.
- Prune runs every 20 minutes, rewrites the file, and leaves probe-chain holes
  (TD-04).
- Timestamp `0` entries are permanent and never pruned.

## 4. `seq_tracker.txt` — sequential state

### 4.1 Format

```
<chat_id> <last_msg_id> <last_date> <scan_from_id> <backfill_done>\n
```

- `last_msg_id` / `last_date`: informational maxima maintained during flush;
  not read by any decision.
- `scan_from_id`: pagination cursor; 0 means “start from newest”.
- `backfill_done`: 0/1.
- Written through `seq_tracker.txt.tmp` + `rename` (`src/tracker.c:24-31`).
- Pending buffers are **not** persisted.
- Only loaded when sequential mode is enabled (`src/main.c:98`).
- Missing/corrupt file → empty tracker; a line with 3 fields is accepted with
  `scan_from=0, backfill_done=0`.

### 4.2 Capacity

128 rows. Extra rows are neither loaded nor created; behavior degrades silently
(04 §2.5).

### 4.3 Pending journal (`seq_pending.txt`)

The backfill pending buffer is journaled so a crash mid-backfill does not lose
collected messages (TD-02, addressed; Step 4.4b).

- Format, one line per collected message:
  `<chat_id> <id> <album_id> <reply_to_msg_id> <date>`.
- Appended (`fprintf` + `fclose`, no `fsync`) by `tracker_store_pending_append`
  each time a message is pushed to a chat's RAM buffer. A crash can lose the
  last unflushed appends (same window as `history.txt`).
- On flush (`flush_pending`), the chat's lines are removed by rewriting
  `seq_pending.txt.tmp` + `rename` (compaction).
- Loaded at startup **only in sequential mode**, after `tracker_store_load`
  (`seq_pending_load`); records are pushed to the RAM buffers without
  re-journaling. Missing file → empty. A line with fewer than 5 fields is
  skipped.
- Save/append failure is silent; the RAM buffer still works for the run.

## 5. Durable forward queue (`fwd_queue.txt`)

The forward queue and the in-flight job are persisted so a restart resumes
delivery (TD-03, addressed; Step 4.4a).

### 5.1 Format

One line per job, queued jobs in FIFO order, then the in-flight job (if any):

```
<src_chat_id> <retries> <id1> <id2> ...
```

- `retries` is the failed-attempt count (03 §7); ids are space-separated,
  at most `MAX_FWD_IDS` (256) per line.
- Written via `fwd_queue.txt.tmp` + `rename` after **every** queue mutation:
  enqueue batch (`process_msgs`), dispatch, confirm, requeue, drop.
- The in-flight job is included until confirmed; on load it becomes a queued
  job again, so a crash between send and response can re-forward it
  (at-least-once duplicate, 03 §7).

### 5.2 Load

- At startup, after `history_store_load` (`src/main.c`), always (not gated on
  sequential mode).
- Missing file → empty queue (normal first run).
- A line with fewer than 3 tokens (chat, retries, ≥1 id) is skipped; ids
  beyond 256 are truncated by `fwd_queue_push`; negative retries clamp to 0.
- Loaded ids are already in `history.txt`, so polls do not re-enqueue them.
- Save failure is silent (like the tracker); the queue remains in RAM and is
  lost on restart. Load OOM skips the job (`ui_oom`).

## 6. TDLib state

| Path | Content | Owner |
|---|---|---|
| `tdlib_db/db.sqlite` (+`-wal`, `-shm`) | TDLib message cache and session metadata | TDLib |
| `tdlib_db/td.binlog` | MTProto auth keys / session | TDLib |
| `tdlib_db/temp/`, `tdlib_db/thumbnails/` | TDLib scratch | TDLib |
| `tdlib_files/` | downloaded files (none are requested by TGF) | TDLib |

TGF only sets `database_directory` and `files_directory` in
`setTdlibParameters` (`src/auth.c:21-34`). Directory creation, schema, and
migration are TDLib’s concern. Deleting `tdlib_db/` forces a new login.

## 7. Secrets and repository hygiene

- `config.json`, `config.bak`, `history.txt`, `seq_tracker.txt`, `doc/`,
  `Issues.md`, and both binaries are gitignored
  (`.gitignore:1-13`).
- Only 16 files were tracked at baseline; no credentials are tracked.
- `config.json` holds a live API hash. It must never be pasted into specs,
  issues, or commits. `config.bak` is a second copy with the same secret.
- Files are created with the process umask (typically `0644`); the TDLib
  session file is `0600`. If the machine is shared, tighten permissions on
  `config.json` and `config.bak` (TD-20).
- Rotating the API hash invalidates both files and requires a TDLib re-login
  only if the app credentials are rejected.

## 8. Edge cases

| Case | Behavior |
|---|---|
| Empty `config.json` | exit 1, no message |
| Truncated JSON | `Invalid JSON`, exit 1 |
| Comment inside the root value | `Invalid JSON`, exit 1 |
| Trailing text after a complete root value | Accepted; the trailing bytes are ignored |
| `"api_id": "123"` | `Missing api_id`, exit 1 |
| `"enable_sequential_forwarding": "true"` | Treated as false |
| `"history_window_hours": -1` | Unlimited |
| `"forward_delay_sec": -5` | No spacing |
| `source_channels: []` | `ERROR: No source channels configured`, exit 1 (01 §1) |
| `source_channels: [1, null]` | Becomes `["", ""]`; resolution errors |
| `history_file` in a missing directory | Runs; dedupe in memory only |
| `seq_tracker.txt` read-only | Saves fail silently; state repeats on restart |
| `fwd_queue.txt` missing | Empty queue on startup (normal first run) |
| `fwd_queue.txt` corrupt line | Line skipped; valid lines loaded |
| `fwd_queue.txt` save failure | Silent; queue still in RAM, lost on restart |
| `seq_pending.txt` missing | Empty pending on startup (normal) |
| `seq_pending.txt` corrupt line | Line skipped at load |
| Kill mid-backfill | Journaled pending survives; resume collects older messages and flushes all |
| CWD changed between runs | Different state entirely |

## 9. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-05-01 | `cp config.json.example config.json` then run with placeholders | Config accepted (valid JSON); auth proceeds to the phone prompt — credentials are not validated locally |
| SC-05-02 | Fill real credentials | Starts normally |
| SC-05-03 | Add `"API_ID": …` instead of `api_id` | Accepted (case-insensitive) |
| SC-05-04 | Use string `"true"` for sequential | Backfill does not start |
| SC-05-05 | Delete `history.txt` between runs | Duplicates possible within the window |
| SC-05-06 | Kill during prune rewrite | Previous `history.txt` intact (tmp+rename) |
| SC-05-07 | Run with `history_window_hours: 0` | No prune lines; `history.txt` grows |
| SC-05-08 | Kill with jobs queued, restart | Queue resumes from `fwd_queue.txt`; queued posts forwarded (in-flight may duplicate) |
| SC-05-09 | Kill mid-backfill, restart | Journaled pending survives; backfill resumes and flushes collected messages |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c` and `.gitignore` citations to the working tree |
| 2026-11-17 | Step 2: moved config/storage into `src/`; refreshed citations |
| 2026-11-19 | Step 4.1 completion: empty source array is now fatal at startup (01 §1); refreshed citations; `.gitignore` gained `tests/tgf_tests` |
| 2026-11-19 | TD-18 fixed: config.json.example is valid JSON with history_window_hours and a numeric api_id placeholder; README license/deps corrected; TD-29 load guard documented; SC-05-01/02 updated |
| 2026-11-19 | SC-05-01 expectation corrected to observed behavior: placeholder credentials pass local config validation; TDLib proceeds to the phone prompt |
| 2026-11-19 | Step 4.4a: new §5 durable forward queue (fwd_queue.txt); history prune rewrite is tmp+rename (TD-27); TD-03 addressed; sections renumbered 5-8 → 6-9; SC-05-06 updated, SC-05-08 added |
| 2026-11-19 | Step 4.4b: new §4.3 pending journal (seq_pending.txt); TD-02 addressed; SC-05-09 added |
