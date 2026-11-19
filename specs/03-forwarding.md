# 03 — Forwarding: Grouping, Queue, Delay, Errors

Status: baseline · Owner: `src/forward.c`, `src/main.c` (`process_fwd_queue` caller) · Depends on: 02, 04

## 1. Input contract

`process_msgs(src_chat_id, msgs, count)` (`src/forward.c:162`) receives messages that
already passed id-zero, dedupe, and cutoff filters. It performs no additional
filtering. It is called from two places:

- normal polling: a temporary array in wire order, newest first
  (`src/poll.c:148`);
- sequential backfill flush: the tracker’s pending buffer (04 §3.6).

## 2. Ordering

When sequential mode is **on** and `count > 1`, the array is sorted in place
before grouping (`src/forward.c:94-95`):

| `sequence_direction` | Comparator | Order |
|---|---|---|
| `asc` (0, default) | date ascending, then id ascending | oldest → newest |
| `desc` (1) | date descending, then id descending | newest → oldest |

When sequential mode is **off**, the array is not sorted; wire order
(newest-first) is preserved.

Intra-job ordering is **always ascending by message id**, regardless of
`sequence_direction`:

- reply chain ids are bubble-sorted ascending (`src/forward.c:106-108`);
- album ids are sorted ascending by message id (`src/forward.c:130-134`).

`desc` therefore orders batches, not the messages inside a batch.

## 3. Reply chains (depth 1)

For a non-album message at index `i` (`src/forward.c:96-123`):

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

For an album message (`media_album_id != 0`, `src/forward.c:124-144`):

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
    int       retries;   /* failed attempts so far */
} ForwardJob;
```

(`src/tgf.h:37-42`). The queue is a ring buffer of jobs with a head index
(`fwd_queue_head`), capacity starting at 1024 and doubling
(`src/forward.c:3-20`). Each job's ids are heap-allocated to `count`, so a
single-message job costs ~48 bytes (TD-25, addressed). It is persisted to
`fwd_queue.txt` after every mutation (05 §5); a restart resumes the queue
(TD-03, addressed).

At most one job is **in flight**: dispatch moves the head job into
`rt.fwd_inflight` (`fwd_inflight_active`, `fwd_inflight_time`); the slot is
resolved only by the response or the 60 s timeout (§6, §7).

`fwd_queue_push`:

- grows the array on demand; OOM → status `OOM: fwd queue (<cap>)` and the job
  is dropped (messages are already marked forwarded → lost, TD-01/TD-19);
- copies at most `MAX_FWD_IDS` ids; extras are silently discarded.

### 5.2 Dispatch

`process_fwd_queue` (`src/forward.c:200-231`) runs once per main-loop tick and:

1. If a job is in flight: when `now - fwd_inflight_time > FWD_TIMEOUT_SEC`
   (60 s), publish `Forward timed out` and take the retryable-failure path
   (§7); otherwise return 0.
2. Returns 0 if the queue is empty.
3. Returns 0 if `now - fwd_last_time < forward_delay_sec`
   (`now = (double)time(NULL)`, 1 s resolution).
4. Returns 0 if `now < fwd_flood_until` (flood-wait pause, §7).
5. Serializes the head job's ids into a comma-separated string (8 KiB buffer,
   `APPEND_ID` at `src/tgf.h:153-157`).
6. Builds a `"@extra"` tag:
   - one id → `fwd_<src_chat_id>_<message_id>`;
   - multiple ids → `fwd_<src_chat_id>_batch`.
7. Sends:

```json
{"@type":"forwardMessages",
 "chat_id":<dest_chat_id>,
 "from_chat_id":<src_chat_id>,
 "message_ids":[...],
 "@extra":"fwd_..."}
