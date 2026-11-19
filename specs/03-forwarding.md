# 03 — Forwarding: Grouping, Queue, Delay, Errors

Status: baseline · Owner: `src/forward.c`, `src/main.c` (`process_fwd_queue` caller) · Depends on: 02, 04

## 1. Input contract

`process_msgs(src_chat_id, msgs, count)` (`src/forward.c:115`) receives messages that
already passed id-zero, dedupe, and cutoff filters. It performs no additional
filtering. It is called from two places:

- normal polling: a temporary array in wire order, newest first
  (`src/poll.c:148`);
- sequential backfill flush: the tracker’s pending buffer (04 §3.6).

## 2. Ordering

When sequential mode is **on** and `count > 1`, the array is sorted in place
before grouping (`src/forward.c:47-48`):

| `sequence_direction` | Comparator | Order |
|---|---|---|
| `asc` (0, default) | date ascending, then id ascending | oldest → newest |
| `desc` (1) | date descending, then id descending | newest → oldest |

When sequential mode is **off**, the array is not sorted; wire order
(newest-first) is preserved.

Intra-job ordering is **always ascending by message id**, regardless of
`sequence_direction`:

- reply chain ids are bubble-sorted ascending (`src/forward.c:59-61`);
- album ids are sorted ascending by message id (`src/forward.c:83-87`).

`desc` therefore orders batches, not the messages inside a batch.

## 3. Reply chains (depth 1)

For a non-album message at index `i` (`src/forward.c:49-76`):

1. Build `ids` (max 256):
   - if `reply_to_msg_id != 0`, append it first;
   - append the message’s own id;
   - append the ids of every **ungrouped** message in the same batch whose
     `reply_to_msg_id` equals this message’s id (while under the 256 cap).
2. Sort `ids` ascending.
3. `fwd_queue_push(src_chat_id, ids, id_count)`.
4. `history_add(src_chat_id, own_id)`; in sequential mode
   `seq_tracker_set(src_chat_id, own_id, date)`.
5. Mark the message grouped.
6. For **every** matching direct child (even beyond the 256 cap):
   `history_add`, optional `seq_tracker_set`, mark grouped.

Consequences (all current behavior, not bugs to fix in a structural refactor):

- **The parent is not marked as forwarded.** If the parent is also present in
  the same batch, it will be processed later as a normal message and forwarded
  a second time. The destination can therefore receive the parent twice: once
  as part of the reply chain and once standalone. It also happens if the parent
  was forwarded in an earlier poll.
- A parent outside the current cutoff window is still forwarded, because the
  decision happened before the cutoff.
- Depth is exactly one. A parent that itself replies to something does not pull
  in the grandparent.
- Children beyond the 255th are marked forwarded but omitted from the job, i.e.
  silently dropped (TD-09).
- `reply_to` is read from the message’s `reply_to.message_id` field
  (`src/poll.c:81-82`, `138-139`); absent → 0.

## 4. Albums

For an album message (`media_album_id != 0`, `src/forward.c:77-97`):

1. Collect **all ungrouped** messages in the batch with the same
   `media_album_id` into `album_indices[256]`. There is no bounds check; the
   Telegram album limit (10) is the only guard.
2. Sort those indices ascending by message id.
3. Push one job containing exactly those ids.
4. For each: `history_add`, optional `seq_tracker_set`, mark grouped.

Album messages never carry reply parents, even if `reply_to_msg_id` is set.

Grouping is **batch-scoped**. An album split across two poll responses, or
across two backfill pages, is forwarded as two partial albums (TD-13). Members
already present in the dedupe set are absent from the batch, so a partially
seen album forwards only its unseen members.

## 5. Forward queue

### 5.1 Structure

```c
typedef struct {
    long long src_chat_id;
    long long *ids;   /* heap, count entries, count <= MAX_FWD_IDS */
    int       count;
} ForwardJob;
```

(`src/tgf.h:35-39`). The queue is a ring buffer of jobs with a head index
(`fwd_queue_head`), capacity starting at 1024 and doubling
(`src/forward.c:3-19`). Each job's ids are heap-allocated to `count`
(`src/forward.c:15-17`), so a single-message job costs ~40 bytes, not 2 KiB
(TD-25, addressed). It lives only in memory.

`fwd_queue_push`:

- grows the array on demand; OOM → status `OOM: fwd queue (<cap>)` and the job
  is dropped (messages are already marked forwarded → lost, TD-01/TD-19);
- copies at most `MAX_FWD_IDS` ids; extras are silently discarded.

### 5.2 Dispatch

`process_fwd_queue` (`src/forward.c:130-152`) runs once per main-loop tick and:

1. Returns 0 if the queue is empty.
2. Returns 0 if `now - fwd_last_time < forward_delay_sec`
   (`now = (double)time(NULL)`, 1 s resolution).
3. Serializes the head job's ids into a comma-separated string (8 KiB buffer,
   `APPEND_ID` at `src/tgf.h:146-150`).
4. Builds a `"@extra"` tag:
   - one id → `fwd_<src_chat_id>_<message_id>`;
   - multiple ids → `fwd_<src_chat_id>_batch`.
