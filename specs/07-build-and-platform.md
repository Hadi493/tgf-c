# 07 — Build and Platform

Status: baseline · Owner: `nob.c`, `nob.h` · Depends on: none

## 1. Prerequisites

| Requirement | Needed when |
|---|---|
| C compiler (`cc`; `clang`, `cl`, `tcc` selected by compiler used to build `nob`) | always |
| TDLib JSON library | always |
| CMake + make + C++ toolchain | only when TDLib is not already present |
| ncurses development files | non-Windows |
| git | only to fetch the TDLib submodule |
| A valid Telegram API id/hash | runtime |

## 2. Bootstrap

`nob` is a self-rebuilding build program (`GO_REBUILD_URSELF`,
`nob.c:159`). The first build is manual:

```sh
gcc -o nob nob.c
./nob
```

Subsequent `./nob` runs recompile `nob` if `nob.c`/`nob.h` changed, keeping the
previous binary as `nob.old`.

## 3. What `./nob` does

1. `GO_REBUILD_URSELF`.
2. If `argv[1] == "clean"`: remove `lib/td/build`, `tgf`, `nob.old`, and
   `tests/tgf_tests` (`rmdir /s /q` + `del` on Windows), exit 0
   (`nob.c:161-164`, `8-23`).
   It does **not** remove `tdlib_db/`, `tdlib_files/`, `history.txt`,
   `seq_tracker.txt`, or `config.json`.
3. Select compiler (`cc` on Unix) and flags (`-Wall -Wextra` for GCC/Clang;
   `/W4 /nologo /D_CRT_SECURE_NO_WARNINGS` for MSVC) (`nob.h:790-814`).
4. Add platform-specific TDLib/ncurses options (§4).
5. Compile everything into `./tgf` in one `cc` invocation:

```
cJSON.c -O3 -o tgf src/tracker.c src/history.c src/forward.c src/config.c src/main.c src/loop.c src/auth.c src/resolve.c src/poll.c src/transport.c src/transport_tdlib.c src/ui.c
```

   (`nob.c:285-307`). `-O3` is unconditional; there is no debug/profile target.
6. Targets: `test` builds and runs `tests/tgf_tests` then `tests/tgf_e2e`
   (`nob.c:80-118`, `210-217`); `e2e` runs just the fake-transport end-to-end
   binary (`nob.c:99-118`, `220-226`); `sanitize`/`asan` build the full `tgf`
   with `-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined`
   (`nob.c:141-197`, `229-244`); `tsan` builds and runs the e2e under
   `-fsanitize=thread` (`nob.c:120-139`, `247-253`); `gui` builds `tgf-gui`
   using `pkg-config --cflags --libs gtk4 libadwaita-1`; `run-gui` builds and
   executes `tgf-gui`; `clean` removes build artifacts including `tgf-gui`.
   `test`/`e2e`/`sanitize`/`asan`/`tsan`/`gui`/`run-gui` are POSIX-only.

## 4. TDLib discovery and build (Unix)

`check_system_td` looks for exactly (`nob.c:25-30`):

```
/usr/lib/libtdjson.so
/usr/local/lib/libtdjson.so
/usr/lib/x86_64-linux-gnu/libtdjson.so
```

If one exists → link `-ltdjson` only.

Otherwise:

1. `td_already_built` checks `lib/td/build/libtdjson.so|.a`
   (`nob.c:73-78`).
2. If absent, ensure sources (`nob.c:32-48`):
   - `app/lib/td/CMakeLists.txt` exists → use it;
   - `lib/td/` exists but empty → `git submodule update --init --depth 1 lib/td`;
   - otherwise → `git clone --depth 1 https://github.com/tdlib/td.git lib/td`.
3. Build (`nob.c:57-71`):
   - require `cmake --version` to succeed;
   - `cmake -B lib/td/build -S lib/td -DCMAKE_BUILD_TYPE=Release`;
   - `cmake --build lib/td/build --target tdjson -- -j1` (single job,
     deliberately low parallelism).
