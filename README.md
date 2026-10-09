# xIPs

xIPs is a small, local library for reusable FPGA assets: Module, IP, Project, Artifact, or Other.
It manages explicitly created Module/IP/Project definitions, working sources, immutable revisions and shared references.
The interface uses Ela controls and shares ZeroSlack's compact panel layout. All application text is English.

## Everyday workflow

1. Choose the catalog root once. Only explicitly created, registered or referenced entries appear; loose source files never become catalog entries automatically.
2. Click **New**, choose Module, IP or Project, and enter a name. This creates an empty working folder and catalog definition, with no HDL template or saved version. Empty entries persist across refreshes and restarts. Recreating a deleted working folder creates a new identity without old group links or versions; explicitly registering an existing source can recover its retained history. Registering an existing file/folder inside the root is optional and also creates no version automatically.
3. In **Working files**, use **Add files** or **Add folder**, or drag individual files into the file area. These actions copy files into the working folder without creating a version. Identical duplicates are skipped, and conflicting names are reported without overwriting existing files.
4. Check the files to include. A folder checkbox selects its descendants; **All** selects or clears every file. Browsing a row does not change its checkbox. Checks survive refreshes and version switches, and missing files are removed from the selection. Click **Create version** (**Ctrl+S**), review the included files and the added, modified or omitted files compared with the previous version, and optionally add a note. Omitting a file from a version keeps its working copy. Zero checked files cannot create a version. This explicit save creates **rev1**, followed by **rev2** and later versions; unchanged content creates no extra version unless joining parallel heads.
5. Use **Versions** to select a saved version, then **Copy to project** to copy exactly that version's files to a new destination. The copies are independently editable. Continue adding or editing working files and create another version without changing earlier versions.

Dragging a folder anywhere in the panel opens **Import folder**. Choose a new Module/IP/Project or an existing entry with a writable working folder, check the incoming files to include, and choose **Copy** or **Move**. Set **Version** before importing (for example, `v1.0.0`), or leave it empty for the automatic `revN` name. **Import and archive** imports only the checked files and immediately saves that exact selection with the chosen version name. Names must be unique within the entry; importing identical files under a new name creates a separate version. The working set contains the latest imported selection together with files that have not yet been archived. Working files displays this set only when files were added or modified since the latest archive; otherwise the file area stays blank. Missing archived files alone, including machine-local lock files omitted during synchronization, do not expose unchanged files. A comparison error leaves the file area blank with an error notice until a successful check. Earlier imported files remain available through Versions and are retained on disk. Add files / Add folder can make an older file part of the working set again. Folder names and nested paths are retained. Copy keeps originals; Move removes each checked source only after the working copy and saved version verify. Unchecked or locked source files remain, and retained sources are reported. Cancel makes no changes. File conflicts never overwrite existing content.

Renaming a registered directory entry in Edit asset details also renames its working folder. The saved identity, revision history, groups and pinned references are retained. A name conflict or filesystem error leaves the previous name and folder in place.

Dropdown lists use compact rows. Click the text, empty area or arrow to open or close the list.

**Working files** and **Versions** are separate tabs. The version table contains only
saved snapshots, with **Version** and **Status: Archived** plus a lock. Double-click a file,
press **Enter**, or use its open action to edit the original working file or open a
verified read-only copy of an archived file. Secondary actions remain visible as
outline icons with tooltips; errors and operation progress appear when needed.

Working-file folders start collapsed. Explicitly expanded folders, the selected file and scroll position are remembered per IP and
included in saved browser state. File additions, content changes and removals are checked in the background for the selected entry; editing outside the app updates the view automatically. Saving a clean version leaves the file area blank while retaining the original files. Parallel revision heads remain available for review and adoption. Saving a version or importing working files updates
that IP and its local search index without rescanning the whole catalog. External
edits and synchronized changes refresh automatically after a short pause. Returning
to the window checks for missed changes in the background. **Rescan** remains available
for an immediate full check.
Refreshing other IPs leaves the current details intact. Refreshing the current IP
keeps the selected archived version, file and scroll position while they remain available.

Group names use a stronger, theme-aware text color and weight than their members. The filter button beside **Groups** toggles **Hide empty groups**, including groups with no members matching the current search or filters. It only changes visibility, retains every group and membership, and is included in saved browser state.

