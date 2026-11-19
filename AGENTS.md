# TGF-C — Agent Instructions

## Non-negotiable: spec-driven development

**No code change without a spec change first.** "Spec" means a file under
`specs/`. The only exceptions are typo fixes in comments and formatting with no
behavioral meaning.

Before editing code:

1. Find the owning spec in the map below. If none owns the behavior, create a
   new spec file and add it to `specs/README.md` **before** writing code.
2. Amend the spec: behavior, edge cases, acceptance scenarios, and an
   Amendment-log row.
3. Implement against the amended spec.
4. Run the gates in "Definition of done".
5. Update `specs/99-tech-debt-and-cleanup.md` if the change closes or creates
   an item.

Rules:

- **Behavior changes** (feature, bug fix, UX change, config change) require a
  spec edit. A bug fix must name the spec clause it restores.
- **Pure refactors** must not edit spec text. If a refactor needs a spec
  change, it is not a refactor — split it into two changes.
- If code and spec disagree, stop and report the discrepancy. Do not "fix" the
  code to match your assumption, and do not silently update the spec.
- Specs describe current behavior (as-built) unless a clause explicitly says
  "proposed". Proposals belong in 99, not in feature specs.

## Spec map

| ID | File | Owns |
|---|---|---|
| 00 | `specs/00-architecture.md` | System shape, modules, execution model, state, boundaries |
| 01 | `specs/01-runtime-and-auth.md` | Startup, config load, auth, resolution, shutdown |
| 02 | `specs/02-polling-and-dedupe.md` | Polling, cutoff, dedupe set, pruning |
| 03 | `specs/03-forwarding.md` | Reply/album grouping, queue, delay, errors |
| 04 | `specs/04-sequential-backfill.md` | Sequential mode, tracker, backfill, resume |
| 05 | `specs/05-config-and-storage.md` | Config schema, file formats, paths, secrets |
| 06 | `specs/06-ui-and-cli.md` | argv, streams, status channel, dashboard |
| 07 | `specs/07-build-and-platform.md` | `nob` build, TDLib bootstrap, platforms |
| 99 | `specs/99-tech-debt-and-cleanup.md` | Debt register and refactor plan |

`specs/README.md` is the index and change protocol.

## Read before working

1. `specs/00-architecture.md` — where state lives and how the loop runs.
2. The owning spec for your task.
3. `specs/99-tech-debt-and-cleanup.md` §4 — behaviors that must be preserved
   even though they look like bugs.

## Build

```sh
./nob              # build (cc -Wall -Wextra -O3, links libtdjson + ncurses)
./nob clean        # remove lib/td/build, tgf, tests/tgf_tests, nob.old
./nob sanitize     # build tgf with ASan/UBSan (-g -O1), POSIX only
```

First-time bootstrap: `gcc -o nob nob.c && ./nob`.

Compiles `cJSON.c` + `src/tracker.c` + `src/history.c` + `src/forward.c` +
`src/config.c` + `src/main.c` + `src/auth.c` + `src/resolve.c` + `src/poll.c` +
`src/transport.c` + `src/ui.c` in one invocation. No flags are needed to invoke it; `nob` supplies
`-Wall -Wextra` and `-O3`. It auto-detects a system `libtdjson` or builds the
`lib/td` submodule with CMake (`-j1`). See spec 07 before changing the build.

## Lint

Narrow check (only `src/main.c`):

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

`./nob` is the only check that covers `src/tracker.c`, `src/history.c`, and
`src/forward.c`.
Never weaken or suppress a warning to make a change land.

## Test

```sh
./nob test         # build + run the characterization tests (ASan/UBSan)
./tgf -d           # manual: debug mode, resolution + forwarded lines on stdout,
                   # statuses on stderr, no ncurses
```

`tests/test_main.c` links `src/tracker.c`, `src/history.c`, `src/forward.c`,
`src/config.c`, `src/resolve.c`, and `cJSON.c`; it stubs `transport_send`/
`transport_destroy`/`ui_status`/`ui_oom`/`ui_forwarded` and runs in a temporary CWD, so
it never touches real config, session, history, or tracker files. It
intentionally locks in current behavior, including known quirks (TD-04, TD-22).
Changing an assertion is a behavior change: amend the owning spec first.