4. Compile/link flags:

```
-Ilib/td -Ilib/td/build -Llib/td/build -ltdjson
-Wl,-rpath,$ORIGIN/lib/td/build
```

Then `-lncurses` (`nob.c:204-217`).

The `$ORIGIN` rpath makes the binary find the built library relative to its own
location; moving `tgf` without `lib/td/build` breaks it.

## 5. Windows

- `-DTGF_NOGUI` removes all ncurses code (`src/ui.c:29-89`, `src/loop.c:86-97`).
- No `-lncurses`, no rpath.
- If `tdjson.dll` or `tdjson.lib` exists in the working directory, link
  `-ltdjson`; otherwise perform the same ensure/build sequence and add
  `-I/-L` paths (`nob.c:188-202`).
- `cmake_available` redirects output to `/dev/null` (`nob.c:50-55`), a POSIX
  path, which is suspicious under `cmd.exe`. The Windows path is not verified
  in this project (TD-21; platform claims are tracked in TD-18).

## 6. Platform support matrix

| Platform | Status | Notes |
|---|---|---|
| Linux x86_64 | primary, verified in this tree | system lib detection, rpath build, ncurses |
| Linux other arch | expected to work | system-lib path list is x86_64-biased |
| macOS | unverified | no system-lib path (`/opt/homebrew`), `$ORIGIN` rpath is a GNU-ld convention, ncurses from SDK |
| Windows | code path exists, unverified | GUI disabled, `/dev/null` redirection, `tdjson.dll` placement |

## 7. Runtime dependencies of `./tgf`

- `libtdjson` (system copy or `lib/td/build` via rpath).
- `libncurses` (non-Windows builds).
- `config.json` in the CWD.
- Writable CWD for `tdlib_db/`, `tdlib_files/`, `history.txt`,
  `seq_tracker.txt`.
- A valid `TERM` in dashboard mode.
- Network access to Telegram.

## 8. Verification commands

Build:

```sh
./nob
```

Tests (Step 0 of spec 99):

```sh
./nob test
```

Builds `tests/tgf_tests` with `-g -O1 -fno-omit-frame-pointer
-fsanitize=address,undefined` and runs it in a temporary working directory
(`mkdtemp`+`chdir`). It links `tests/test_main.c`, `src/tracker.c`,
`src/history.c`, `src/forward.c`, `src/config.c`, `src/resolve.c`, `src/poll.c`,
and `cJSON.c` with stubbed `transport_send`/`transport_destroy`/`ui_status`/
`ui_oom`/`ui_forwarded`; it does not require TDLib, ncurses, or a session.

`test` then builds and runs `tests/tgf_e2e` (also ASan+UBSan, `TGF_NOGUI`,
temp CWD): the real `loop_tick` + real `transport_send` driven by a fake
`Transport` (`tests/transport_fake.c`) that replays scripted JSON responses and
records sends. Links `tests/test_e2e.c`, `tests/transport_fake.c`, `src/loop.c`,
`src/transport.c`, `src/auth.c`, `src/resolve.c`, `src/poll.c`, `src/tracker.c`,
`src/history.c`, `src/forward.c`, `src/config.c`, `cJSON.c`. No TDLib needed.
`./nob tsan` runs the same e2e under ThreadSanitizer.

Lint (from `AGENTS.md`; compiles `src/main.c` only):

```sh
cc -Wall -Wextra -Wshadow -Wpedantic -fsanitize=address \
   -Ilib/td -Ilib/td/build -Isrc -I. -c src/main.c -o /dev/null
```

Static analysis:

```sh
cppcheck --enable=all --suppress=missingIncludeSystem \
  --suppress=unmatchedSuppression --suppress=normalCheckLevelMaxBranches \
  --suppress=toomanyconfigs *.c src/*.c *.h src/*.h 2>&1 | grep -v 'nob.h:'
```