Use **New group** beside **Groups** to create a group in the left tree. Drag an IP onto
a group to add membership while retaining its existing groups. Right-click an IP to
add it to one or more groups or remove its membership. Right-click a group to rename
or delete it. Empty groups persist, and group changes never move source files. Successful group changes update the tree without a persistent saved message; failures remain visible.
Selecting a group lists all its IPs/modules in the detail pane; click an entry to open it.
Selecting a group before **New IP** puts the new IP in that group.

Assign multiple categories, tags, interfaces and purposes to one definition with
**Edit details**. Browse with **All indexes**, combine type filters with search, or use
terms such as `category:Communication tag:serial interface:"AXI4 Lite"`. Quote a field
value or phrase containing spaces. Category paths support parent browsing, e.g.
Communication includes Communication/UART.

While focus is in the catalog, **Ctrl+F** focuses search, **Ctrl+N** creates an IP,
**Ctrl+Shift+N** creates a group, **Ctrl+S** saves a revision, and **F5** refreshes.
**F2** renames the selected group in the tree; **Esc** clears the focused search field.

Existing folders registered in place retain their relative file layout, including IP packages with
`component.xml`. Hidden files/directories, links and generated build directories are
excluded by their existing scan rules. Explicitly imported hidden and build files are
included and remain tracked after refresh and reopening, including older registrations.
Unselected hidden/build files remain excluded; internal catalog/Git paths and links
cannot be imported.
New empty workspaces can include ordinary files and build outputs explicitly added to them.
Refresh updates definitions and their file lists without hashing or copying source
content. Hashes are calculated when saving,
resolving or using a version. Vendor Tcl is not executed by the catalog.

Created definitions have a globally unique ID and a library-relative source location.
Moving the whole library preserves history. **Working files** shows the editable sources;
**Versions** shows saved snapshots. **Edit details** maintains the index values, and
**Source folder** opens the working directory.

**Reference** adds a reference to a saved revision in another
catalog or project directory. The reference contains the owner location, asset ID and
revision ID, with no source or history-object copy. New owner revisions do not advance
existing references. Referenced definitions are read-only through the receiving catalog;
their owner must remain available. Use **Copy to project** when an editable materialized copy is needed.

Each saved revision records a complete file set; for working sources, this is exactly
the checked set. Unchanged content is shared across revisions and assets. Editing
collected asset details creates no revision.
Project copies never update automatically. **Collect** remains available for creating
an asset directly from external files and saving its first revision; **New revision**
saves subsequent versions of collected assets. Existing collected assets, legacy
conversion and pinned references retain their workflows.

**Create version** reviews the exact checked files before saving. **Collect**,
**New revision** and new-format **Copy to project** also review files before writing.
Copying identifies whether the destination will be a new file or directory.
If included files or bytes change after review, the operation fails safely and asks
for another review. Collected-asset update reviews summarize file changes against the
latest saved revision. Parallel heads are shown explicitly:
**Adopt reviewed files** records all current heads as parents, with no text merge.
A head arriving after the review also requires a fresh review.

A damaged independent revision manifest is listed in **Issues** without hiding healthy
saved revisions. An empty, missing or unreadable registered source keeps its healthy
history available. Saving and metadata/deletion changes are blocked while revision
metadata is incomplete; restore that metadata and refresh before mutating history.
Content-object integrity is checked when a saved version is used.

**Unregister** hides the definition while retaining source files, history
and group memberships. Register the same source again to recover its identity and history.
**Remove local reference** retains the local record under `.xips/removed-references` and
leaves its owner untouched. An unavailable reference remains selectable for
**Relocate / change reference version**: choose the owning library, inspect its versions,
then explicitly choose a saved revision. The asset ID must match; later owner versions
never silently advance a reference. Malformed reference JSON is reported in Issues.

After a standalone export, **Save receipt** writes an optional
`xips.use/v1` JSON sidecar containing the asset ID, revision, content digest and
receipt-relative payload location. The proposed name is `<export destination>.xips-use.json`.
It records the most recent export in this session and never overwrites different content.
Current-file exports are marked `sourceImmutable: false`; this receipt records origin,
not the continued integrity of an editable project copy.

Background scans, reviews, saves and exports report their current phase and file bytes.
Search, selection and saved-version browsing remain available against the last loaded
catalog; mutations and drag/drop are disabled while work is active. **Cancel** is
cooperative and stops at the next file/block checkpoint before publication. Once
publication starts it is disabled. A cancelled save may leave unreferenced shared
objects, but no new revision or export destination. New-source creation becomes
non-cancellable before exposing its source folder. Legacy service I/O has coarser
checkpoints; legacy export becomes non-cancellable before copying. Legacy export does
not use the new file-review dialog. No global percentage or speed improvement is implied.

