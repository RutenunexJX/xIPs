# CLI and native integration

## CLI

The CLI retains a schemaVersion 1 JSON **envelope**; this is separate from the schemaVersion 2 asset format.
Success is emitted on stdout, and errors on stderr. Exit codes are 0 success, 2 invalid arguments, 3 unavailable/invalid data, and 4 missing asset or revision.

```powershell
xips-cli --action list --library E:/Library --query uart
xips-cli --action resolve --library E:/Library --asset <id>
xips-cli --action resolve --library E:/Library --asset <id> --asset-version 1 --destination E:/Project/uart.sv
```

For schema 2, resolve defaults to the highest retained revision and verifies its payload.
Without --destination it returns metadata, revision ID, digest, and relative file names, with access=metadata-only.
It never returns the private snapshot directory as an editable source path.
With --destination it creates a verified new copy and returns the exported path(s).
A one-file asset needs a final file path; a multi-file asset needs a new directory path.
The CLI never modifies the library.

Schema 1 preserves its prior behavior: resolution without --asset-version addresses the working copy.
Saved legacy versions require explicit materialization before exposing paths.
New integrations should use schema 2 and exact resolvedVersion/contentHash values.

The GUI accepts --library, --open-asset, --search, and existing xips://asset and xips://search activation requests.
XIPS_LIBRARY overrides a saved library when no --library or explicit native library path is given.
A missing configured directory remains unavailable until changed or restored.

## Native surface v1

The public header is **include/xips/BrowserApi.h**.
Load the two C exports with QLibrary:

- xips_browser_abi_v1(): compares the Qt patch version, pointer size, and compiler ABI.
- xips_create_browser_v1(QWidget *parent, QObject *host): creates a browser widget owned by the parent.

Use matching Ela builds as well; the Windows build is Qt 6.10.2 / MinGW 13.1 / 64-bit.
The host must initialize its Ela theme before creating the panel.
Keep the component loaded until all widgets and background operations are gone. ZeroSlack uses PreventUnloadHint.

The returned QWidget exposes public Qt invokables:

| Method | Purpose |
| --- | --- |
| setContext(QString library, QString workspace) | Set explicit library and current project. Empty library uses environment or xIPs settings. |
| collectPaths(QStringList) | Open the small collection form for selected sources. |
| revealAsset(QString id) | Select a stable asset ID. |
| refresh() | Reload external changes. |
| saveState() -> QVariantMap | Preserve query, category, selected asset, and selected revision. |
| restoreState(QVariantMap) | Restore panel state. |

An optional host QObject implements:

| Method | Contract |
| --- | --- |
| collectionSources() -> QStringList | Return saved files from the current editor, or an empty list on cancellation. |
| destinationError(QString) -> QString | Empty permits the destination; otherwise show the reason and abort. |
| exportCompleted(QVariantMap) -> QString | Record provenance after copying; empty means success, otherwise display the returned error and exported location. |

Export receipts use schema **xips.use/v1**, with assetId, name, category, revision, contentHash, path, workspace, and relative files.
A callback failure does not remove an already created project copy.

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

The optional SuiteApp SDK publishes the **xips** application descriptor using
**suite-app/v1** and the shared per-user runtime. The standalone process owns
the provider; loading the native panel does not create another provider or service.
Runtime lookup follows the SDK's explicit override, environment, application,
sibling **../Runtime**, and PATH search. Missing runtime is non-fatal.

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
**xips.exe**, **xips-cli.exe**, **xips-browser.dll**, and
**assets/icons/xips-256.png**. The Windows executable embeds **xips.ico**.