The lint command does **not** compile `src/tracker.c`, `src/history.c`, or `src/forward.c`;
warnings in those files are only visible from `./nob`. Baseline status:

- `cc -Wall -Wextra -Wshadow -Wpedantic -fsanitize=address -I. -Isrc -c src/main.c`
  (with the TDLib include paths) is clean.
- `./nob` build passes.
- `./nob test` passes 76 handler tests / 308 checks plus 5 fake-transport e2e
  tests / 17 checks under ASan+UBSan; `./nob tsan` runs the e2e clean under
  ThreadSanitizer.
- cppcheck (with `nob.h:` filtered, as in `AGENTS.md`) reports 2 errors in
  vendored `cJSON.c` (`invalidFunctionArg`, lines 113 and 415) and 16 warnings,
  also in `cJSON.c` (unchanged baseline). In `src/` it reports style-level
  findings only: `staticFunction` for `fwd_queue_push`, `group_batch`,
  `group_result_free`, `history_response` and `unusedFunction` for
  `config_free` (all used by `tests/test_main.c`, which is outside the
  cppcheck file set — false positives); `constParameterPointer` suggestions in
  `forward.c`, `main.c`, `resolve.c`; `variableScope` for `batch_oldest_id`
  (`poll.c`); `unusedStructMember` noise on header structs. `nob.c:159`
  `varFuncNullUB` comes from the vendored `GO_REBUILD_URSELF` macro. No
  errors or warnings in `src/`.

### 8.1 CI

`.github/workflows/ci.yml` runs on push/PR to `master`: checkout with
submodules, apt install (build-essential, cmake, libncurses5-dev, cppcheck),
`./nob`, the narrow lint, cppcheck, `./nob test`, `./nob sanitize`. TDLib is
built from the pinned submodule when no system lib is present (slow, `-j1`).

## 9. Edge cases

| Case | Behavior |
|---|---|
| No compiler | `cc` not found; build fails with the shell error |
| No CMake and no system TDLib | `cmake is required to build TDLib from source`, exit 1 |
| TDLib clone fails (offline) | `git clone` failure, exit 1 |
| `lib/td/build` exists but stale | Used as-is; no version check |
| System TDLib older/newer than submodule | Used as-is; JSON API is assumed compatible |
| `./nob clean` | Leaves all user data intact |
| `-O3` on a debug build | Symbols absent; use the manual `cc -g` command from CONTRIBUTING |
| ncurses missing | Header/link error on non-Windows |

## 10. Acceptance scenarios

| ID | Scenario | Expected |
|---|---|---|
| SC-07-01 | Fresh clone with submodule, `gcc -o nob nob.c && ./nob` | `tgf` produced; `./tgf -d` starts |
| SC-07-02 | `./nob` twice | Second run is a no-op except any needed rebuild |
| SC-07-03 | Remove `lib/td/build`, delete `tdlib_db` | TDLib rebuilt (or system lib used), fresh login |
| SC-07-04 | `./nob clean` | `tgf` removed; config/history/tracker/session preserved |
| SC-07-05 | Move `tgf` to another directory | Works only if system `libtdjson` is present |
| SC-07-06 | Move the repo directory | rpath still resolves from the binary’s own dir |

## Amendment log

| Date | Change |
|---|---|
| 2026-11-17 | Baseline created |
| 2026-11-17 | Corrected `tgf.c` citations and the test count to the working tree |
| 2026-11-17 | Step 2: split sources into `src/`; updated build/lint/test commands |
| 2026-11-19 | Documented `sanitize` target and CI workflow; fixed the narrow lint command (added `-I.`); test suite now links `src/resolve.c` (42 tests / 175 checks); recorded the current cppcheck baseline including src style findings; refreshed nob.c citations |
| 2026-11-21 | Added `gui` and `run-gui` targets to `./nob` specification using `pkg-config` for `gtk4` and `libadwaita-1` |
