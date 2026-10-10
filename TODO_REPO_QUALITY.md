# Repo-quality follow-ups

Working checklist for the quality gates, not a design doc.

## What the gates check now

The original gate failed builds when a comment began with "The " or "This "
unless it matched a ~200-keyword allowlist. That penalised correct why-comments
and caught nothing real; the comment/naming scan is gone (already removed from
`scripts/check_repo_quality.sh`). This pass replaced the remaining
hygiene-only CI with real error detection:

- `build-and-test.yml` `repo-quality`:
  - Hard gates: cppcheck error-class checks (warning/performance/portability,
    exhaustive; calibrated zero, with Qt's Q_SLOTS family suppressed as
    unknownMacro), hygiene (tracked build artifacts, trailing whitespace,
    bare printf-family calls). Style findings (102 today) stay advisory.
  - Reported: compiler warnings under `-Wall -Wextra`, clang-tidy
    (bugprone/clang-analyzer/performance) over the parsing + IPC core.
    Both feed the sticky PR comment.
- `build-and-test.yml` `sanitizers`: ASan+UBSan build and ctest run.
  UB halts (`UBSAN_OPTIONS=halt_on_error=1`). Leak check is ON for the logic
  tests (no GPU drivers linked, so a leak is ours) and OFF only for the
  Qt-Gui/lavapipe rendering paths, where third-party static destructors drown
  real signal.
- `build-and-test.yml` `workflow-lint`: actionlint.
- `security-scan.yml`: gitleaks over the full history (replaces
  `git log | grep -E "(password|secret|key|token)"`, which matched code words
  and missed real credential formats); flawfinder advisory list (replaces an
  `echo` placeholder).

## Why not a small local LLM in CI

GitHub-hosted `ubuntu-latest` = 2 vCPU / 7 GB RAM (standard runners). A ~1B
model would run, but: its output is nondeterministic (a flaky gate people
learn to ignore — the exact failure mode this file warns about), C++ memory
and parsing bugs are where small models are weakest, and the same runner
budget buys strictly more with cppcheck/clang-tidy/ASan/UBSan. If AI review is
wanted later, call a hosted model from an advisory PR-comment job — no runner
CPU cost, and keep it non-blocking.

## Open items

- `png_regression_batch`: 3 of 72 fixtures fail on this machine
  (3601075812.png, 3673417519.png, 3762441477.png). Root cause pending;
  resolve (code fix vs stale baseline) before adding new hard gates.
- The compiler-warning step is advisory because the tree is not warning-clean:
  96 occurrences / 19 distinct (file,type) under `-Wall -Wextra`, dominated by
  unused parameters in deliberate stubs. Clean up opportunistically, then flip
  the step to a hard gate (consider gating with `-Wno-unused-parameter` once
  the non-stub warnings are gone).
- clang-tidy is advisory; promote individual checks to hard as findings are
  triaged.
- gitleaks image tag is pinned (`zricethezav/gitleaks:v8.21.2`); bump
  periodically. Full-history scans need `fetch-depth: 0` (kept).
- Heavier next options if the advisory layer keeps finding things: CodeQL
  (C/C++), Valgrind on the logic tests. Skipped for now — slow, and the
  current stack covers most of the same ground.
