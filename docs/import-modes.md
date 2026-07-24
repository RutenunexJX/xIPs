# Reference and Vendor imports

Both modes resolve dependencies by stable asset ID before writing anything.
Resolution detects missing required assets, duplicate IDs, cycles, semantic
version conflicts, optional missing assets, and target-tool incompatibility. The
result order is deterministic and dependencies precede their users.

## Reference

Reference mode writes `.xips/references.json` below the target project and does
not copy source files. Each entry records:

- asset ID, manifest version, Git commit, and content hash;
- source root and source-file paths relative to the target where possible;
- include directories and defines needed by downstream tools.

Repeated Reference imports merge by stable asset ID. Existing assets that are
outside the new dependency closure remain present, generated fields for
re-imported assets are refreshed, and unknown top-level or per-asset extension
fields are retained. Invalid or duplicate-ID documents are blocked rather than
overwritten. Execution also verifies that the document has not appeared,
disappeared, or changed since planning.

The configuration is suited to development projects that should follow library
changes. If an asset directory moves, xIPs reports the reference as unavailable.
It can repair the entry by matching the stable ID and expected content hash in
the registered catalog. The desktop inspection action displays valid, missing,
hash-mismatched, and repairable states before an atomic repair write. Repair
publication compares the current file with the exact bytes inspected and
refuses to overwrite a concurrent edit. Windows symbolic links are not
required.

## Vendor

Vendor mode freezes the resolved closure below
`vendor/xips/<asset-id>/` and writes `xips-lock.json`. It includes the manifest,
declared sources, constraints, tests, examples, documentation, and files below
declared include directories. Git metadata, xIPs metadata, generated build/cache
directories, and manifest-declared generated exclusions are not copied.

Planning hashes every source and destination and classifies each file:

- `add`: destination does not exist;
- `overwrite`: destination still has the hash owned by the prior lockfile;
- `conflict`: destination exists but is not safely owned;
- `skip`: destination already equals the requested source.

The desktop application displays the complete plan and requires explicit
confirmation. A plan containing an error or conflict cannot execute. Execution
rechecks all hashes to prevent changes between preview and copy, stages every
file, journals backups, publishes files and lockfile, and rolls back on failure.
Vendor planning and copying run outside the UI thread. Copying is cancellable at
file boundaries; cancellation after publication begins uses the same journaled
rollback. Existing lockfiles must have the supported schema, unique asset IDs,
unique safe relative owned paths, and non-empty ownership hashes. Planning,
execution, and recovery reject path traversal plus symbolic-link or Windows
junction components in metadata, destinations, staging, and backup paths.
Source-tree traversal also skips symbolic links and junctions.
Execution verifies that the lockfile has not appeared, disappeared, or changed
since planning.

Target tool versions can be supplied through the CLI's repeated
`--target-tool name=version` option. Dependency resolution checks every
manifest tool constraint and blocks conflicts. If assets declare tool
requirements but the target versions are absent, the plan records a warning
that compatibility remains unverified.

Absolute files outside an asset root are mapped into a deterministic
`external/<hash-prefix>/` path. Unsafe relative paths are rejected. Operations
are limited to the selected target project.