Folder selection, refresh, asset actions and reported issues have visible controls with tooltips.
In **Versions**, select an archived version and click **Delete version**. The confirmation
identifies the IP and version. You can also delete the last version: the IP, working files,
groups and metadata remain, and you can create another version from the checked files.
Double-click a version name, press **F2** while the version list is focused, or choose **Edit version** to rename an archived version (for example, `rev1` to `v1.0.0`). Names persist after refresh and reopening. Working files, saved contents, creation order and pinned references remain unchanged. Version names must be nonempty and distinct within the asset; referenced versions are edited in their owning library.

Deletion records hide removed versions after refresh or reopen; immutable history and shared
content are retained for synchronization. Later versions keep increasing their numbers.
References cannot delete versions in the owning catalog. Existing project copies stay unchanged.

## Files and revisions

Version metadata and content are synchronized with the library. SQLite is a disposable
search/version index in the local user's cache directory, outside the selected library.
Scanning rebuilds it; missing or unavailable caches fall back to the in-memory catalog.

```text
Library/
  rtl/uart.sv                    # original working file
  .xips/
    objects/ab/<hash-rest>.obj   # shared, compressed content
    assets/<source-key>/         # definition and history of registered sources
      .xips.json
      .xips/revisions/<uuid>.json
    references/<asset-id>.json   # references to versions in other libraries
    groups/<group-id>.json       # group name and asset ID memberships
  uart/
    .xips.json                  # optionally collected asset
    .xips/revisions/<uuid>.json
```

Revision UUIDs identify immutable JSON manifests; rev1/rev2 are display sequence numbers.
Concurrent saves can share a sequence number and remain distinct. A later save records
both parents, preserving the supplied current file set without merging source text.
Content objects use SHA-256 and bounded compressed blocks, including for large artifacts.
Every use verifies the selected content before publishing a copy. Missing objects during
cloud synchronization cause an explicit error until the content arrives.
Source files remain untouched. Existing destination files are never overwritten.

Module, IP and Project collections omit common generated folders, including build and ip_user_files.
Artifact collections retain generated output folders. All categories exclude .git, .xips and xIPs metadata.
The collection dialog identifies this policy before copying.

A single-file asset is exported to the specified **file path**. A multi-file asset is exported to the specified **new directory**.
Only payload is exported; internal manifests and revisions are not copied to the project.

Unrecognized staging directories, retained deletions, and orphaned revision payloads are reported by path.
Shared objects are retained after deletion; there is no automatic garbage collection,
object packing, or built-in cloud synchronization service.
External file/metadata changes are coalesced before refreshing affected IPs; new
definitions and groups trigger a catalog check. Updates wait while a modal editor
or catalog operation is active, and incomplete sync writes are retried before errors
are displayed. Search uses cached in-memory fields and preserves surviving tree nodes
as filters change. Use **Rescan** to force a full check.

## ZeroSlack

The standalone app and ZeroSlack load the same **BrowserPanel** through **xips-browser.dll**.
The panel uses the same local SQLite cache and runs without a separate process or HTTP service.

- The host manages docking, floating windows, and panel state.
- **Collect > Current file** asks ZeroSlack to save the current document before collection.
- **Use in project** requires a new path in the active workspace.
- ZeroSlack records the asset ID, exact revision, digest, and relative destination in **.zeroslack/xips-references.json**.
- The resulting project files are ordinary editable copies. Updating the library never changes them.
- If provenance cannot be saved, the panel reports the failure and the exported location.

Launch the new standalone xIPs once to register its component location, then open or reload the xIPs panel in ZeroSlack.
Alternatively, deploy the component DLLs together next to ZeroSlack, or set XIPS_BROWSER_LIBRARY to the full path of xips-browser.dll.
Both applications must use matching Qt, compiler and architecture. Deploy the
component's **xips-browser-impl.dll** and private **XipsEla.dll** beside
**xips-browser.dll**; the entry point loads these from its own directory and can coexist with
the host's own Ela runtime. The component initializes its own controls without
changing the host application identity or font. The host can call **setDarkTheme(bool)**
to synchronize the panel with its theme.
An incompatible component leaves a small fallback panel with **Open xIPs** and **Reload xIPs**.

