# xIPs long-term goal

Deliver a reliable personal FPGA reusable-asset manager whose source directories
and Git repositories are the only facts of record. xIPs must remain independent
from ZeroSlack and Pinloom while exposing stable file, CLI, and URI integration
boundaries.

## Invariants

- SQLite contains only disposable indexes, cached semantics/test results, and
  convenience activity timestamps.
- External registration never moves or copies source.
- Manifest and lockfile writes are atomic and preserve extensions.
- SystemVerilog facts come from Slang; no fallback source regex parser exists.
- Dependency resolution and import planning are deterministic and independent of
  Qt Widgets.
- User-modified destination files are never silently overwritten.
- Long-running analysis, indexing, Git, test, and copy work stays off the UI
  thread and supports cancellation where applicable.
- Results are published only for the matching asset identity, content hash, and
  generation.

## Milestone state

- Phase 1: complete; rebuildable/incremental indexing, selective fuzzy search,
  text/binary-safe multi-source preview, last-used caching, and 10 automated
  cases verified.
- Phase 2: complete; Slang protocol parsing and stale-generation guards verified.
- Phase 3: complete; deterministic Reference/Vendor planning, non-destructive
  Reference merge, metadata/path confinement, ownership conflicts,
  cancellation, guarded Reference repair, transactional rollback, and 11
  phase-specific cases verified.
- Phase 4: complete; bounded Git facts, structured differences, confirmed and
  cancellable test execution, stale-result guards, and upgrade preview verified.
- Phase 5: complete; stable JSON CLI, explicit execution gates, atomic handoff,
  confined/stale-safe managed creation, in-place Slang-assisted registration,
  and outbound ZeroSlack URI boundaries verified without shared databases.
- Final acceptance: complete; a clean Release build passed 6/6 CTest suites and
  47 functional cases, including three offscreen GUI cases. A self-contained
  Windows deployment tree was generated and its GUI/CLI executables passed
  loader startup checks without the Qt development `PATH`; the deployed CLI
  cataloged the installed example library, and the offscreen GUI harness passed
  against the installed Qt runtime/plugin directory.

The completed Codex goal covered all milestones, phased updates to this file,
`PLAN.md`, and `README.md`, and an evidence-based report of behavior,
limitations, and changed files. No remote push was part of the goal.
