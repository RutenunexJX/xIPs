# CLI and native integration

## CLI

The CLI retains a schemaVersion 1 JSON **envelope**; this is separate from the schemaVersion 3 asset format.
Success is emitted on stdout, and errors on stderr. Exit codes are 0 success, 2 invalid arguments, 3 unavailable/invalid data, and 4 missing asset or revision.

```powershell
xips-cli --action list --library E:/Library --query uart
xips-cli --action resolve --library E:/Library --asset <id>
xips-cli --action resolve --library E:/Library --asset <id> --asset-version <revision-uuid> --destination E:/Project/uart.sv
```

For collected assets, resolve defaults to the latest retained revision and verifies its content objects.
Source items default to `current`; supply a saved revision UUID to resolve immutable history.
Decimal sequence selectors are accepted only when unambiguous. `resolvedVersion` always returns the exact saved UUID.
Without --destination it returns metadata, revision ID, digest, and relative file names, with access=metadata-only.
It never returns the private snapshot directory as an editable source path.
With --destination it creates a verified new copy and returns the exported path(s).
A one-file asset needs a final file path; a multi-file asset needs a new directory path.
The CLI never modifies the library. Listing refreshes the disposable local SQLite index.

Listing includes only explicitly created/registered definitions and pinned references.
Registered working sources default to version `current`. Resolve returns
`discovered: true` and `immutable: false`, with the current file digest; it still
returns metadata only unless `--destination` is supplied. An explicit exported
copy of current files reports `sourceImmutable: false`. Saved history of the same
source reports `immutable: true` for metadata or `sourceImmutable: true` for an export.
Scanning itself never hashes or copies
source contents. A `component.xml` package keeps its relative source layout.

List and resolve expose `indexes` and `reference`. List queries support combined
terms such as `category:Communication tag:serial interface:AXI`. Reference entries
resolve their pinned UUID in the owning library; listing a project directory also
loads its `.xips/references` catalog. Adding a reference never materializes source files.

GUI and CLI share quoted-query parsing. For example, pass the query string
`interface:"AXI4 Lite" tag:verified` to match both conditions, or `"clock domain"`
for a phrase. Inside quotes, backslash escapes the next character. Unclosed quotes
and empty field values report an error instead of running a partial query. Shell
quoting must preserve the inner double quotes in the argument delivered to the CLI.

Standalone users can save an optional `xips.use/v1` receipt from the More menu after
an export. It contains assetId, name, category, revision, contentHash, sourceImmutable,
files and a path relative to the receipt directory. It uses a new sidecar file and
does not require a host or alter the exported payload. The host callback remains
unchanged and continues to use its workspace-aware receipt path.

Schema 1 preserves its prior behavior: resolution without --asset-version addresses the working copy.
Saved legacy versions require explicit materialization before exposing paths.
New integrations should use schema 3 and exact resolvedVersion/contentHash values.

The GUI accepts --library, --open-asset, --search, and existing xips://asset and xips://search activation requests.
XIPS_LIBRARY overrides a saved library when no --library or explicit native library path is given.
A missing configured directory remains unavailable until changed or restored.

## Native surface v1

The public header is **include/xips/BrowserApi.h**.
Load the two C exports with QLibrary:

- xips_browser_abi_v1(): compares the Qt patch version, pointer size, and compiler ABI.
- xips_create_browser_v1(QWidget *parent, QObject *host): creates a browser widget owned by the parent.

The Windows build is Qt 6.10.2 / MinGW 13.1 / 64-bit. Deploy **xips-browser.dll**
with **xips-browser-impl.dll** and its matching **XipsEla.dll** together. The lightweight
v1 entry point locates its own module directory and loads the implementation with
Windows' altered search path for that load only. It does not alter process PATH or
global DLL search directories. The implementation retains a loader reference until
process exit, including after its last panel closes. This private runtime can coexist with
the host's **ElaWidgetTools.dll**; never replace the host DLL with xIPs' copy.
Call the factory on the QApplication thread. It initializes its own Ela controls,
preserves host identity/font/palette, and returns null if construction fails or
the call is made on another thread. The host must check for null before embedding.
The optional **xips_browser_last_error_v1()** export returns a UTF-8 diagnostic on
the calling thread; copy the string before the next factory call on that thread.
Missing implementation/dependencies can be redeployed and the factory retried.
Keep the component loaded until all widgets and background operations are gone. ZeroSlack uses PreventUnloadHint.

The returned QWidget exposes public Qt invokables:

| Method | Purpose |
| --- | --- |
| setContext(QString library, QString workspace) | Set explicit library and current project. Empty library retains the selected library; initial fallback is environment or xIPs settings. Empty workspace closes the project context. |
| collectPaths(QStringList) | Open the small collection form for selected sources. |
| importWorkingFiles(QStringList) | Add files/folders to the selected writable Working files area, preserving the existing import/conflict rules. |
| revealAsset(QString id) | Select a stable asset ID. |
| refresh() | Reload external changes. |
| isCatalogBusy() -> bool | Observe a pending catalog operation. |
| setDarkTheme(bool) | Set the private xIPs runtime theme; no host theme or settings changes. All xIPs panels in the process share it. |
| saveState() -> QVariantMap | Preserve library, checked files per asset, tab (including empty Versions), query, indexes, groups, selection and splitter ratios. |
| restoreState(QVariantMap) | Restore panel state. |

