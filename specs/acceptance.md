# Manual Acceptance Matrix (Step 0)

Status: active · Owner: `tests/test_main.c` + this checklist · Derived from:
00–07 acceptance scenarios

This is the human verification pass for changes the automated suite cannot
cover (TDLib, UI, files, multi-process restart). The automated suite
(`./nob test`) remains the first gate; this matrix is the second.

## Rules

- Run against a **disposable profile and a test destination channel**. The
  scenarios forward real messages and delete state files.
- Each scenario names the spec that defines the expected behavior. If the
  result differs, the spec and code are out of sync: stop and report it before
  "fixing" either.
- Record `pass`, `fail`, or `n/a` with a one-line note. A failed row blocks the
  change unless the owning spec says the behavior is a known limitation and the
  change does not worsen it.
- Scenarios marked **destructive** delete `history.txt`, `seq_tracker.txt`, or
  `tdlib_db/`; run them in a copy of the working tree.

## Preconditions

1. Build: `./nob && ./nob test` passes.
2. A `config.json` with a test destination and at least one low-traffic source.
3. A cached TDLib session (run once interactively) unless the scenario tests
   login.
4. `./tgf -d` runs from the repository root unless stated otherwise.

## Checklist

### Architecture (spec 00)

| ID | Scenario | Result |
|---|---|---|
| SC-00-01 | Start with valid config: prompts or immediate resolution, then dashboard | |
| SC-00-02 | Start from another directory: fresh state is created there | |
| SC-00-03 | SIGINT while running: exit within ~1 s, code 0, terminal restored | |
| SC-00-04 | Process has one thread and no extra listeners | |

### Runtime and auth (spec 01)

| ID | Scenario | Result |
|---|---|---|
| SC-01-01 | Delete `tdlib_db/` (destructive), `./tgf -d`: phone → code → 2FA → authorized | |
| SC-01-02 | Run again without deleting the session: no prompts | |
| SC-01-03 | Nonexistent `dest_channel`: `ERROR: Failed to resolve destination ...`, exit 1 | |
| SC-01-04 | One valid + one nonexistent source: valid polled; error line + summary for failed; exit 0 | |
| SC-01-05 | `"source_channels": []`: `ERROR: No source channels configured`, exit 1 | |
| SC-01-06 | Ctrl+C in steady state: exits promptly | |
| SC-01-07 | Ctrl+C at the phone prompt: prompt remains until answered | |

### Polling and dedupe (spec 02)

| ID | Scenario | Result |
|---|---|---|
| SC-02-01 | Fresh state, recent posts: newest-first forwards, `New: N` line | |
| SC-02-02 | Immediate restart: no duplicates | |
| SC-02-03 | Window smaller than a post's age: post skipped | |
| SC-02-04 | `history_window_hours: 0`: old posts forwarded, no prune lines | |
| SC-02-05 | Run 20+ min: prune line appears; live messages not re-forwarded afterwards | |
| SC-02-06 | Corrupt a timestamp in `history.txt`: entry becomes permanent | |
| SC-02-07 | Delete `history.txt` (destructive): bounded duplicate risk | |

### Forwarding (spec 03)

| ID | Scenario | Result |
|---|---|---|
| SC-03-01 | One text post: one forward | |
| SC-03-02 | Reply to an older post: parent + reply block; parent may duplicate | |
| SC-03-03 | 3-photo album: one album, ascending ids | |
| SC-03-04 | Two posts, delay 10: `FORWARDED` lines ≥10 s apart | |
| SC-03-05 | `desc`: batches newest-first; reply/album members ascending | |
| SC-03-06 | Revoke destination permission: `ERROR [fwd_…] code=403` + permanent-drop status, no retry | |
| SC-03-07 | Kill with jobs queued: queue resumes after restart; in-flight may duplicate | |
| SC-03-08 | Album split across polls: two partial albums | |
| SC-03-09 | Flood wait: `Flood wait: pausing Ns`, dispatch resumes, no loss | |
| SC-03-10 | Transient error then success: retried, delivered | |

### Sequential backfill (spec 04)

| ID | Scenario | Result |
|---|---|---|
| SC-04-01 | Fresh state, sequential asc, 24 h: oldest→newest after backfill | |
| SC-04-02 | Same with desc: newest→oldest batches | |
| SC-04-03 | Window 0: pages to channel start, tracker ends `0 1` | |
| SC-04-04 | Kill mid-backfill, restart (destructive): journaled pending survives, resume flushes all | |
| SC-04-05 | Delete tracker, keep history (destructive): window re-forwarded | |
| SC-04-06 | Restart after completed backfill: only new posts | |
| SC-04-07 | 129 sources: 129th never tracks or forwards | |
| SC-04-08 | Empty channel: row written with `backfill_done = 1` | |

### Config and storage (spec 05)

| ID | Scenario | Result |
|---|---|---|
| SC-05-01 | `cp config.json.example config.json`: config accepted (valid JSON); auth proceeds to the phone prompt | |
| SC-05-02 | Fill real credentials: starts | |
| SC-05-03 | `"API_ID"` instead of `api_id`: accepted | |
| SC-05-04 | String `"true"` for sequential: backfill does not start | |
| SC-05-05 | Delete `history.txt` between runs: bounded duplicates | |
| SC-05-06 | Kill during prune rewrite: previous history file intact | |
| SC-05-07 | Window 0: `history.txt` grows, no prune | |
| SC-05-08 | Kill with jobs queued, restart: queue resumes from `fwd_queue.txt` | |
| SC-05-09 | Kill mid-backfill, restart: pending resumes from `seq_pending.txt` | |

### UI and CLI (spec 06)

| ID | Scenario | Result |
|---|---|---|
| SC-06-01 | `./tgf` with cached session: dashboard fields match config | |
| SC-06-02 | `./tgf -d`: no dashboard, debug stdout + status stderr | |
| SC-06-03 | `./tgf foo`: no dashboard, no debug | |
| SC-06-04 | Forward in dashboard: `FORWARDED` source updates within ~1 s | |
| SC-06-05 | Misconfigured destination: `ERROR: Failed to resolve destination ...`, exit 1 | |
| SC-06-06 | SIGINT: terminal restored, exit 0 | |
| SC-06-07 | `env -u TERM ./tgf`: ncurses startup failure | |

### Build and platform (spec 07)

| ID | Scenario | Result |
|---|---|---|
| SC-07-01 | Fresh clone, `gcc -o nob nob.c && ./nob`: `tgf` runs | |
| SC-07-02 | `./nob` twice: second run is a no-op | |
| SC-07-03 | Remove `lib/td/build` (destructive): TDLib rebuilds or system lib used | |
| SC-07-04 | `./nob clean`: `tgf` removed; user data preserved | |
| SC-07-05 | Move `tgf` elsewhere: works only with system `libtdjson` | |
| SC-07-06 | Move the repo: rpath still resolves | |

## Recording results

Copy this file's checklist into the change description or paste a table:

```
SC-02-01 pass — 3 posts, newest-first
SC-02-02 pass — restart, no duplicates
SC-04-04 n/a  — destructive, covered by unit test instead
```
