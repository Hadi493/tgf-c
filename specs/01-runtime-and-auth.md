# 01 — Runtime, Auth, Resolution, Shutdown

Status: baseline · Owner: `src/main.c`, `src/auth.c`, `src/resolve.c` · Depends on: 00, 05

## 1. Startup sequence

Exact order in `main` (`src/main.c:80-104`):

1. `setvbuf(stdout/stderr, _IONBF)` — unbuffered output (`src/main.c:82-83`).
2. `ui_init` selects the stderr or ncurses sink from `argv[1]`
   (`src/main.c:84`, 06 §1).
3. `load_config("config.json")`; any failure prints to stderr and exits **1**
   (`src/main.c:86`). The path is hardcoded and CWD-relative.
4. `num_sources == 0` → `ERROR: No source channels configured` on stderr,
   exit **1** (`src/main.c:87`). No TDLib client is created.
5. `td_json_client_create()`; NULL → stderr + exit **1** (`src/main.c:89-90`).
6. `setLogVerbosityLevel 0` — TDLib logging silenced (`src/main.c:91`). TDLib's
   own creation-time log lines may reach stderr before this takes effect.
7. `calloc(num_sources, sizeof(long long))` for `source_chat_ids`; failure
   destroys the client and exits **1** (`src/main.c:93-94`).
8. `history_store_load()`; `tracker_store_load()` only if sequential mode is
   enabled (`src/main.c:96-97`).
9. `signal(SIGINT)`, `signal(SIGTERM)` → set `keep_running = false`
   (`src/main.c:98-99`, `src/main.c:15-18`).
10. `getAuthorizationState` (`src/main.c:100`).
11. Enter the loop; `last_poll = 0`, `last_prune = 0` (`src/main.c:102-104`,
    `src/main.c:146-147`).

No CLI flags affect startup beyond `-d` (06). No config path override exists.

## 2. TDLib authorization

### 2.1 Trigger and dispatch

`updateAuthorizationState` messages are handled by `on_auth_state`
(`src/auth.c:18-72`). The state is detected with substring matching on the raw
JSON (`strstr`), not by reading the state object’s `@type` (`src/auth.c:19-71`). An
`authorizationState` update with no recognized state causes no action.

The main loop routes `updateAuthorizationState` directly to `auth_handle`
(`src/main.c:112-113`); there is no separate update path.

### 2.2 States handled

| TDLib state | Request sent | `@extra` | Prompt |
|---|---|---|---|
| `authorizationStateWaitTdlibParameters` | `setTdlibParameters` | `auth_params` | — |
| `authorizationStateWaitPhoneNumber` | `setAuthenticationPhoneNumber` | `auth_phone` | clear screen, `Phone (+xxx):` |
| `authorizationStateWaitCode` | `checkAuthenticationCode` | `auth_code` | `Code:` |
| `authorizationStateWaitPassword` | `checkAuthenticationPassword` | `auth_pw` | `2FA password:` |
| `authorizationStateReady` | see §3 | `getme` / `resolve_dest` | prints `✓ Authorized!` |

`setTdlibParameters` payload is fixed (`src/auth.c:21-34`):

```json
{"database_directory":"tdlib_db","files_directory":"tdlib_files",
 "use_message_database":true,"use_secret_chats":false,
 "api_id":<config>,"api_hash":"<config>","system_language_code":"en",
 "device_model":"Desktop","application_version":"1.0"}
```

### 2.3 States not handled

`authorizationStateWaitOtherDeviceConfirmation`, `WaitRegistration`,
`WaitQrCode`, `LoggingOut`, `Closing`, `Closed` have no branch. The process
remains running and inert; no message explains why (TD-08, TD-15).

### 2.4 Prompt behavior

- Prompts are blocking `fgets` calls on stdin (`src/auth.c:35-58`) inside the receive
  handler; the loop does not tick until input arrives.
- Input is truncated to 31 chars (phone/code) or 127 chars (password) and
  trailing `\r`/`\n` stripped.
