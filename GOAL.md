# xIPs product goal

xIPs is a personal FPGA reusable-asset library. It minimizes the work required to collect code, find it again, keep a few meaningful snapshots, and copy the required files into a project.

## Invariants

- The only user-visible object is an asset. An asset may originate from a directory or a single file.
- The asset directory and minimal `.xips.json` manifest are the source of truth; no shared database is required.
- Import copies content and never modifies the source.
- Import completion provides a one-step undo that removes only the newly created assets.
- Updating an asset replaces only its working payload after a visible difference preview; stable identity, metadata, and saved versions survive the update.
- Saved versions are immutable full snapshots. Deleting a saved version never deletes the working copy.
- Complete-asset deletion is bounded to a validated child of the selected library, includes every saved version, and never deletes the original import source.
- Copy to names its working/saved source and final destination, exports payload only, and never overwrites an existing target.
- Groups reuse manifest `tags` and do not change directory layout.
- A new search starts across all assets; an intentional group scope remains visible. Search includes and directly shows real file names and relative paths.
- Git, HDL semantics, tool execution, dependency management, and cloud protocols are not product responsibilities.
- Jianguoyun support means operating safely on an ordinary synced folder and refreshing external changes, not implementing a cloud client.
- Future global activation and ZeroSlack embedding reuse stable IDs, the read-only CLI, and `xips://` requests without expanding the asset model.
