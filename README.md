# xIPs

xIPs is a standalone Qt desktop application for managing reusable FPGA assets:
Code Blocks, independently analyzable Modules, and multi-file IP packages. Source
directories and Git repositories remain the source of truth; SQLite is a
deletable index and activity/result cache only.

The first screen is the asset library. It provides a compact sortable table,
type/tag/project/status filters, indexed fuzzy search, an Inspector, source
preview with multi-file selection, and dependency/version/test/usage panels.
Registering an external root indexes assets in place and never moves or copies
source files.

## Build

Prerequisites:

- Qt 6.5 or newer with Core, Concurrent, SQL, Widgets, and Test
- CMake 3.25 or newer
- Ninja
- A C++20 compiler

Windows example for the installed Qt 6.10.2 toolchain:

```powershell
$env:PATH = "E:\QT6\Tools\mingw1310_64\bin;E:\QT6\6.10.2\mingw_64\bin;$env:PATH"
E:\QT6\Tools\CMake_64\bin\cmake.exe -S . -B build -G Ninja `
  -DCMAKE_PREFIX_PATH=E:\QT6\6.10.2\mingw_64 `
  -DCMAKE_MAKE_PROGRAM=E:\QT6\Tools\Ninja\ninja.exe `
  -DCMAKE_CXX_COMPILER=E:\QT6\Tools\mingw1310_64\bin\g++.exe
E:\QT6\Tools\CMake_64\bin\cmake.exe --build build
E:\QT6\Tools\CMake_64\bin\ctest.exe --test-dir build --output-on-failure
```

Run the example library:

```powershell
.\build\src\xips.exe --library .\examples\library
```

Create an install tree and deploy the Qt runtime on Windows:

```powershell
E:\QT6\Tools\CMake_64\bin\cmake.exe --install build --prefix build\install
E:\QT6\6.10.2\mingw_64\bin\windeployqt.exe `
  --no-translations --compiler-runtime build\install\bin\xips.exe
.\build\install\bin\xips.exe
```

The installed executable locates the installed example library under
`share/xips/examples/library`. The same install tree contains `xips-cli.exe`,
the format/behavior documentation, and the example assets.

The application index is stored below Qt's per-user application-data directory,
not in the asset source tree. Removing the SQLite file cannot remove source
assets; **Library > Rebuild index** recreates derived records from manifests
and declared files. Cached test results and last-used timestamps are
convenience state and reset with that database.

## Current implementation

Phase 1 is implemented and verified:

- versioned `.xips.json` parsing, validation, version-0 migration, unknown-field
  preservation, and atomic replacement;
- relative-path asset discovery, duplicate-ID rejection, missing-file
  diagnostics, deterministic content hashes, and relocatable external roots;
- rebuildable SQLite indexing with explicit indexes, generation protection,
  selective trigram fuzzy search that reports matching fields, and a
  source-preserving last-used activity cache;
- incremental per-asset refresh for manifest, source, constraint, test,
  documentation, and include-tree changes, with full rebuild reserved for
  library-structure changes;
- Code Block, Module, and IP example assets;
- asynchronous indexing and the desktop asset-browser UI, including selectable
  previews for every declared source in a multi-file asset and an explicit
  non-decoding placeholder for binary FPGA artifacts.

Verification on 2026-07-24 used Qt 6.10.2, MinGW 13.1, CMake 3.30, and
Ninja. The application and `tst_phase1` built successfully; all 10 phase-1 test
cases passed through CTest.

Phase 2 is implemented and verified:

- Slang is invoked out of process with AST JSON, CST JSON, diagnostic JSON, and
  include-dependency output;
- module, interface, package, parameter/localparam, port type/dimensions,
  package import, instance dependency, diagnostic, and top-candidate facts are
  indexed from Slang output only;
- manifest include directories and defines are passed explicitly to Slang;
- unchanged hashes reuse semantic cache entries, changed hashes mark old
  semantics stale, and publication requires matching asset ID, content hash,
  and generation;
- if Slang is missing, xIPs records an explicit `unavailable` diagnostic and
  does not fall back to source regex or line scanning.

The phase-2 test uses a separate protocol fixture executable to validate process
integration without creating a second HDL parser. CTest `phase1` and `phase2`
both pass; 14 functional cases are covered. A real Slang executable can
be selected with `XIPS_SLANG` or made available in `PATH`.

Phase 3 is implemented and verified:

- stable-ID dependency resolution supports recursive closure, optional
  dependencies, deterministic topological order, semantic-version constraints,
  cycles, duplicates, missing assets, and target-tool conflicts;
- Reference import merges repeated imports without dropping prior entries or
  extension fields, writes relocatable source/include/define metadata without
  copying source, and can repair moved entries by ID and content hash;
- Vendor import previews add/overwrite/conflict/skip actions, protects
  user-modified files through lockfile ownership hashes, and performs confirmed,
  staged, journaled, rollback-safe copies;
- source and destination hashes are rechecked at execution to close the
  plan-to-copy race, while generated/cache directories are excluded;
- malformed, path-escaping, duplicate-owned, concurrently changed, or
  link-backed Reference/Vendor metadata is rejected before mutation;
