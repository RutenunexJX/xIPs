# Structured xIPs CLI operations

This extends the 2.14.2 ordinary-asset product. The existing `--action list` and `--action resolve` interface, streams, selectors and explicit `--destination` materialization remain compatible. HDL commands, templates and slot configuration are not xIPs responsibilities.

## Discover and call

```powershell
xips-cli --help
xips-cli --action capabilities
xips-cli --request request.json
xips-cli --request change.json --dry-run | Set-Content -Encoding utf8 preview.json
if ($LASTEXITCODE -ne 0) { throw 'Preview failed' }
xips-cli --apply preview.json
```

New requests are UTF-8 JSON objects, with an optional UTF-8 BOM. Request limit: 1 MiB; saved preview limit: 64 MiB. Relative filesystem paths are resolved against the request JSON file's directory, then bound as absolute paths in the preview. `files` contains relative payload names, never arbitrary filesystem paths. A request accepts only the fields belonging to its action. Do not mix structured requests with the old `--library`, `--asset`, `--query`, `--destination` or `--action` flags.

A read runs directly. Every structured mutation requires a reviewed preview followed by `--apply`; `--request` by itself never performs a mutation. Applying accepts either the complete preview response or its `data.plan` object. Plans include the normalized request, current library generations, immutable heads, selected file hashes and a content token. Edits, source changes and destination conflicts invalidate the preview. Two processes applying one creation preview cannot publish two assets. An unchanged save does not create a new version.

Example `create.json` (first create the `library` directory):

```json
{"action":"create","library":"./library","name":"uart","category":"ip"}
```

The result contains `data.asset.id` and `workingRoot`. Edit ordinary files in that working folder, then save using the stable ID:

```json
{"action":"save","library":"./library","asset":"ASSET_UUID","note":"Initial implementation"}
```

Save again after editing to create a second immutable revision. `history` lists only saved versions; `working` reports the separate working file set and added/modified/removed content. `workingRoot` is not the immutable revision's location.

```json
{"action":"history","library":"./library","asset":"ASSET_UUID"}
```

Use the exact returned revision UUID for reproducible export:

```json
{"action":"export","library":"./library","asset":"ASSET_UUID","revision":"REVISION_UUID","destination":"./exported-copy"}
```

The destination must not exist and must have an existing parent outside the library. Existing single-file export behavior is preserved: one payload file becomes the destination **file**; multiple files become a destination **directory**. The preview reports `detail.destinationKind`. No destination is overwritten.

## Ordinary catalog operations

All catalog requests require `action` and `library`. Most asset actions additionally require `asset`. Optional strings include `note` and `description`; `sources` and `files` are arrays of strings. `move` and `included` are booleans.

| Action | Inputs and behavior |
| --- | --- |
| `list` | Optional `category` (asset kind) and `query`; uses `CatalogIndex` exactly as the GUI. Quoted terms, `interface:`, `tag:`, `purpose:` and hierarchical `category:` indexes work. |
| `history`, `working` | Saved revisions/deleted revision IDs, or current working-copy differences. Archived-only assets have `working: null`. |
| `verify` | `asset`, `revision`; verifies saved content through the owning service. |
| `create` | `name`, `category` (`module`, `ip`, `project`). Omit `source` for an empty managed working folder; set `source` to register an existing file/folder inside the library. Optional `indexes`, `description`. |
| `create` with import | Set `sources`; optionally `files`, `move`, `note`, `version`. The selected files are imported and immediately archived. Cannot also use `source`. |
| `collect` | `name`, `sources`, optional `category`/`note`; creates an archived-only asset. |
| `import` | Existing working asset, `sources`, optional `files`/`move`/`note`/`version`; imports and immediately saves a version. A move removes only successfully published, unchanged selected input files. Retained inputs are reported. |
| `save` | Working asset: optional selected `files`. Archived-only asset: required `sources`. Optional `note`. The expected preview is enforced by the original save service. |
| `metadata` | Optional `name`, `category`, `description`, `indexes`; unspecified fields are preserved. `indexes` is an object of string arrays with keys `category`, `tag`, `interface`, `purpose`. Existing managed-folder rename behavior is reused. |
| `version-rename` | `revision`, `name`; changes the display label, never the revision ID/content. |
| `delete-version`, `restore-version` | Soft deletion preserves manifests, blobs and ancestry. Recovery requires the exact deleted UUID and verifies retained content before restoring visibility. |
| `unregister`, `relocate` | Retain source/history; relocation requires `source`. Re-register an unregistered source with `create` to recover its registration. |
| `delete-asset` | Existing archived-only asset deletion via the OS recycle bin. Working sources use `unregister`. Whole-asset recovery is performed in the OS recycle bin; CLI does not claim to recover permanently removed payloads. |
| `migrate` | Reuses the existing legacy-to-current asset conversion service. |
| `groups` | Lists stable group IDs and memberships. |
| `group-create`, `group-rename`, `group-delete` | `name` for create/rename; `group` for rename/delete. Delete removes the group, not asset contents. |
| `group-member`, `group-move` | `group`, `asset`, and respectively `included` or `targetGroup`. |
| `references` | Lists local pinned references, including broken references with diagnostics. |
| `reference-add` | `asset`, `revision`, `destinationLibrary`; pins the exact verified revision in the destination project/library. |
| `reference-verify` | `asset`, `ownerLibrary`, optional `revision` (defaults to the pin); verifies identity, location and content. |
| `reference-update` | `asset`, `ownerLibrary`, `revision`; updates a pin or repairs the owner location after a library move. |
| `reference-remove` | Removes only the local reference; owning content remains. |
| `receipt-create` | `asset`, `revision`, `payload`, `receipt`; verifies the existing exported file/directory before recording its origin. Receipt is outside the library and payload. |
| `receipt-verify` | `receipt`; validates the owner revision, recorded hash/file list and current exported bytes. |
| `receipt-recover` | `receipt`, new `destination`; verifies retained owner content and restores a separate copy. An edited/missing old export is not overwritten. Create a new receipt separately if required. |