5. Sends:

```json
{"@type":"forwardMessages",
 "chat_id":<dest_chat_id>,
 "from_chat_id":<src_chat_id>,
 "message_ids":[...],
 "@extra":"fwd_..."}
```

6. Frees the job's heap ids and advances the head index (O(1)).
7. Sets `fwd_last_time = now` and returns 1.

At most **one job per loop tick**. With `forward_delay_sec >= 1`, wall-clock
spacing is that many seconds (1 s clock resolution). With `forward_delay_sec <=
0`, dispatch is limited only by tick frequency. Albums and reply chains count
as one job and therefore one delay unit.

`transport_send` formats the request into an 8 KiB buffer (`src/transport.c:6-25`). If the
formatted request would exceed it, it calls `ui_status("WARNING: request
truncated")` and **still sends the truncated JSON**. Current maximum id string
size (~256 × 20 chars) fits, but there is no rejection path.

## 6. Response handling

All `forwardMessages` errors arrive as `@type == "error"` and are routed to
`on_error`, not `on_response` (`src/main.c:112-113`). `on_error` prints:

```
ERROR [fwd_<...>] code=<n>: <message>
```

via `ui_status`. `forward_handle` has no error branch (the former unreachable
`Forward fail [...]` code was removed, TD-24).

Success (`src/forward.c:154-164`):

- the source chat id arrives in the typed `RequestId` decoded from the extra
  (`src/main.c:28-32`); single-id tags use `fwd_<chat>_<id>`, batch tags
  `fwd_<chat>_batch`;
- set `src_name` to the matching configured `source_channels` entry;
- `ui_forwarded`: in `-d` mode print ` FORWARDED from <src_name> → <dest_channel>`;
- in dashboard mode the change appears on the next refresh.

Not done: checking the returned forwarded-message ids, detecting partial
success, retrying, surfacing the error in the dashboard beyond one status slot,
or un-marking the dedupe entry.

## 7. Failure and loss policy

- A failed forward is **never retried**; its messages are already in
  `history.txt` and are treated as seen (TD-01).
- A process restart discards the whole queue; those messages are also already
  marked (TD-03).
- TDLib flood-wait (`retry_after`) is not recognized; the next jobs keep
  coming at the configured rate (TD-06).
- A single invalid/deleted message id in a batch fails the whole request; the
  other ids in that batch are not retried.

## 8. Caps and buffers

| Limit | Value | Effect when exceeded |
|---|---|---|
| Ids per job | 256 (`MAX_FWD_IDS`) | extra reply children marked but not forwarded; album array may overflow |
| Reply chain depth | 1 | grandparents omitted |
| Request buffer | 8 KiB (`src/transport.c:10`) | truncated JSON still sent |
| Id string buffer | 8 KiB (`src/forward.c:136`) | truncated id list |
| Payload buffer | 16 KiB (`src/forward.c:141`) | truncated JSON |
| Queue capacity | unbounded growth | ring buffer doubles from 1024; ~40 B + 8 B/id per job |

## 9. Edge cases

| Case | Behavior |
|---|---|
| Single post, no reply | Job with one id |
| Reply whose parent is in the same batch | Parent sent twice (chain + standalone) |
| Reply whose parent is outside the batch | Parent included once, even if old/outside window |
| Message with many replies | Up to 255 children included; all marked |
| Album of 1 | One-id job |
| Album split across polls | Two forward jobs, two partial albums in destination |
| `forward_delay_sec = 0` | Dispatch rate limited by tick frequency |
| `forward_delay_sec` negative | Same as 0 (`now - last < negative` is false) |
| Queue OOM | Job dropped, messages marked seen |
| Destination lacks permission | `ERROR`, job dropped, permanent loss |

## 10. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-03-01 | Source posts text, wait one poll | One message in destination |
| SC-03-02 | Source posts a reply to an older post | One forward block containing parent + reply; parent is duplicated if it is also in the same poll batch |
| SC-03-03 | Source posts a 3-photo album | One album in destination with 3 photos, ascending id order |
| SC-03-04 | Two single posts, `forward_delay_sec: 10`, `-d` | `FORWARDED` lines at least 10 s apart |
| SC-03-05 | `sequence_direction: desc` with several posts | Batches newest-first; reply/album members still ascending |
| SC-03-06 | Revoke bot/user permission to post in destination | `ERROR [fwd_...]`; post is not retried after restart |
| SC-03-07 | Kill the process with several jobs queued | Queue is empty on restart; those posts are not forwarded |
| SC-03-08 | Album arrives across two polls (slow source) | Two partial albums in destination |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c`/`tgf.h` citations to the working-tree `Runtime` refactor |
| 2026-11-17 | Step 2: moved forwarding into `src/`; refreshed citations |
| 2026-11-19 | As-built refresh for Step 3: ring-buffer queue with heap id payloads (TD-25), head-index pop, RequestId-based response handling, dead error branch removed (TD-24); observable pacing/grouping unchanged; refreshed citations |