## AppSuite

xIPs is an independent AppSuite application with a coral X and stacked-asset icon.
Its Windows icon, window title, library header, and ZeroSlack panel use the same mark.
The portable component lives at **AppSuite/Apps/xIPs/** and uses the shared
**Apps/Runtime/suite-runtime.exe** for optional discovery and routing.

When built with the SuiteApp SDK, startup registers **xips** as a provider for
**xips://show** and **xips://asset/<id>?revision=<revision-id-or-number>**. Suite actions open the
library or select an asset; **xips.library** declares the native panel and an
external fallback. Resource resolution returns cached metadata only, without
exposing private file paths or verifying payloads. File copying remains an explicit
**Use** operation. The library remains usable when the shared runtime is absent.

## Build

Use Qt **6.10.2**, its private Widgets headers, CMake 3.25 or newer, and C++20.
Qt SQL and its SQLite driver are required for the local index. Deploy `Qt6Sql.dll`
and `sqldrivers/qsqlite.dll` with the Windows application/native component.
The Windows embedding build is tested with MinGW **13.1**.
Ela is vendored from the ZeroSlack fork; its license and bundled font license are included.
AppSuite integration defaults to enabled when the installed SuiteApp SDK is found.
Pass **-DSuiteApp_DIR=E:/SuiteRuntime/install-release/lib/cmake/SuiteApp** to
build the AppSuite package, or **-DXIPS_ENABLE_SUITEAPP=OFF** for a standalone-only build.

```powershell
cmake -S . -B build/ela -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=E:/QT6/6.10.2/mingw_64 -DBUILD_TESTING=ON
cmake --build build/ela --parallel 6
ctest --test-dir build/ela --output-on-failure
```

Executables and the native component are written to **build/ela/bin**.
Qt and compiler runtime directories must be on PATH when running the build directly.

## Formal package

Build a clean tagged checkout with **CMAKE_BUILD_TYPE=Release**, **BUILD_TESTING=OFF**,
and the SuiteApp SDK enabled. For an explicitly standalone release, configure
**XIPS_ENABLE_SUITEAPP=OFF** and pass **-Standalone** to the packaging script.
The release tag **v2.13.2** must identify HEAD.
Then create a new staging directory:

```powershell
pwsh -File scripts/package-release.ps1 -BuildDirectory build/release -OutputDirectory build/packages/xIPs -Formal
```

The script includes Qt/Ela dependencies, the icon, notices, **build-info.json**,
**xips-capabilities.json**, replayable Ela patches, and **SHA256SUMS.txt**.
The package records its exact clean source commit and Ela source/DLL fingerprints.
The formal delivery is the runnable directory
**E:/PinloomRoot/AppPackage/AppSuite/Apps/xIPs/**. Each application has its own
directory under **Apps/**. Deploy the prepared directory there, preserve the other
applications, and update the suite manifest and checksum inventory.
Do not create ZIP archives. Build each release in a new staging directory under
**build/packages/**. Before replacement, preserve and verify a recoverable backup of the
old xIPs package and the affected suite indexes outside the formal directory. Replace
only managed xIPs files and its index entries; retain unknown files and other applications.
The package does not include or modify the user's library.

The repository owner authorized automatic delivery on 2026-10-08: after each completed
xIPs development round and its required checks, build and verify the formal package,
push the release commit on **origin/main** with its matching **v<version>** tag, and
replace the xIPs formal package above. This standing authorization applies only to xIPs
and may be overridden by the owner's instructions for a later round.
Machine-specific settings are retained in the ignored **release-profile.local.yaml**.
See [release notes](CHANGELOG.md).
See [Ela capabilities and validation](docs/ela-integration.md) for the shared ABI,
component boundaries, interaction regression, and catalog performance measurements.

See [Qt Creator](docs/qt-creator.md), [manifest format](docs/manifest-format.md), and [integration contract](docs/cli-and-integration.md).


## License and public-release status

Original application code is licensed under [Apache-2.0](LICENSE); see [NOTICE](NOTICE).
Third-party code, fonts and data retain their licenses in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
[Asset provenance](docs/ASSET-PROVENANCE.md) records the known sources and unresolved permissions.
[Public-release review](docs/PUBLIC-RELEASE-REVIEW.md) lists the checks still required before publication.
