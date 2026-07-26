# xIPs product goal

xIPs is a personal FPGA reusable-asset library. It minimizes the work required to collect code, find it again, keep a few meaningful snapshots, and copy the required files into a project.

## Invariants

- The only user-visible object is an asset. An asset may originate from a directory or a single file.
- The asset directory and minimal `.xips.json` manifest are the source of truth; no shared database is required.
- Import copies content and never modifies the source.
- Saved versions are immutable full snapshots. Deleting a saved version never deletes the working copy.
- Copy to exports payload only and never overwrites an existing target.
- Groups reuse manifest `tags` and do not change directory layout.
- Search includes real file names and relative paths.
- Git, HDL semantics, tool execution, dependency management, and cloud protocols are not product responsibilities.
- Jianguoyun support means operating safely on an ordinary synced folder and refreshing external changes, not implementing a cloud client.
- Future global activation and ZeroSlack embedding reuse stable IDs, the read-only CLI, and `xips://` requests without expanding the asset model.