For any change, walk the scenarios in the owning spec (§"Acceptance
scenarios") and state which ones you ran. Backfill completion heuristics are
not covered yet; they become testable with the Step 2 handler seam.

## Code style

- **static** for file-internal functions and globals.
- **extern** in `src/tgf.h` for cross-file globals and declarations — transitional
  convention, being replaced by explicit `Runtime`/`Config` passing; after the
  spec-99 refactor, prefer module-private state and passing `Config`/`Runtime`
  explicitly; do not add new globals without a spec change.
- No comments on implementation code (minimal comments only in public headers).
- Single-line `if`/`for` bodies: no braces.
- Error paths: `ui_status("message")` for recoverable, `fprintf(stderr, ...)`
  for fatal. Preserve the exact strings listed in the specs.
- Variable naming: snake_case, no prefixes.
- Pointer alignment: `*` attached to the name (`char *p`, not `char* p`).

## Project structure (current)

| File | Purpose |
|---|---|
| `src/main.c` | Entry point, main loop, dispatch, shutdown, config adapter, static `Runtime` |
| `src/auth.c` | TDLib auth state machine + prompts |
| `src/resolve.c` | Destination/source resolution |
| `src/poll.c` | Poll request + history handler |
| `src/transport.c` | TDLib JSON send (`transport_send`) + client destroy (`transport_destroy`) |
| `src/ui.c` | Status sink + ncurses dashboard |
| `src/config.c/h` | `config.json` parsing, defaults, `Config` value (no globals) |
| `src/tgf.h` | Shared types, macros, `Runtime` struct, cross-file declarations |
| `src/tracker.c` | Sequential-forwarding tracker (load/save/get/set, pending, flush) |
| `src/history.c` | History hash set (load/has/add/prune, FNV-1a) |
| `src/forward.c` | Forward queue, grouping (replies, albums), dispatch |
| `nob.c` | Build system |
| `nob.h` | Build library (vendored, unmodified) |
| `cJSON.c/h` | JSON parser (vendored, unmodified) |
| `lib/td/` | TDLib submodule (do not edit) |
| `specs/` | Living specifications (edit these first) |

Target layout and sequencing are in `specs/99-tech-debt-and-cleanup.md` §3. Steps 1–3
(seams, `src/` split, structural cleanups) and Step 4.1 (resolution fail-fast)
are done; Steps 4.2–5 remain.

## Architectural boundaries

- All Telegram traffic goes through the TDLib JSON client and the request
  helpers in `src/transport.c`. Do not call TDLib from new modules without going
  through the transport seam.
- Storage formats are contracts: `history.txt` and `seq_tracker.txt` are
  specified in 05. Do not change a field, timestamp basis, or write mode
  without a spec amendment.
- The dedupe decision happens before forwarding (spec 03 §7). Do not "fix" that
  in a refactor.
- The main loop is single-threaded. Do not introduce threads without a spec.
- Polling is timed (5 s), not event-driven. Real-time updates are not part of
  the baseline.
- Every path is CWD-relative. Do not add `$HOME`/XDG paths as a drive-by.

## Configuration and secrets

- `config.json` is required in the CWD; schema and defaults are in spec 05.
- Never commit `config.json`, `config.bak`, `history.txt`, `seq_tracker.txt`,
  `tdlib_db/`, `tdlib_files/`, or binaries. They are gitignored; keep it that
  way.
- Never paste `api_hash`, session data, or forwarded message content into
  specs, commits, issues, or test fixtures.
- Treat `config.json` and `config.bak` as secrets (TD-20).

## Definition of done

A change is done only when all of the following are true:

1. The owning spec was amended first (or a new spec was added and indexed).
2. `./nob` succeeds.
3. The narrow lint command succeeds; cppcheck has no new findings.
4. New behavior has tests when a test target exists; otherwise the relevant
   acceptance scenarios were run manually and their output is summarized in
   the change description.
5. The change does not alter behavior outside the spec clauses it touches.
6. Any newly discovered debt or removed debt is reflected in spec 99.

"This should work" is not done. Editing the file is not done. Observed output
is done.
