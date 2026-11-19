# 04 — Sequential Mode and Backfill

Status: baseline · Owner: `src/tracker.c`, `src/poll.c` (`history_response`), `src/forward.c` (`process_msgs` sorting) · Depends on: 02, 03

## 1. What the flag means

`enable_sequential_forwarding` controls the **backfill phase**: before
anything is forwarded, collect the channel’s history window into a pending
buffer (journaled, 05 §4.3), paging backwards, then forward it in one ordered
pass. It also enables ordering.

`enable_ordering` (05 §2) enables **ordering only** — sort each `process_msgs`
batch by date (asc or desc) — without backfill, tracker load/save, or paging
(TD-14 addressed). Ordering is active when either flag is on.

When both flags are off there is no tracker load/save, no paging, no pending
buffer, and no explicit sort (wire order, newest-first).

`sequence_direction` (`"asc"` default, `"desc"`) only affects the sort.

## 2. Tracker file and API

### 2.1 Schema

`seq_tracker.txt` (`src/tracker.c:24-31`):

```
<chat_id> <last_msg_id> <last_date> <scan_from_id> <backfill_done>
```

Example: `-1002498030279 41520463872 1784622155 0 1`.

### 2.2 Load

`tracker_store_load` (`src/tracker.c:3-22`):

- Missing file → empty tracker.
- Reads up to `MAX_SEQ_TRACKER` (128) valid lines; extra lines ignored.
- `sscanf` with 5 fields; a line with **at least 3** fields is accepted.
- Missing field 4 → `scan_from_id = 0`; missing field 5 →
  `backfill_done = 0`.
- No validation of ids, ordering, duplicates, or trailing garbage.

### 2.3 Save

`tracker_store_save` (`src/tracker.c:24-31`): write `seq_tracker.txt.tmp`, then
`rename` over `seq_tracker.txt`. No `fsync`, no error reporting if `fopen` or
`rename` fails. Called after tracker mutations in the history handler and at
the end of `process_msgs` when sequential mode is on.

### 2.4 Entry operations

