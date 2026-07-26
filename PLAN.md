# xIPs 1.0 implementation status

## User workflow

Status: complete.

1. Explicitly choose an asset library on first launch.
2. Import files or folders by selection or mixed drag-and-drop; infer metadata and recover name collisions automatically.
3. Find an asset by its user-facing metadata or any contained file name/path.
4. Open a single-file asset directly or open a directory asset in the file manager.
5. Save immutable versions only after content changes; show selected-asset change state and suggest the next patch version.
6. Copy only payload files from the working copy or a saved version without overwriting a destination.
7. Assign, rename, and remove user groups across one or more assets.
8. Refresh on startup and window activation for a Jianguoyun-backed library; retain actionable errors until viewed.

## Interface boundary

Status: complete.

- Primary toolbar: Add, Copy to, Search.
- Asset-specific actions: Open, Edit details, Save version.
- Secondary actions: choose library, refresh now, problems, group management, saved-version deletion.
- Inventory: Groups, three-column asset list, details, files, versions.
- Internal ID and filesystem path are not normal-workflow fields.

## Excluded scope

HDL parsing, source preview, dependency analysis, Git integration, simulation, synthesis, cloud accounts, installers, Windows URI registration, single-instance IPC, and ZeroSlack embedding are outside 1.0.

## Acceptance

Status: complete when the strict-warning Debug build, `core`, offscreen `gui_smoke`, and offscreen `user_journey` all pass from a disposable build directory and that directory is removed afterward.