- Phone/code/password values are JSON-escaped by a local helper for `"` and
  `\` only (`src/auth.c:6-16`). Empty input is still sent.
- Phone prompt clears the screen with ANSI `ESC[2J ESC[H` before printing.
- Auth request errors are reported through `ui_status`; because the dashboard
  starts only after `authorized`, they go to **stderr** as `ERROR [extra]
  code=N: message` (`src/main.c:58-74`, `src/ui.c:104-108`).
- There is **no retry or re-prompt** after an auth error. A wrong code leaves
  TDLib waiting and the process idle (TD-08).
- A SIGINT during a prompt sets `keep_running` but `fgets` usually restarts;
  the prompt must be answered before shutdown proceeds.

### 2.5 Session reuse

The TDLib session lives in `tdlib_db/` and `tdlib_files/`, both CWD-relative.
With a valid session, TDLib goes straight to `authorizationStateReady` on the
next run; no prompts appear.

## 3. Destination resolution

On `authorizationStateReady` (`src/auth.c:59-71`):

1. `authorized = 1`; prints `\n✓ Authorized!\n` to stdout.
2. Strip one leading `@` from `dest_channel`.
3. If the remainder is exactly `me` or `Me` (case-sensitive pair):
   `getMe` with extra `getme`.
   Otherwise: `searchPublicChat{"username":<remainder>}` with extra
   `resolve_dest`.
4. `getme` response: parse numeric `id`, send
   `createPrivateChat{"user_id":<id>,"force":true}` with extra `resolve_dest`
   (`src/resolve.c:32-38`).
5. `resolve_dest` success: set `dest_chat_id`, `dest_resolved = 1`, reset
   `resolve_index = 0`, call `resolve_next` (`src/resolve.c:40-47`).

Destination forms supported: `@username`, bare `username`, `@me`, `me`, `Me`.
Anything else (`t.me/...` links, numeric ids, invite links) is sent as a
username and fails. `@ME` fails (TD-14).

A destination resolution failure is **fatal**: the process prints
`ERROR: Failed to resolve destination '<dest_channel>'` to stderr and
exits with code 1 (`src/main.c:65-69`). `dest_resolved` is never set, polling
never starts, and no dashboard is initialized. A `getMe` error (the `me`/`Me`
destination form) is treated as a destination failure. The fatal path calls
`transport_destroy(client)` before `exit(1)` so the TDLib session database is
closed cleanly.

## 4. Source resolution

`resolve_next` (`src/resolve.c:6-30`) resolves sources strictly one at a time:

- Take `source_channels[resolve_index]`, strip one leading `@`.
- Send `searchPublicChat{"username":...}` with extra `resolve_<index>`.
- Increment `resolve_index`, set `resolve_active = 1`, return.
- If `resolve_index >= num_sources`, clear `resolve_active`.

Success (`src/resolve.c:49-59`): if the response is an object with a numeric `id`,
store `source_chat_ids[index]`, increment `source_count`, optionally print
`Resolving channels... n/total` in debug mode, then resolve the next source.

Failure: `on_error` sees an extra beginning with `resolve_` and not equal to
`resolve_dest`, records the failure, and calls `resolve_next` (`src/main.c:70`).
The failed source is left at `chat_id = 0` and is skipped by polling forever.
After all sources are attempted, if `source_count == 0` (all failed), the
process prints `ERROR: No source channels resolved` to stderr, destroys the
TDLib client, and exits with code 1 (`src/resolve.c:18-22`). An empty
`source_channels` array is rejected earlier, at startup (§1). If at least one
source resolved, a summary of failed sources is printed to stderr and the
process continues with the resolved subset. The completion check runs in
`resolve_next` when the cursor passes the last source (`src/resolve.c:17-29`);
the summary format is
`WARNING: <k> of <n> source channels failed to resolve: <name>…` (raw
configured names, space-separated, failed entries only).

## 5. Main loop

Pseudo-code (`src/main.c:104-148`):

```
while (keep_running):
    result = td_json_client_receive(client, 1.0)
    if result:
        root = parse(result)
        type  = root["@type"]  (default "")
        extra = root["@extra"]
        if type == updateAuthorizationState: auth_handle(raw)
        else if type == error:               on_error(root)
        else if extra:                       on_response(root, extra, argv)
                                               # RequestId -> handler_for
        free root

    if authorized && dest_resolved && source_count >= 1 && !resolve_active:
        if now - last_poll >= 5: poll_channels(); last_poll = now

    if argv[1] == NULL && authorized && dest_resolved
       && source_count >= 1 && !resolve_active:
        init ncurses once; refresh dashboard at most 1 Hz

    process_fwd_queue(client)          # at most one job

    if now - last_prune >= 1200: history_store_prune(); last_prune = now
```

Consequences:

- The receive timeout sets the floor for every task: ~1 s.
- Errors are routed by `@type == "error"` **before** `@extra`; all request
  errors are handled by `on_error` (`src/main.c:58-74`). The response handlers
  contain no error branches (the unreachable ones were removed, TD-24).
- `argv` is passed into the response handlers solely to decide whether to print debug
  lines (`src/resolve.c:54-56`); `forward_handle` ignores it (the UI sink owns
  debug printing).
- Dashboard initialization is gated on the same full condition as polling
  (`authorized && dest_resolved && source_count >= 1 && !resolve_active`), so
  no dashboard starts on any fatal resolution path (Step 4.1).

## 6. Shutdown

SIGINT/SIGTERM only clear `keep_running` (`src/main.c:15-18`). The loop exits after the
current `receive` returns (≤1 s) and cleanup runs (`src/main.c:150-166`):

- free every non-NULL `history_set` entry, the table, and `history_time`;
- free each queued job's heap `ids`, then the `fwd_queue` ring array;
- free the in-flight job's heap `ids` if present;
- free `source_chat_ids`, each `source_channels[i]`, `source_channels`,
  `api_hash`, `dest_channel`, `history_file`;
- `td_json_client_destroy`.

It does **not** flush the forward queue, flush backfill pending buffers, free
`seq_tracker[i].pending`, save the tracker, or close/checkpoint anything else.
Queued work silently disappears (TD-02, TD-03). The ncurses terminal is
restored by `atexit(ncurses_cleanup)`.

Exit codes: `0` on normal loop exit; `1` for config load failure, empty
`source_channels`, client creation failure, `source_chat_ids` allocation
failure, destination resolution failure, or all-sources-failed (Step 4.1).
The fatal resolution paths call `transport_destroy(client)` before `exit(1)`;
other process state is left to the OS. No other exit path.

## 7. Edge cases

| Case | Behavior |
|---|---|
| Missing `config.json` | `Cannot open config.json`, exit 1 |
| Invalid JSON | `Invalid JSON in config.json`, exit 1 |
| `dest_channel` unresolvable | `ERROR: Failed to resolve destination '<dest>'`, exit 1 |
| Some sources unresolvable | `ERROR` lines for failed; summary printed; resolved subset polled |
| All sources unresolvable | `ERROR: No source channels resolved`, exit 1 |
| Empty source array | `ERROR: No source channels configured`, exit 1 |
| Wrong code / password | `ERROR` line; no re-prompt |
| Cached session | Straight to resolution |
| SIGINT while idle | Exit within ~1 s |
| SIGINT while prompting | Waits for the prompt to be answered |
| Run from another CWD | New profile + new files; old state ignored |

## 8. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-01-01 | Delete `tdlib_db/`, run `./tgf -d` | Phone prompt, then code, then optional 2FA; `✓ Authorized!`; resolution lines |
| SC-01-02 | Run again without deleting the session | No auth prompts |
| SC-01-03 | Set `dest_channel` to a nonexistent public name | `ERROR: Failed to resolve destination ...`, exit 1 |
| SC-01-04 | Configure one valid and one nonexistent source | Valid source polled; error line + summary for failed; exit 0 |
| SC-01-05 | Configure `"source_channels": []` | `ERROR: No source channels configured`, exit 1 |
| SC-01-06 | Press Ctrl+C during steady state | Prompt returns within ~1 s, exit code 0 |
| SC-01-07 | Press Ctrl+C at the phone prompt | Prompt remains until answered; then shutdown proceeds |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c` citations to the working-tree `Runtime` refactor |
| 2026-11-17 | Step 2: moved auth/resolve/dispatch into `src/`; refreshed citations |
| 2026-11-17 | Step 4.1: Destination resolution failure now fatal (exit 1); all-sources-failed and empty-sources now fatal (exit 1); failed sources summary printed |
| 2026-11-19 | Step 4.1 restored after loss in the RequestId seam rewrite: fatal destination/source resolution re-implemented with clean client destroy, empty-sources startup check, failed-source WARNING summary format pinned, dashboard gated on full resolution; fixed resolve_dest misrouting in request_id_from_extra; verified SC-01-03/04/05/06 and SC-06-05 against a live cached session; refreshed citations |
| 2026-11-19 | Step 4.3: shutdown frees the in-flight job; queued and in-flight work is still dropped on exit (TD-03, Step 4.4) |
