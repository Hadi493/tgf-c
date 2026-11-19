# 05 — Configuration and On-Disk State

Status: baseline · Owner: `src/config.c` (`config_load`), `src/history.c`, `src/tracker.c`, TDLib | Depends on: 00

## 1. Location rules

- `config.json` is opened at the **hardcoded path** `config.json` in the
  process CWD (`src/main.c:84`). There is no flag or environment override.
- `tdlib_db/` and `tdlib_files/` are hardcoded relative paths passed to TDLib
  (`src/auth.c:24-25`).
- `history_file` defaults to `history.txt`; a relative value is resolved
  against the CWD.
- `seq_tracker.txt` is a hardcoded relative path (`src/tracker.c:4`, `25`).
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
- `history_window_hours` is not present in `config.json.example`, so a user
  editing the example silently gets 24.
- The example contains a `//` comment **inside the array** and is therefore not
  valid JSON; following the README quick start without removing it exits with
  `Invalid JSON in config.json` (TD-18). Trailing text after a complete root
  value is ignored, because `cJSON_Parse` does not require the input to be
  fully consumed (`cJSON.c:1227-1230`).

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
- Rewritten wholesale by pruning (`src/history.c:119-125`), which **truncates
  first** (`"w"`), so a crash during rewrite loses the file.
- Load requires only a whitespace-delimited key; a missing timestamp becomes 0.
  Lines longer than 127 bytes are split by `fgets`, producing garbage entries.
- A non-empty line that does not start with a token (e.g. only spaces) leaves
  the local `key` buffer unwritten before hashing → undefined behavior on
  corrupt input (`src/history.c:41-46`).
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
- Only loaded when sequential mode is enabled (`src/main.c:95`).
- Missing/corrupt file → empty tracker; a line with 3 fields is accepted with
  `scan_from=0, backfill_done=0`.

### 4.2 Capacity

128 rows. Extra rows are neither loaded nor created; behavior degrades silently
(04 §2.5).

## 5. TDLib state

| Path | Content | Owner |
|---|---|---|
| `tdlib_db/db.sqlite` (+`-wal`, `-shm`) | TDLib message cache and session metadata | TDLib |
| `tdlib_db/td.binlog` | MTProto auth keys / session | TDLib |
| `tdlib_db/temp/`, `tdlib_db/thumbnails/` | TDLib scratch | TDLib |
| `tdlib_files/` | downloaded files (none are requested by TGF) | TDLib |

TGF only sets `database_directory` and `files_directory` in
`setTdlibParameters` (`src/auth.c:21-34`). Directory creation, schema, and
migration are TDLib’s concern. Deleting `tdlib_db/` forces a new login.

## 6. Secrets and repository hygiene

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

## 7. Edge cases

| Case | Behavior |
|---|---|
| Empty `config.json` | exit 1, no message |
| Truncated JSON | `Invalid JSON`, exit 1 |
| Comment inside the root value (as in the example) | `Invalid JSON`, exit 1 |
| Trailing text after a complete root value | Accepted; the trailing bytes are ignored |
| `"api_id": "123"` | `Missing api_id`, exit 1 |
| `"enable_sequential_forwarding": "true"` | Treated as false |
| `"history_window_hours": -1` | Unlimited |
| `"forward_delay_sec": -5` | No spacing |
| `source_channels: []` | `ERROR: No source channels configured`, exit 1 (01 §1) |
| `source_channels: [1, null]` | Becomes `["", ""]`; resolution errors |
| `history_file` in a missing directory | Runs; dedupe in memory only |
| `seq_tracker.txt` read-only | Saves fail silently; state repeats on restart |
| CWD changed between runs | Different state entirely |

## 8. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-05-01 | `cp config.json.example config.json` then run | `Invalid JSON in config.json`, exit 1 |
| SC-05-02 | Remove the comment and fill credentials | Starts normally |
| SC-05-03 | Add `"API_ID": …` instead of `api_id` | Accepted (case-insensitive) |
| SC-05-04 | Use string `"true"` for sequential | Backfill does not start |
| SC-05-05 | Delete `history.txt` between runs | Duplicates possible within the window |
| SC-05-06 | Kill during prune rewrite | Truncated/empty `history.txt` |
| SC-05-07 | Run with `history_window_hours: 0` | No prune lines; `history.txt` grows |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c` and `.gitignore` citations to the working tree |
| 2026-11-17 | Step 2: moved config/storage into `src/`; refreshed citations |
| 2026-11-19 | Step 4.1 completion: empty source array is now fatal at startup (01 §1); refreshed citations; `.gitignore` gained `tests/tgf_tests` |
