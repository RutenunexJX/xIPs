# xIPs 1.1 implementation status

## User workflow

Status: complete.

1. Explicitly choose an asset library on first launch.
2. Import files or folders by selection or mixed drag-and-drop; infer metadata, recover name collisions automatically, and offer one-step undo.
3. Start searches across all assets, show any intentional group scope, display the matched file path directly, select it in details, and expose a contextual one-click open action.
4. Open a single-file asset directly or open a directory asset in the file manager.
5. Replace an existing working copy from a file or directory only after previewing additions, replacements, and removals; preserve identity, metadata, and saved versions.
6. Save immutable versions only after content changes; show selected-asset change state and suggest the next patch version.
7. Copy payload from an explicitly named source version to an explicitly previewed final path without overwriting a destination.
8. Assign, rename, and remove user groups; edit an asset by checking existing groups or entering one new group.
9. Delete a saved version without touching the working copy, or delete the complete selected asset after a bounded confirmation without touching import sources.
10. Refresh on startup and window activation for a Jianguoyun-backed library; surface retained problems in a non-modal action banner.

## Interface boundary

Status: complete.

- Primary toolbar: Add, explicit Copy working copy/version, Search, visible search scope.
- Asset-specific actions: Open file/folder, Edit details, Save version.
- Secondary actions: choose library, refresh, update working copy, complete-asset deletion, problems, group management, saved-version deletion.
- Inventory: Groups, three-column asset list (Asset, Groups, Version), concise details, files, saved versions.
- Internal ID and filesystem path are not normal-workflow fields.

## Excluded scope

HDL parsing, source preview, dependency analysis, Git integration, simulation, synthesis, cloud accounts, installers, Windows URI registration, single-instance IPC, and ZeroSlack embedding are outside 1.1.

## Acceptance

Status: complete when the strict-warning Debug build, `core`, offscreen `gui_smoke`, and an independently executed offscreen `user_journey` all pass from a disposable build directory; source checks pass; build products are removed; and the reviewed changes are committed locally.
