# 02 — Polling, History Cutoff, Deduplication, Pruning

Status: baseline · Owner: `src/poll.c` (`poll_channels`, `history_response`), `src/history.c` · Depends on: 01, 05

## 1. Poll contract

Polling is time-driven, never event-driven. `poll_channels` (`src/poll.c:7-17`)
runs when the gate in §1.1 is true and `now - last_poll >= 5`
(`src/main.c:121-127`). There is no `updateNewMessage` subscription; new posts are
discovered only by re-reading history.

### 1.1 Gate

```
authorized && dest_resolved && source_count >= 1 && !resolve_active
```

### 1.2 Request

For each `i` in `0..num_sources-1` with `source_chat_ids[i] != 0`:

```
{"@type":"getChatHistory",
 "chat_id":<source_chat_ids[i]>,
 "limit":10000,
 "from_message_id":<scan_from or 0>,
 "offset":0,
 "@extra":"history_<source_chat_ids[i]>"}
```

- `MSG_LIMIT` is 10000 (`src/tgf.h:14`).
- `from_message_id` is `seq_tracker_get_scan_from(chat_id)` only in sequential
  mode; otherwise 0 (`src/poll.c:11`).
- `offset:0` with a `from_message_id` means the requested message is included
  in the response (this is why the backfill path skips it explicitly, 04 §3.3).
- All sources are requested in the same tick (a burst of up to N requests).
- Responses are newest-first and capped at 10,000 messages. In non-sequential
  mode there is no pagination: if more than 10,000 messages appeared since the
  previous poll, the excess is never seen (TD-09).

## 2. Response handling

`history_<chat_id>` responses go to `history_response`
(`src/poll.c:24-150`), which parses `"messages"` (array). Missing or non-array →
silently ignored. `total == 0` is handled only by the sequential backfill
completion path (04 §3.5); in normal mode it is a no-op.

`src_chat_id` comes from the `@extra` tag, not from the payload
(`src/main.c:27`, `src/poll.c:19-22`).

Memory ownership: `main.c` parses each response once (`cJSON_Parse`) and calls
`cJSON_Delete(root)` exactly once after the handler returns
(`src/main.c:105-116`). Response handlers, including `history_response`, read
from `root` but must not free it; deleting `root` in a handler double-frees
(`src/poll.c` backfill and completion paths previously did).

### 2.1 Cutoff

```
cutoff = history_window_hours > 0
       ? time(NULL) - history_window_hours * 3600
       : 0
```

(`src/poll.c:44-45`). A message is dropped when `cutoff != 0 && msg_date < cutoff`
(`src/poll.c:78`, `134`). The comparison uses the **message’s `date` field**, not
the time the program saw it. `0` or negative disables the cutoff.

The same window value also controls dedupe-entry validity (§3.3), where the
clock is **enqueue time**. The two clocks are independent by design.

### 2.2 Normal-path algorithm

When sequential mode is off, or backfill is already complete
(`src/poll.c:54-120` is skipped, `122-149` runs):

1. `calloc(total, sizeof(PendingMsg))`; failure → status `OOM in history
   handler`, return (`src/poll.c:122-123`).
2. Iterate the response array in wire order (newest first). For each element:
   - read `id`, `media_album_id`, `chat_id`, `date`;
   - skip if `id == 0`;
   - skip if `history_has(chat_id, id)` is true;
   - skip if below the cutoff (do **not** count it);
   - copy `id`, `album_id`, `date`, and `reply_to.message_id` into the buffer;
   - increment `new_count`.
3. If sequential mode is on, publish `New: <n> msgs, cutoff=<cutoff>` as a
   status message (`src/poll.c:143-144`).
4. If `new_count == 0`, free and return.
5. `process_msgs(src_chat_id, new_msgs, new_count)` (03), then free.

Notes:

- Filters run in the order id-zero → dedupe → cutoff.
- Messages in one response are not deduplicated against each other; TDLib
  returns unique ids.
- `history_has` uses the **message’s own `chat_id` field**
  (`src/poll.c:130`, `133`), while `process_msgs` records with `src_chat_id`
  (`src/forward.c:123`). For channel posts these are identical. For any response
  where they differ, dedupe and recording use different keys (TD-11).
- Memory: one temporary array of up to 10,000 `PendingMsg` (32 B each) per
  response.
- The response order (newest first) is preserved into `process_msgs` when
  sequential mode is off, so forwarding is newest-first.

## 3. Dedupe set (`history.c`)

### 3.1 Key and table

- Key: `"<chat_id>_<message_id>"` (`src/history.c:62`, `87`).
- Hash: 32-bit FNV-1a (`src/history.c:3-7`).
- Collision: linear probing with mask `size - 1`; size is always a power of
  two (`src/history.c:15-16`, `45`, `63`).
- Load factor: rehash to double size when
  `count >= set_size * 3 / 4` before insertion
  (`src/history.c:43-44`, `84-85`).
- Allocation: 16,384 slots on first use (`src/history.c:32`, `79`).
- Entries are `strdup`ed; `time[i]` is the enqueue timestamp.

### 3.2 Load

`history_store_load` (`src/history.c:28-57`):

- Missing file → return with the table absent (`set == NULL`; all lookups
  return 0).
- Reads lines up to 127 chars; format `"%63s %lld"`; blank lines and lines
  with no parsable token (e.g. whitespace-only) are skipped — the key buffer
  is never hashed uninitialized (TD-29, fixed).
