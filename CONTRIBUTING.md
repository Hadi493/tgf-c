# Contributing to TGF-C

First off, thank you for considering contributing. TGF is a small project with big ambitions — help us make it better.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [What We Need Help With](#what-we-need-help-with)
- [Getting Started](#getting-started)
- [Development Setup](#development-setup)
- [Coding Guidelines](#coding-guidelines)
- [Commit Messages](#commit-messages)
- [Pull Request Process](#pull-request-process)
- [Reporting Bugs](#reporting-bugs)
- [Feature Requests](#feature-requests)

## Code of Conduct

This project adheres to the [Contributor Covenant](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code.

## What We Need Help With

| Area | Difficulty | Good for beginners? |
|---|---|---|
| Bug fixes in existing features | Easy–Medium | Yes |
| Test coverage | Easy | Yes |
| Documentation | Easy | Yes |
| CI/CD improvements | Medium | Yes |
| New forwarding modes | Medium–Hard | No |
| TDLib integration features | Hard | No |
| Cross-platform support | Medium | No |
| Performance optimization | Hard | No |

Check [issues labeled `good first issue`](https://github.com/Hadi493/tgf-c/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22) for starter tasks.

## Getting Started

```bash
git clone https://github.com/Hadi493/tgf-c.git
cd tgf-c
cp config.json.example config.json
gcc -o nob nob.c
./nob
```

This builds the `tgf` binary. You need:
- GCC or Clang
- CMake & make (only if building TDLib from source)
- ncurses dev library (Linux/macOS only)

## Development Setup

### For debugging

```bash
./nob sanitize     # builds ./tgf with ASan/UBSan (-g -O1)
./tgf -d
```

### For testing

```bash
./nob test         # characterization suite, ASan/UBSan, temporary CWD
```

## Coding Guidelines

### Style

- **Indentation:** 4 spaces (no tabs)
- **Braces:** K&R style (opening brace on same line)
- **Line length:** max 100 characters
- **Names:** `snake_case` for functions and variables, `UPPER_CASE` for macros
- **Comments:** Only when the "why" is not obvious. Don't comment "what" the code does.

### Patterns to follow

```c
// Good — early return
static int do_thing(int arg) {
    if (!arg) return -1;
    // ...
}

// Good — error checking
FILE *f = fopen(path, "r");
if (!f) return -1;

// Good — sizeof for safety
snprintf(buf, sizeof(buf), "%lld", val);
```

### What to avoid

- Global state when possible (accept `void *ctx` instead)
- Fixed-size buffers without bounds checking (use `snprintf`)
- Signal-unsafe calls in signal handlers
- Memory leaks — every `malloc` needs a matching `free`

## Commit Messages

```
component: brief description

Optional longer explanation — what and why, not how.
```

Examples:
```
history: fix rehash crash when table size is 0

The calloc could return NULL when new_size was 0.
Added early return check.
```

```
build: add macOS support via Homebrew ncurses
```

## Pull Request Process

1. **Spec first.** Read `AGENTS.md` and `specs/README.md`: amend the owning
   spec before changing behavior.
2. **Small PRs are better.** One feature or fix per PR.
3. **Run `./nob` and `./nob test`** before submitting — both must pass.
4. **Update README.md** if you change configuration or behavior.
5. **No changelog needed** — we maintain that separately.
6. **Don't bump version.** We do that at release time.

### PR Checklist

- [ ] `./nob` builds without errors
- [ ] `./nob test` passes
- [ ] Owning spec amended first (behavior changes)
- [ ] Code follows existing style (4-space indent, K&R braces)
- [ ] No new compiler warnings
- [ ] Config changes are documented in README

## Reporting Bugs

Open an issue at https://github.com/Hadi493/tgf-c/issues/new/choose

Include:
- OS and version
- `tgf` version (commit hash; there is no `-v` flag)
- Config file (redact API credentials)
- Steps to reproduce
- Expected vs actual behavior

## Feature Requests

Open a discussion at https://github.com/Hadi493/tgf-c/discussions first.

Feature requests should include:
- Real-world use case
- How it would work (rough design)
- Would you implement it yourself? (Yes = higher priority)

---

*Keep it simple. Keep it working. Don't add bloat.*
