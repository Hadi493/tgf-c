# TGF-C Specifications

Status: **baseline (as-built)**, generated 2026-11-17 from the working tree at
commit `fa1181a` plus the uncommitted Step 1–2 refactor (`Config`/`Runtime`,
seams, `src/` split). Reconciled 2026-11-19 with the Step 3 structural work
(ring-buffer queue, `ui_oom`, dead-code removal, parse-once, CI) and the
restored Step 4.1 resolution fail-fast behavior.

These specs describe what the program **does today**, not what it should do.
Every statement is evidence-backed with `path:line`. Where the code is
surprising or lossy, the spec records the behavior and points at the owning
tech-debt item in [99-tech-debt-and-cleanup.md](99-tech-debt-and-cleanup.md).

## Why these files exist

TGF-C was built iteratively by prompting. The specs are the reverse-engineered
contract that lets future changes be reviewed against something other than
memory. They are **living documents**: a code change that alters observable
behavior must land with a spec change in the same change set.

## Spec map

| ID | File | Owns |
|---|---|---|
| 00 | [00-architecture.md](00-architecture.md) | System shape, modules, execution model, state, boundaries |
| 01 | [01-runtime-and-auth.md](01-runtime-and-auth.md) | Startup, config load, TDLib auth, channel resolution, shutdown |
| 02 | [02-polling-and-dedupe.md](02-polling-and-dedupe.md) | Poll contract, history cutoff, dedupe set and file, pruning |
| 03 | [03-forwarding.md](03-forwarding.md) | Reply/album grouping, queue, delay, `forwardMessages`, error path |
| 04 | [04-sequential-backfill.md](04-sequential-backfill.md) | Sequential mode, tracker file, backfill phases, resume |
| 05 | [05-config-and-storage.md](05-config-and-storage.md) | `config.json` schema, on-disk formats, directories, secrets |
| 06 | [06-ui-and-cli.md](06-ui-and-cli.md) | argv, stdout/stderr, status channel, ncurses dashboard |
| 07 | [07-build-and-platform.md](07-build-and-platform.md) | `nob` build, TDLib bootstrap, platform differences |
| 99 | [99-tech-debt-and-cleanup.md](99-tech-debt-and-cleanup.md) | Prioritized structural debt and fix sequencing |

`AGENTS.md` at the repository root is the entry point for agents and points
here. `specs/acceptance.md` is the manual verification checklist derived from
the scenario tables below (56 scenarios); it is not a numbered spec.

## Normative language

- **MUST / MUST NOT** — invariant of the current implementation. Changing it is
  a behavior change and needs an amended spec.
- **SHOULD** — intended design where the implementation is known to be weak;
  used only in "Known limitations" and tech-debt sections.
- Present tense elsewhere is descriptive ("the loop sends …"), not a wish.

## Change protocol

1. Find the owning spec (table above). If none exists, add one before coding.
2. Amend the spec first: behavior, edge cases, acceptance scenarios, and an
   entry in the spec's Amendment log.
3. Implement.
4. Run the build and lint gates in `AGENTS.md`; walk the scenarios the change
   touches.
5. Update `99-tech-debt-and-cleanup.md` if the change closes or creates an item.

Pure refactors (no observable change) MUST NOT edit spec text. If a refactor
needs a spec edit, it is not a refactor.

## Conventions

- Evidence citations are `path:line` against the baseline above. Line numbers
  are allowed to drift; when they do, fix the citation in the same change that
  moves the code.
- "Edge case" means reachable in normal use, not a hypothetical.
- Acceptance scenarios are manual and use `./tgf -d` unless stated otherwise.
  The automated harness is the characterization suite (`./nob test`); the
  remaining scenarios are manual (see TD-17).
- Configuration values are referenced by key name only. No live credentials
  appear in these files.

## Non-goals

- Documenting `cJSON`, `nob.h`, or TDLib internals. Only TGF-C's use of them.
- Designing future features. Proposals live in 99 as debt items and need their
  own spec when picked up.
- Reproducing README/CONTRIBUTING prose.
