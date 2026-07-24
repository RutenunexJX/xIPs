# Version, difference, and test behavior

## Version facts

xIPs does not maintain an internal version history. The Versions panel combines
facts from the asset source:

- manifest semantic version;
- current Git commit and exact tag;
- bounded Git working-tree status;
- deterministic content hash;
- latest cached test status.

Git integration runs `rev-parse`, exact-tag, and porcelain-status queries only.
Each process has a timeout and a 4 MiB output limit, and indexing cancellation
terminates the query. It never scans full Git history. A library root that is not
a Git work tree remains usable and is marked `not-versioned`.

## Structured differences

The comparison command reads another `.xips.json` and reports four independent
categories:

- file additions, removals, and SHA-256 changes;
- manifest field changes;
- Slang-derived unit, port, parameter, import, instance, include, define, and
  top-candidate changes;
- dependency additions, removals, and version/optional changes.

File hashing and Slang analysis run outside the UI thread. If Slang data is
unavailable for either side, semantic differences are omitted explicitly rather
than inferred from source text.

## Test commands and results

Schema version 1 supports structured commands:

```json
{
  "testCommands": [
    {
      "name": "elaboration",
      "program": "slang",
      "arguments": ["--top", "top_tb", "rtl/top.sv", "tb/top_tb.sv"],
      "workingDirectory": "."
    }
  ]
}
```

xIPs passes `program` and `arguments` directly to `QProcess`; it does not invoke
a shell or interpret a command string. A repository-provided command is shown
with its resolved working directory and requires explicit user confirmation.
The working directory must resolve within the asset root.

Standard output and standard error stream into the Tests panel without blocking
the UI. Runs can be cancelled. The result records command, arguments, working
directory, UTC start time, exit code/status, duration, bounded logs, Git commit,
and content hash. The disposable SQLite cache stores the latest result per
asset/command. Publication requires the same asset ID, content hash, and index
generation; a later content hash or commit marks an older result stale.

## Vendor upgrades

An existing `xips-lock.json` turns Vendor planning into an upgrade preview.
The preview classifies assets as added, removed, changed, or unchanged, and
still shows every file action before mutation. Previously owned files that
disappear from the new asset are removed only when their current hash still
matches the lockfile. User-modified removals become conflicts. Updates and
removals use the same journaled transaction and rollback path as initial import.