Revision UUIDs take precedence. Existing unique decimal sequence selectors remain supported. Display labels such as `1.1` are not stable identifiers and are not used to infer a revision.

## Existing GUI sessions

The SuiteApp SDK is optional. Offline catalog/archive operations build without it. When available, per-window sessions reuse its public JSON transport; the shared Runtime and SDK are unchanged. Standalone and native BrowserApi v1 panels register their own session IDs.

```powershell
xips-cli --action sessions
xips-cli --action session-status --session SESSION_UUID
xips-cli --apply preview.json --session SESSION_UUID --wait-ms 10000
xips-cli --apply preview.json --session SESSION_UUID --focus-asset ASSET_UUID --focus-revision REVISION_UUID
```

A library open in a discovered live GUI requires an explicit target session. Matching windows are reserved before disk mutation; any active modal/edit dialog, busy catalog/archiving operation, outdated view, or live but unresponsive session rejects the write. No window is selected arbitrarily. A crashed CLI process releases its leases through owner-process monitoring. Stale process IDs are distinguished by process-start identity on Windows.

The result distinguishes `disk: committed`, `gui.adoption.state`, `gui.viewToken`, `gui.selection` and, when requested, `gui.focus.state`. A successful refresh request is not treated as adoption. `adopted` requires the visible catalog to match the committed generation after loading completes. `pending`, `conflict`, `unknown` and failed focus remain observable. The selected view is preserved unless explicit focus was requested; other sessions are refreshed without changing their selection. Their initial refresh result appears in `otherSessions` and can be queried again by ID.

Only the target session is awaited. `--wait-ms` accepts 0–60000. Externally edited buffers that have not been saved to disk are not visible to xIPs; xIPs protects its own active dialogs and operations, not another editor's in-memory document.

Isolated standalone configuration is public and useful for tests or separate profiles:

```powershell
xips --library ./library --profile ./profile --session-dir ./sessions --no-runtime
xips-cli --action sessions --session-dir ./sessions
```

`--profile` redirects QSettings to INI files; `--session-dir` selects the local session registry. `--no-runtime` omits global AppSuite broker registration while retaining explicit per-window transport.

## Archive Project

`archive-inspect` takes `project`; `archive-scan` takes `root`; `archive-installations` accepts optional `roots`. They reuse existing discovery and inspection services.

```json
{
  "action":"archive-run",
  "project":"./project/demo.xpr",
  "output":"./archives",
  "name":"demo-verified",
  "vivado":"C:/Xilinx/Vivado/2022.2/bin/vivado.bat",
  "sharedRoots":["./shared"]
}
```

Use the same preview/apply commands. `name` is explicit so the preview describes the final folder and `.7z` target. The exact XPR Vivado release is required; compression uses bundled 7-Zip. These are independent archive jobs, not catalog mutations, so omit `--session`.

The preview inventories and fingerprints all project inputs and dedicated shared roots. Declared external HDL files, Vivado `VerilogDir`/include directories and compatible repository directories are copied beneath `.srcs/_archive_shared`; shared layout is preserved. Explicit `sharedRoots` can contain sibling headers/data needed by declared files. Input changes invalidate the preview and changes during copying abort before Vivado or publication. Vivado runs only on the independent copy with an isolated profile; source contents and timestamps are not intentionally modified.

Existing safety limits remain: links/reparse points, unsupported path lists/variables, active custom Tcl hooks, and nonportable IP/BD metadata are rejected. Literal HDL include/memory-file paths that escape their isolated root or contain absolute paths are rejected instead of silently producing an incomplete archive. Dependencies chosen dynamically by macros/runtime code must already be self-contained; this is not a general HDL dependency compiler. Failed/cancelled diagnostic workspaces are retained and reported. Folder and `.7z` publication uses the original two-rename rollback scheme, not a crash-atomic multi-file transaction.

## Cancellation, errors and verification

`--cancel-file path` cooperatively cancels when the file appears. Catalog cancellation is accepted before publication; once publication wins, its committed result is returned. Archive cancellation interrupts its own contained child job and retains diagnostics. Progress/log events are JSON lines on stderr; the final structured result is one JSON object on stdout. Old CLI streams remain unchanged.

| Exit | Meaning |
| --- | --- |
| 0 | Requested operation completed, with adopted/focused target GUI when requested. |
| 2 | Invalid input, unknown field, malformed/altered plan or missing preview. |
| 3 | Conflict, busy/unavailable live GUI, verification failure or operation failure. |
| 4 | Missing asset/version/session or unavailable session transport. |
| 5 | Accepted cancellation. |
| 6 | Disk committed; requested GUI adoption or focus is pending/unavailable/conflicted. Query the session; do not blindly replay the write. |

Errors include a stable `error.code`, message and details. A successful publication followed by transport failure includes the committed disk result, with GUI state unknown. Existing service partial-failure details (`retainedPath`, `retainedSources`, revision identity) are preserved. Cancellation can leave unreferenced immutable objects; archive failures can leave diagnostics. No whole-library rollback or crash-replay guarantee is claimed.

Runnable validation: `tests/cli_workflow.py --cli PATH --work NEW_DIRECTORY [--gui PATH]` uses only public CLI requests and ordinary source-file edits. GUI mode launches isolated real standalone windows. `tests/archive_cli_workflow.py` accepts a fresh real Vivado fixture, launcher, CLI and new evidence directory; it checks archive contents, unchanged source hashes/timestamps, stale previews and cancellation after the real tool banner. Tests never belong in a formal package.
