# xIPs implementation plan

## Phase 1 — Asset library foundation

Status: complete and verified.

- Domain model for Code Block, Module, IP, dependencies, slots, diagnostics,
  semantic results, and indexed asset records.
- Manifest schema validation and in-memory v0-to-v1 migration.
- Unknown JSON fields retained during safe `QSaveFile` replacement.
- Recursive manifest discovery for managed and external roots without source
  movement or copying.
- Duplicate-ID and missing-file diagnostics.
- Deterministic content hashing and relocatable relative paths.
- Rebuildable SQLite cache with explicit indexes, generation publication guards,
  selective indexed fuzzy search, last-used activity timestamps, and per-asset
  incremental update/remove operations.
- Non-blocking library rebuild, sortable/filterable asset table, Inspector,
  multi-source preview selection, binary-artifact preview protection, and
  detail panels.
- Debounced manifest/source/include-tree monitoring refreshes only affected
  assets and preserves unchanged asset generations.
- Example Code Block, package/module, and opaque Vivado IP assets.
- Automated phase-1 tests, including a 2,000-asset cached-search case.

Evidence: Qt 6.10.2/MinGW Release build succeeded; CTest `phase1` passed all 10
functional cases on 2026-07-24.

## Phase 2 — Slang semantic analysis

Status: complete and verified.

- Invokes Slang as the sole SystemVerilog semantic authority.
- Parses Slang AST/CST/dependency/diagnostic JSON without source regex scanning.
- Extracts units, parameters, ports, dimensions, imports, instances, includes,
  defines, diagnostics, dependencies, and top candidates.
- Preserves cached semantics for unchanged content, marks changed content stale,
  and publishes only when asset ID, hash, and generation still match.
- Reports missing Slang explicitly; no heuristic fallback exists.

Evidence: the real process boundary is exercised by a dedicated fixture
executable, and CTest `phase1` plus `phase2` cover 14 cases. Console output is
drained and bounded to prevent an analysis process from blocking on output. The
local machine has no real Slang installation, so an end-to-end run against
upstream Slang remains environment-dependent.

## Phase 3 — Reference and Vendor import

Status: complete and verified.

- Deterministic dependency closure, optional/missing/cycle/duplicate/version/tool
  conflict checks, and dependency-first topological ordering.
- Relocatable Reference configuration with explicit source/include/define
  metadata, non-destructive repeated-import merging, extension preservation,
  and ID/content-hash repair after source relocation.
- Previewed Vendor add/overwrite/conflict/skip plan, file ownership hashes,
  plan-to-copy hash checks, transaction journaling/recovery, rollback, and a
  deterministic lockfile.
- Cancellable Vendor execution at file boundaries and symbolic-link rejection
  for planning, execution, and transaction recovery.
- Strict Reference/Vendor metadata validation rejects invalid JSON/schema,
  duplicate ownership, path traversal, link/junction escapes, and changes
  between planning and execution.
- Desktop plan preview and explicit confirmation; planning and file copying are
  dispatched outside the UI thread. Reference state can be inspected and
  atomically repaired by ID/content hash, with publication rejected if the
  inspected configuration changed before repair.

Evidence: CTest `phase1` through `phase3` cover 25 functional cases, including
deterministic closure/lockfiles, conflict protection,
rollback injection, Reference merge/repair, concurrent metadata changes,
malicious lockfile paths, and workspace isolation.

## Phase 4 — Versions, differences, and tests

Status: complete and verified.

- Bounded, cancellable Git commit/tag/branch/status queries with time and output
  limits; no history scan.
- File/manifest/Slang-semantic/dependency differences computed outside the UI
  thread.
- Structured, explicitly confirmed QProcess test execution with streaming
  bounded logs, cancellation, provenance, SQLite caching, and hash/generation
  stale-result protection.
- Vendor upgrade preview with asset changes, owned-file removal, conflict
  protection, and transactional rollback.

Evidence: CTest `phase1` through `phase4` cover 32 functional cases. Phase-4
cases use a real temporary Git repository
and process fixture and verify all four diff categories, cancellation,
provenance, stale publication rejection, and upgrade-before-mutation behavior.

## Phase 5 — Cross-application interfaces

Status: complete and verified.

- Stable, schema-versioned CLI JSON envelopes and documented exit codes.
- Non-interactive desktop/CLI help and version startup paths suitable for
  automation and deployment loader checks.
- Preview-by-default module registration and Reference/Vendor import; all writes
  require `--execute`.
- Previewed, staged managed-Module creation from an empty template, selected
  Module, source file, or existing directory, with failure/cancellation cleanup.
- Desktop file/directory/Git-checkout registration uses Slang for top and
  dependency-unit discovery, requires user confirmation, and leaves sources in
  place.
- Versioned `zeroslack://open` and `zeroslack://insert-code-block` outbound URIs.
- Atomic Code Block handoff with ordered slot metadata and example input/output.
- Duplicate-ID prevention and explicit target-tool version checking.
- Execution-time validation confines managed creation and in-place
  registration to their previewed destinations, rejects link-backed roots or
  sources, and blocks forged or stale plans before publication.
- Desktop actions for opening assets and sending Code Blocks through the same
  file/URI contracts, without cross-application database access.

Evidence: CTest `phase1` through `phase5` cover 44 core functional cases.
Phase-5 tests execute the built CLI as a child
process and verify JSON contracts, preview/write separation, module registration
without source copying, managed publication/rollback/cancellation, project
imports, target-tool conflicts, URIs, and Code Block handoff.

## Final verification

Status: complete and verified.

- A clean Release configuration in `build-usable` compiled all 92 build steps
  with Qt 6.10.2, MinGW 13.1.0, CMake 3.30.5, and Ninja 1.12.1.
- CTest passed 6/6 suites in 7.36 seconds: phase counts are 10, 4, 11, 7, and
  12, plus 3 offscreen GUI cases, for 47 functional cases total. The Debug
  build passed the same suites in 7.53 seconds with Qt assertions enabled.
- GUI smoke coverage verifies the first-screen asset library, relevance-ordered
  selective search, short-file preview, binary-file handling, multi-source
  switching, and per-asset include-file refresh without changing an unaffected
  asset generation.
- CMake installation plus `windeployqt` produced
  `build-usable/install/bin/xips.exe` and `xips-cli.exe` with examples and
  documentation. Both deployed executables returned exit code 0 with the Qt
  development directories removed from `PATH`; the deployed CLI also cataloged
  all four installed example assets, and the offscreen GUI harness passed with
  the installed Qt runtime/plugin directory selected.
- Formats, behavior, source inventory, known limitations, and follow-up scope
  are documented. No remote push was performed.