| Function | Behavior |
|---|---|
| `tracker_store_get_scan_from` | linear find; 0 if absent |
| `tracker_store_set_scan_from` | update first match; append a zeroed entry if absent and count < 128; otherwise silent no-op |
| `tracker_store_is_backfill_done` | 0 if absent |
| `tracker_store_set_backfill_done` | update first match; append if absent and count < 128 |
| `tracker_store_set` | update `last_msg_id`/`last_date` to the max; append if absent |
| `tracker_store_pending_append` | find entry; grow pending array (512 then ×2); on OOM status and drop; if no entry, **drop silently**; on success append a record to `seq_pending.txt` (05 §4.3) |
| `flush_pending` | if pending_count > 0, `process_msgs`; then free and zero the buffer and compact `seq_pending.txt` (remove the chat's lines) |

`last_msg_id` and `last_date` are written but never read for any decision.

### 2.5 Capacity

128 entries (`src/tgf.h:16`). At capacity, new chats are not tracked:
`pending_append` drops every message and `set_scan_from` is a no-op, so the
same page is requested on every poll with no progress. The history handler
publishes `WARNING: tracker full (128); chat not tracked` when it cannot create
an entry (TD-09 partial).

### 2.6 Entry creation

The history handler creates a tracker row the first time it sees a chat with
`scan_from == 0` during backfill (`src/poll.c:60-71`), provided the capacity
allows it.

## 3. Backfill algorithm

Runs when sequential mode is on and `backfill_done == 0` for the chat
(`src/poll.c:55-123`).

### 3.1 Polling position

`poll_channels` asks for `from_message_id = scan_from_id` (02 §1.2), which is
0 on the first request. `getChatHistory` includes the reference message, so the
response echoes the id last stored in `scan_from_id`.

### 3.2 Collection pass (per response)

For each message in wire order (newest first):

1. record `batch_oldest_id = message.id` (ends as the oldest id in the page);
2. skip `id == 0`;
3. skip if `id == scan_from_id` (the inclusive echo);
4. skip if below the cutoff, incrementing `cutoff_filtered`;
5. otherwise append `{id, album_id, date, reply_to_msg_id}` to the chat’s
   pending buffer and increment `new_count`.

**No dedupe lookup is performed in this branch.** History is consulted only
after backfill completes.

Publishes `Backfill: <new> msgs, cutoff=<cutoff>` as a status line.

### 3.3 Paging

If not complete and `new_count > 0`:
`scan_from_id = batch_oldest_id`, save. The next poll resumes from the oldest
message of this page.

If not complete and `new_count == 0`: save only (no rescan state change).

### 3.4 Completion conditions

Exactly two, both evaluated on `new_count == 0`:

| # | Condition | Meaning |
|---|---|---|
| A | `cutoff != 0 && cutoff_filtered > 0 && new_count == 0` | a whole page is older than the window |
| B | `new_count == 0 && total > 0 && cutoff_filtered == 0` | a page contained only the inclusive echo (end of chat) |

`total == 0` is handled earlier (`src/poll.c:31-39`) and also completes the
backfill.

A page that straddles the cutoff (some new, some filtered) is **not** complete;
it advances `scan_from` to an already-too-old id and requires one more request
before condition A fires.

### 3.5 Completion actions

On completion, with `has_pending` meaning this chat’s `pending_count > 0`
(`src/poll.c:96-113`):

| `has_pending` | `cutoff_filtered` | Actions |
|---|---|---|
| yes | any | `flush_pending`; `backfill_done = 1`; `scan_from = 0` |
| no | > 0 | `backfill_done = 1`; `scan_from = 0` |
| no | 0 | `scan_from = 0` only; **`backfill_done` stays 0** |

The third row is the restart-with-lost-pending case: the next poll restarts
from the newest message, collects everything again, and (because history was
already written for the first pass, but is not consulted during backfill)
forwards duplicates. See §5.

Also on `total == 0` (`src/poll.c:31-39`): flush pending, set `backfill_done = 1`,
`scan_from = 0`, save — unconditionally when the chat is not already done.

### 3.6 Flush

`flush_pending` (`src/tracker.c:165-178`) calls
`process_msgs(chat_id, pending, pending_count)`, which:

- sorts by date/id asc or desc (03 §2);
- groups replies/albums, marks history, enqueues forward jobs;
- saves the tracker.

The pending buffer is freed and reset, and the chat's journal records are
compacted away (05 §4.3). Nothing is forwarded until this point; the entire
backfill is silent apart from status lines.

### 3.7 Post-backfill steady state

Once `backfill_done == 1`, the normal path (02 §2.2) runs on every poll:
`from_message_id = 0`, dedupe + cutoff, then `process_msgs` with the sort still
active (because the flag is on). So live batches are ordered asc/desc within
the batch.

## 4. Interaction with other features

- **Cutoff** applies during collection, so the backfill covers exactly the
  messages newer than `history_window_hours` (or the entire channel when 0).
- **Dedupe** is written during flush and in live mode; it is not read during
  backfill.
- **Forward delay** applies only to draining the queue after flush; the
  backfill itself can enqueue thousands of jobs instantly (03 §5).
- **Prune** can run while a backfill is in progress; it only affects the
  dedupe file, not the pending buffer.
- **Restart** loads tracker rows, including `backfill_done` and `scan_from`,
  and replays `seq_pending.txt` into the pending buffers (05 §4.3); collected
  messages survive a crash (TD-02 addressed).

## 5. Durability and correctness limits

| Event | Result |
|---|---|
| Kill mid-backfill | Journaled pending survives (05 §4.3); `scan_from` persisted, so the next run collects older messages and flushes all collected ones (no loss beyond the last unflushed appends) |
| Kill after the last collection page, before the completion response | Journaled pending survives; the next run completes with pending and flushes it (no re-scan duplicates) |
| Kill during queue drain after flush | Un-forwarded messages already in `history.txt`; never retried (**loss**) |
| `seq_tracker.txt` deleted, `history.txt` kept | Every chat backfills again and re-forwards its window (**duplicates**; dedupe bypassed during backfill) |
| `history.txt` deleted, tracker done | Live mode re-forwards the newest page within the window once (**duplicates**) |
| 129th chat | Not tracked; backfill never progresses for it |
| Tracker file with a 3-field legacy line | `scan_from = 0`, `backfill_done = 0`: full re-backfill |
| Duplicate `chat_id` rows in the file | Two rows; getters/setters act on the first; both are saved |

## 6. Edge cases

| Case | Behavior |
|---|---|
| Empty channel | First response `total == 0` → backfill marked done, no forwards |
| One-message channel | Page 1 collects it; page 2 is the echo → completion B → flush |
| Window 0 | Pages to the beginning of the channel; unbounded pending/queue |
| Window larger than channel age | Same as window 0 for that chat |
| Page straddling the cutoff | One extra page request, then completion A |
| Cutoff exactly equal to a message date | `date < cutoff` is false → message kept |
| `scan_from` id no longer exists (deleted message) | `getChatHistory` starts from the nearest older message; equality skip never matches |
| Chat with >10,000 messages in the window | Paged correctly (sequential is the only mode that pages) |
| Flush with 0 pending | Possible when completion A/B and no messages survived; no forward calls |

## 7. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-04-01 | Fresh state, sequential on, `asc`, 24 h window, one active source | `Backfill: …` lines, then forwards appear oldest→newest |
| SC-04-02 | Same with `desc` | Forwards appear newest→oldest, album/reply members still ascending inside a job |
| SC-04-03 | `history_window_hours: 0` | Backfill pages to the channel start; `seq_tracker.txt` row ends with `0 1` after flush |
| SC-04-04 | Kill mid-backfill, restart | Journaled pending survives; resume collects older messages and flushes all (no loss beyond unflushed appends) |
| SC-04-05 | Delete `seq_tracker.txt`, restart with history intact | Window is re-forwarded (duplicates) |
| SC-04-06 | Restart after a completed backfill | No re-backfill; only new posts |
| SC-04-07 | 129 distinct sources | 129th never tracks or forwards |
| SC-04-08 | Empty channel | Tracker row written with `backfill_done = 1`, no forwards |
| SC-04-09 | `enable_ordering: true`, sequential off | Live batches sorted asc/desc; no backfill, no tracker file |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c` citations to the working-tree `Runtime` refactor |
| 2026-11-17 | Step 2: moved tracker/poll into `src/`; refreshed citations and `tracker_store_*` names |
| 2026-11-19 | Refreshed citations after Step 3/4.1 changes in `src/poll.c` and `src/tracker.c`; backfill behavior unchanged |
| 2026-11-19 | Step 4.4b: pending buffer journaled to seq_pending.txt (05 §4.3), replayed on restart, compacted on flush; TD-02 addressed; mid-backfill loss and re-scan duplicate cases closed; SC-04-04 updated |
| 2026-11-19 | Step 4.5: tracker-full warning when an entry cannot be created (TD-09 partial) |
| 2026-11-19 | Step 4.6: enable_ordering splits ordering from backfill (TD-14 addressed); SC-04-09 added |