- Reference repair uses the exact inspected-file hash and refuses to overwrite
  a configuration edited before repair publication;
- Vendor staging and publication are cancellable at file boundaries, and
  recovery refuses symbolic-link transaction directories;
- the desktop File menu exposes Reference and Vendor plan preview; planning and
  Vendor execution run outside the UI thread;
- Reference configurations can be inspected in the desktop app and repaired
  atomically by matching registered asset ID and expected content hash.

CTest `phase1` through `phase3` pass; 25 functional cases are covered.
See [Reference and Vendor imports](docs/import-modes.md) and the
[`xips-lock.json` format](docs/lockfile-format.md).

Phase 4 is implemented and verified:

- bounded, cancellable Git queries provide repository root, commit, exact tag,
  branch, and working-tree state without history scans;
- comparisons classify file, manifest, Slang semantic, and dependency changes,
  and explicitly omit semantic comparison when Slang is unavailable;
- structured manifest test commands run directly through QProcess only after
  confirmation, stream bounded logs, support cancellation, and record command,
  working directory, exit, duration, commit, and content hash;
- SQLite caches the latest result per asset/command and rejects publication from
  a stale generation or hash; subsequent source changes expose old results as
  stale;
- Vendor upgrade preview reports asset version/content changes, protects
  modified obsolete files, and transactionally removes only lockfile-owned
  files.

CTest `phase1` through `phase4` pass; 32 functional cases are covered. See
[Version, difference, and test behavior](docs/versioning-and-tests.md).

Phase 5 is implemented and verified:

- `xips-cli` returns a versioned JSON envelope for catalog, URI, module
  registration, managed creation, project import, and Code Block operations;
- desktop and CLI `--help`/`--version` paths are non-interactive and terminate
  without entering the desktop event loop;
- mutating CLI operations are preview-only unless `--execute` is present;
- desktop source/directory/Git-checkout discovery uses Slang, lets the user
  select a top, and creates an atomic in-place manifest without moving or
  copying source;
- managed Module creation can start empty, from the selected module, from one
  source file, or from an existing directory; the complete file plan is
  previewed and a staging-directory publish prevents half-created assets;
- Reference/Vendor CLI import exposes the complete plan and reuses the same
  dependency, conflict, transaction, and lockfile services as the desktop app;
- ordered Code Block slot metadata is exchanged through an atomic JSON handoff,
  including example input/output, while `zeroslack://open` and
  `zeroslack://insert-code-block` provide explicit outbound URI boundaries;
- duplicate catalog IDs are rejected before managed creation or in-place
  registration, and explicit target-tool versions enable compatibility checks;
- managed creation and in-place registration revalidate manifest identity,
  destination confinement, source availability, and link state at execution,
  so forged or stale preview plans cannot redirect a write;
- no integration path reads another application's database.

CTest `phase1` through `phase5` plus the headless desktop smoke suite cover 47
functional cases. See [CLI and cross-application integration](docs/cli-and-integration.md).

## Final verification

On 2026-07-24, a clean Release build in `build-usable` completed all 92 build
steps with Qt 6.10.2, MinGW 13.1.0, CMake 3.30.5, and Ninja 1.12.1. CTest passed
6/6 suites in 7.36 seconds: phase suites contain 10, 4, 11, 7, and 12
functional cases, and the offscreen GUI suite contains 3, for 47 total.
Offscreen screenshots verified relevance-ordered search, readable short-source
preview, switching between files in a multi-source asset, and safe binary-file
preview behavior. The Debug build also passed all suites with Qt assertions
enabled.

`cmake --install` plus `windeployqt` generated
`build-usable/install/bin/xips.exe` and `xips-cli.exe`. With Qt and MinGW
development directories removed from `PATH`, both deployed executables passed
loader/version startup checks with exit code 0, and the deployed CLI cataloged
all four installed example assets. The offscreen GUI harness also passed with
the installed Qt runtime and plugin directory selected. No remote push was
performed.

Detailed evidence is recorded in [PLAN.md](PLAN.md). The long-term
invariants are recorded in [GOAL.md](GOAL.md), and the complete source-artifact
inventory is in [Changed files](docs/changed-files.md).

## Known limitations and follow-up

- This workstation has no upstream Slang executable. Process integration and
  artifact parsing are tested with a protocol fixture; production semantic
  extraction requires a compatible `slang` binary through `XIPS_SLANG` or
  `PATH`.
- ZeroSlack URI dispatch requires an operating-system `zeroslack://` handler.
  xIPs implements the outbound protocol but does not install or emulate
  ZeroSlack.
- Vivado packages are deliberately opaque. xIPs validates declared target-tool
  versions when supplied, but does not invoke Vivado or parse generated
  implementation directories.
- `QFileSystemWatcher` is subject to operating-system watch limits. Manual
  **Library > Rebuild index** remains the recovery path for very large trees or
  missed platform notifications.
- The dependency panel is a compact relationship view rather than a graphical
  editor, and version comparison is manifest/worktree based rather than a Git
  history browser.
- The Windows install tree is deployable but is not an installer and is not
  code-signed. Follow-up work includes upstream-Slang CI, installer/signing,
  richer dependency visualization, and inbound ZeroSlack/Pinloom protocol
  validation.