- Duplicate keys are skipped (first timestamp wins).
- A line with an unparsable timestamp yields `ts = 0`.
- Does not apply the window at load time; expiry is evaluated on lookup.

### 3.3 Lookup and expiry

`history_store_has` (`src/history.c:59-75`): probe until an empty slot. On key match:

| Condition | Result |
|---|---|
| `history_window_hours <= 0` | seen (1) |
| stored timestamp `== 0` | seen (1) — permanent legacy entry |
| `now - timestamp < window * 3600` | seen (1) |
| otherwise | not seen (0) |

An expired entry is **not removed by lookup**; it stays until pruning.

### 3.4 Add

`history_store_add` (`src/history.c:77-102`):

- Allocate if needed.
- Rehash at the load-factor threshold.
- If the key exists: refresh its timestamp to `now`, do not append a file line.
- Else: insert, set timestamp `now`, increment `count`, append
  `"<key> <now>\n"` to `history_file`.
- Called from `process_msgs` **before** the job can be dispatched
  (`src/forward.c:122-124`). Marking therefore means "enqueued", not
  "delivered" (TD-01).
- On OOM (`strdup` failure) the entry is silently not recorded; the file is not
  written.

### 3.5 Prune

`history_store_prune` (`src/history.c:104-127`) runs from the main loop when
`now - last_prune >= 1200` (20 min, `src/main.c:144-145`). `last_prune` starts at 0,
so a prune runs on the **first loop iteration** (before authorization) and
every 20 minutes after that.

- No-op when the table is absent or `history_window_hours <= 0`.
- For every slot with `timestamp > 0 && timestamp < now - window*3600`:
  free, set slot to NULL, decrement `count`.
- If anything was pruned, rehash the survivors into a fresh table of the same
  size (`history_rehash`), eliminating probe-chain holes (TD-04, fixed), then
  rewrite the file with live entries only. If the rehash allocation fails,
  `ui_oom` reports and the holes remain until the next successful prune
  (degraded, pre-fix behavior).
- Publish `History pruned <n> old entries`.

### 3.6 Probe-chain repair (TD-04, fixed)

Setting a slot to NULL in an open-addressing linear-probe table terminates
lookup chains for any key that hashed at or before that slot and resides after
it. Before the fix, such keys were reported as unseen after a prune and could
be forwarded again. Prune now rehashes survivors into a fresh table (§3.5), so
live keys stay reachable. Entries with `timestamp == 0` are never pruned, which
masks nothing but leaks unbounded file lines.

## 4. Interaction between cutoff and dedupe

The design assumes messages arrive soon after publication. If a message’s
`date` is inside the window but its dedupe entry has expired, it can be
forwarded again. This requires a dedupe age older than the window while the
message date is still inside it (clock skew, very long queues, or a window
change between runs). Both checks are independent and there is no reconciliation
(TD-13).

Changing `history_window_hours` between runs changes both behaviors at once:

- smaller window → older messages are dropped, older dedupe entries expire;
- `0` → no cutoff and no expiry/pruning.

## 5. Edge cases

| Case | Behavior |
|---|---|
| `history.txt` missing | Empty set; all in-window messages forwarded |
| `history.txt` empty | Same |
| Entry timestamp 0 | Treated as seen forever; never pruned |
| Duplicate file line | First timestamp retained |
| Window negative | Unlimited (same as 0) |
| Prune with no expired entries | No file rewrite, no status |
| >10,000 new messages between polls | Oldest excess never seen (non-sequential) |
| Source configured twice | Same chat polled twice; second response is deduped |
| Message `date` missing/0 | `0 < cutoff` → dropped when a cutoff is active |
| `media_album_id` missing | Album id 0 (treated as single message) |
| Table full after failed rehash | Undefined probe behavior; no guard (TD-19) |
| Whitespace-only `history.txt` line | Skipped at load (TD-29, fixed) |

## 6. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-02-01 | Fresh state, one source with recent posts, run `-d` | `New: N msgs, cutoff=…`; posts appear in destination newest-first |
| SC-02-02 | Immediately restart | No duplicates; `New: 0 msgs` |
| SC-02-03 | Set `history_window_hours` smaller than a post’s age | Post not forwarded; cutoff line reflects it |
| SC-02-04 | Set `history_window_hours: 0` | Old posts are forwarded; no prune messages ever |
| SC-02-05 | Run 20+ min | `History pruned N old entries` once expired entries exist; live messages are not re-forwarded afterwards (probe chains repaired) |
| SC-02-06 | Corrupt a timestamp to `abc` | Entry is treated as permanent (timestamp 0) |
| SC-02-07 | Delete `history.txt` but keep tracker | Non-sequential: duplicates possible; sequential complete: duplicates possible (04) |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c` citations to the working-tree `Runtime` refactor |
| 2026-11-17 | Step 2: moved polling/history into `src/`; refreshed citations |
| 2026-11-18 | Documented single `root` ownership (`main.c` parses + `cJSON_Delete` once; handlers must not free `root`) and removed the double-`cJSON_Delete` on `history_response` backfill/complete/OOM paths in `src/poll.c` |
| 2026-11-19 | As-built refresh: normal path no longer writes `batch_oldest_id` (TD-24); OOM status routed via `ui_oom` (TD-19, same text); refreshed citations |
| 2026-11-19 | Step 4.2: prune rehashes survivors (TD-04 fixed — fewer duplicates); load skips lines with no parsable token (TD-29 fixed); refreshed store field names; SC-02-05 extended |