An optional host QObject implements:

| Method | Contract |
| --- | --- |
| collectionSources() -> QStringList | Return saved files from the current editor, or an empty list on cancellation. |
| destinationError(QString) -> QString | Empty permits the destination; otherwise show the reason and abort. |
| exportCompleted(QVariantMap) -> QString | Record provenance after copying; empty means success, otherwise display the returned error and exported location. |

The host bridge lives on the GUI thread. Copying to a project requires the
destination validator and receipt callback; losing the bridge never changes an
embedded panel into a standalone exporter. Catalog browsing and local working-file
operations do not require these callbacks. Theme synchronization uses the optional
setDarkTheme invokable; listen to the host's theme changes and forward the boolean.

Export receipts use schema **xips.use/v1**, with assetId, name, category, revision, contentHash, path, workspace, and relative files.
A callback failure does not remove an already created project copy.
For current source files, revision is `current` and contentHash identifies the bytes
actually copied. Selecting saved source history produces its immutable revision UUID.

## ZeroSlack host

XipsContextProvider registers the xips context resource.
It supplies workspace context, keeps document saving in TabManager, and validates workspace paths before export.
The host stores receipts in **.zeroslack/xips-references.json**, schema **zeroslack.xips-references/v1**,
with a workspace-relative path and the exact revision/digest. Writes are locked and atomic.
Unreadable or invalid existing provenance is preserved and reported.

Component lookup: XIPS_BROWSER_LIBRARY, application-local xips-browser.dll, adjacent xIPs package, then the location registered by standalone xIPs.
The fallback panel supports launching the standalone application and retrying component discovery.
No host code reads xIPs private revision directories.

## AppSuite provider

The native ABI string includes **;ela=454cac2d-p27** in addition to Qt 6.10.2,
pointer size, and compiler. The host must match this string. The component directly
imports p26 combo and p27 menu lifecycle APIs, so an older Ela DLL cannot satisfy
its imports. These imports now resolve against **XipsEla.dll**. The v1 ABI token
is retained for existing hosts; actual patch/source provenance remains in the
capabilities JSON. `xips_browser_capabilities_v1` is an optional `const char *(*)()` export
returning UTF-8 JSON with the capability list and normalized vendor source SHA-256.
The same metadata is packaged as **xips-capabilities.json**. **build-info.json** also
records the packaged Ela DLL SHA-256. Patch 28 changes style lifetime without an API
or class-layout change; its source is identified by the fingerprint and patch record.

State adds **library**, **workingChecks** (asset ID to relative file list),
**workingViews** (asset ID to collapsed folder paths, selected path, horizontal
and vertical scroll offsets), and
**page** (0 Working files, 1 Versions), alongside **horizontalRatio** and
**verticalRatio**. Old saved states remain accepted; an empty map is a no-op.
Call setContext with the current host workspace, then restoreState; the state
never restores a workspace path. Context and restored state queue together during
background work, with the latest request taking precedence. Open forms are
rejected on context changes. Deleting the parent cancels pending cancellable I/O
and disconnects completion handlers; keep the DLL loaded because workers may
still be reaching their next checkpoint. Modern revision
metadata is cached until Refresh; Use reloads and verifies the selected immutable
revision before materializing it. Legacy detail reads are serialized and coalesced.

The optional SuiteApp SDK publishes the **xips** application descriptor using
**suite-app/v1** and the shared per-user runtime. The standalone process owns
the provider; loading the native panel does not create another provider or service.
Runtime lookup follows the SDK's explicit override, environment, application,
sibling **../Runtime**, and PATH search. Missing runtime is non-fatal.

The embedded browser accepts a 280-pixel-wide host. Below 580 pixels the catalog
and details use the vertical splitter; below 360 pixels the working/version
actions stack. The Ela details area scrolls when height is limited. Resizing or
reparenting changes the layout without rescanning the catalog or replacing file
model indexes. Standalone windows retain their direct splitter layout.

| Contract | Behavior |
| --- | --- |
| `xips://show` | Current library metadata. |
| `xips://asset/<id>?revision=<n>` | Cached asset metadata and selected revision; omission selects the highest retained revision. |
| `xips.library.open` | Show the application. |
| `xips.asset.open` | Show the application and select the exact asset/revision. |
| `xips.library` | Native ABI v1 surface, with external application fallback. |

Resource metadata reports **contentVerified: false** alongside the recorded
digest. It is not permission to edit or a substitute for verification during Use.
Loading catalogs return **provider_busy**; missing assets and revisions return
structured errors. No suite action writes the library or materializes files.

The **AppSuite/Apps/xIPs/** component contains its own Qt/Ela dependencies,
**xips.exe**, **xips-cli.exe**, **xips-browser.dll**, **xips-browser-impl.dll**, **XipsEla.dll**, and
**assets/icons/xips-256.png**. The Windows executable embeds **xips.ico**.