```

8. Moves the head job into the in-flight slot (ids ownership and `retries`
   transfer; the queue slot's ids pointer is nulled), advances the head
   index (O(1)).
9. Sets `fwd_last_time = now`, `fwd_inflight_time = now`, and returns 1.

Every mutation (enqueue batch, dispatch, confirm, requeue, drop) rewrites
`fwd_queue.txt` atomically, including the in-flight job until confirmed
(05 §5).

At most **one job per loop tick** and at most **one in-flight request**; the
next dispatch waits for the response or the timeout. With
`forward_delay_sec >= 1`, wall-clock spacing is at least that many seconds
(1 s clock resolution). With `forward_delay_sec <= 0`, dispatch is limited by
tick frequency and response latency. Albums and reply chains count as one job
and therefore one delay unit.

`transport_send` formats the request into an 8 KiB buffer (`src/transport.c:6-25`). If the
formatted request would exceed it, it calls `ui_status("WARNING: request
truncated")` and **still sends the truncated JSON**. Current maximum id string
size (~256 × 20 chars) fits, but there is no rejection path.

## 6. Response handling

All `forwardMessages` errors arrive as `@type == "error"` and are routed to
`on_error`, not `on_response` (`src/main.c:115-116`). `on_error` prints:

```
ERROR [fwd_<...>] code=<n>: <message>
```

via `ui_status`, then calls `forward_error_handle(rt, id, code, message)`
(`src/forward.c:247-265`) for `REQ_FORWARD_ONE`/`REQ_FORWARD_BATCH`.

Success (`forward_handle`, `src/forward.c:233-245`):

- the source chat id arrives in the typed `RequestId` decoded from the extra
  (`src/main.c:28-32`); single-id tags use `fwd_<chat>_<id>`, batch tags
  `fwd_<chat>_batch`;
- set `src_name` to the matching configured `source_channels` entry;
- `ui_forwarded`: in `-d` mode print ` FORWARDED from <src_name> → <dest_channel>`;
- in dashboard mode the change appears on the next refresh;
- if the response's chat id matches the in-flight job, the slot is freed and
  cleared (delivery confirmed). Mismatched or unsolicited responses are
  ignored (§7).

Error policy (`forward_error_handle`, guarded by in-flight active and chat-id
match):

| Error | Action | Status line |
|---|---|---|
| code 429 | parse `retry after N` from the message (default 5 s, min 1 s); set `fwd_flood_until = now + N`; re-queue at tail **without** consuming retry budget | `Flood wait: pausing <N>s` |
| code 400 or 403 | permanent: drop the job | `Forward dropped (permanent error <code>)` |
| any other code | retryable: `retries++`; if `retries < MAX_FWD_ATTEMPTS` (3) re-queue at tail, else drop | `Forward dropped after <retries> attempts` on exhaustion |
| no response within 60 s | retryable path (same budget) | `Forward timed out` |

Not done: checking the returned forwarded-message ids or detecting partial
success. Un-marking the dedupe entry on failure is deliberately **not** done
(§7).

## 7. Delivery semantics (at-least-once, Step 4.3)

- Dedupe marking stays at **enqueue** (02 §3.4). Delivery is pursued by
  retrying the job, not by un-marking: un-marking would re-expose the message
  to the next poll while a retry is pending and would require a probe-safe
  remove API. The chosen model keeps the dedupe invariant and retries the job
  itself (TD-01).
- **Retryable failure** (any error except 400/403/429, or the 60 s in-flight
  timeout): `retries++`, re-queue at the **tail** (FIFO among other jobs is
  preserved; retried jobs go last), paced by the delay gate. Budget
  `MAX_FWD_ATTEMPTS = 3` total attempts; on the third failure the job is
  dropped with `Forward dropped after 3 attempts`.
- **Flood wait (429)**: `retry after N` is parsed from the error message
  (default 5 s if absent, minimum 1 s). `fwd_flood_until = now + N` pauses
  **all** dispatch; the job is re-queued without consuming budget. N is
  honored without a cap (Telegram's instruction). Status:
  `Flood wait: pausing <N>s` (TD-06).
- **Permanent failure (400/403)**: dropped immediately with
  `Forward dropped (permanent error <code>)`. A single invalid/deleted id in
  a batch fails the whole batch; the batch is not split (TD-13 residual).
- **Stale responses** (no in-flight job, or chat-id mismatch) are ignored.
  Known edge: a late response for the same chat after a timeout-retry can
  resolve the new in-flight job early; at-least-once semantics tolerate the
  resulting extra retry or drop.
- **Loss residuals**: a restart resumes the queue and re-queues the in-flight
  job (05 §5, TD-03 addressed); a crash between send and response can
  re-forward that job (at-least-once duplicate). A re-queue OOM drops the job
  (TD-19, reported via `ui_oom`); a queue-save failure leaves the queue in
  RAM only (05 §5.2).
- There is no dedupe un-marking anywhere.

## 8. Caps and buffers

| Limit | Value | Effect when exceeded |
|---|---|---|
| Ids per job | 256 (`MAX_FWD_IDS`) | extra reply children marked but not forwarded; album array may overflow |
| Reply chain depth | 1 | grandparents omitted |
| Request buffer | 8 KiB (`src/transport.c:10`) | truncated JSON still sent |
| Id string buffer | 8 KiB (`src/forward.c:212`) | truncated id list |
| Payload buffer | 16 KiB (`src/forward.c:217`) | truncated JSON |
| Queue capacity | unbounded growth | ring buffer doubles from 1024; ~48 B + 8 B/id per job |
| Retry budget | 3 attempts (`MAX_FWD_ATTEMPTS`) | job dropped with a status line |
| In-flight timeout | 60 s (`FWD_TIMEOUT_SEC`) | retryable-failure path |
| Flood pause | `retry after N`, uncapped | all dispatch paused until `now + N` |

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
| Destination lacks permission | `ERROR` + `Forward dropped (permanent error 403)`; no retry |
| Transient error (e.g. 500) | retried up to 3 attempts, then dropped with status |
| Flood wait (429) | dispatch paused `N` s; job re-queued, budget untouched |
| Response never arrives | 60 s timeout → retryable path |
| Stale forward response | ignored (chat-id guard) |

## 10. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-03-01 | Source posts text, wait one poll | One message in destination |
| SC-03-02 | Source posts a reply to an older post | One forward block containing parent + reply; parent is duplicated if it is also in the same poll batch |
| SC-03-03 | Source posts a 3-photo album | One album in destination with 3 photos, ascending id order |
| SC-03-04 | Two single posts, `forward_delay_sec: 10`, `-d` | `FORWARDED` lines at least 10 s apart |
| SC-03-05 | `sequence_direction: desc` with several posts | Batches newest-first; reply/album members still ascending |
| SC-03-06 | Revoke bot/user permission to post in destination | `ERROR [fwd_...] code=403` + `Forward dropped (permanent error 403)`; no retry |
| SC-03-07 | Kill the process with several jobs queued | Queue resumes on restart; queued posts are forwarded (in-flight job may duplicate) |
| SC-03-08 | Album arrives across two polls (slow source) | Two partial albums in destination |
| SC-03-09 | Trigger a flood wait (rapid forwards) | `Flood wait: pausing Ns`; dispatch resumes after the pause; no messages lost |
| SC-03-10 | Transient forward error that later succeeds | job retried; message delivered; `FORWARDED` line on success |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c`/`tgf.h` citations to the working-tree `Runtime` refactor |
| 2026-11-17 | Step 2: moved forwarding into `src/`; refreshed citations |
| 2026-11-19 | As-built refresh for Step 3: ring-buffer queue with heap id payloads (TD-25), head-index pop, RequestId-based response handling, dead error branch removed (TD-24); observable pacing/grouping unchanged; refreshed citations |
| 2026-11-19 | Step 4.3: at-least-once delivery — in-flight slot, bounded retry (3 attempts), flood-wait pause, permanent-drop and timeout statuses, stale-response guard; marking stays at enqueue (un-mark option rejected); TD-01 partially addressed, TD-06 addressed; restart loss remains TD-03 (Step 4.4) |
| 2026-11-19 | Step 4.4a: queue + in-flight persisted to fwd_queue.txt (05 §5), resume on restart; TD-03 addressed; SC-03-07 updated |
