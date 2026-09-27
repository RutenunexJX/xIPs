# xIPs

xIPs is a small, local library for reusable FPGA assets: Module, IP, Artifact, or Other.
It manages explicitly created IP/module definitions, working sources, immutable revisions and shared references.
The interface uses Ela controls and shares ZeroSlack's compact panel layout. All application text is English.

## Everyday workflow

1. Choose the catalog root once. Only explicitly created, registered or referenced entries appear; loose source files never become catalog entries automatically.
2. Click **New** to create an IP or module. Generate a SystemVerilog source folder, or register an existing file/folder inside the root. Registering a folder associates its files automatically and saves the first revision.
3. Assign multiple categories, tags, interfaces and purposes to one definition. Browse with **All indexes**, combine type filters with search, or use terms such as `category:Communication tag:serial interface:AXI`. Category paths support parent browsing, e.g. Communication includes Communication/UART.
4. Click **Save revision** on an original item to save its current content and an optional note. Unchanged content creates no extra revision. Choose **Current files** or a saved revision from the version list.
5. Click **Use** (or **Use in project** in ZeroSlack) to export the selected content to a new destination. **Collect** also saves external files as a new asset; **Update** saves subsequent revisions of collected assets.

Registered folders retain their relative file layout, including IP packages with
`component.xml`. Hidden files/directories, links and generated build directories are
excluded from registered working sources. Rescan refreshes definitions and their file
lists without hashing or copying source content. Hashes are calculated when saving,
resolving or using a version. Vendor Tcl is not executed by the catalog.

Created definitions have a globally unique ID and a library-relative source location.
Moving the whole library preserves history. **Current files** selects working sources;
saved revisions remain selectable. **More → Edit asset details** maintains the index
values, and **Open source folder** opens the working directory.

**More → Reference in library / project** adds a reference to a saved revision in another
catalog or project directory. The reference contains the owner location, asset ID and
revision ID, with no source or history-object copy. New owner revisions do not advance
existing references. Referenced definitions are read-only through the receiving catalog;
their owner must remain available. Use **Use** when an editable materialized copy is needed.

Each saved revision describes a complete file set, while unchanged content is shared
across revisions and assets. Editing collected asset details creates no revision.
Project copies never update automatically.

**More** contains folder selection, rescan, saved-asset editing/deletion, and reported issues.
Removing a new-format revision writes a deletion record; shared content remains available to other revisions.
The last revision can only be removed with the whole collected asset.
Deletion never renumbers subsequent revisions. Existing project copies stay unchanged.

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

Module and IP collections omit common generated folders, including build and ip_user_files.
Artifact collections retain generated output folders. All categories exclude .git, .xips and xIPs metadata.
The collection dialog identifies this policy before copying.

A single-file asset is exported to the specified **file path**. A multi-file asset is exported to the specified **new directory**.
Only payload is exported; internal manifests and revisions are not copied to the project.

Unrecognized staging directories, retained deletions, and orphaned revision payloads are reported by path.
Shared objects are retained after deletion; there is no automatic garbage collection,
object packing, or built-in cloud synchronization service.
Use Rescan after external or synchronized-folder changes.

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
Alternatively, deploy xips-browser.dll next to ZeroSlack, or set XIPS_BROWSER_LIBRARY to its full path.
Both applications must use matching Qt, compiler, architecture, and Ela builds.
An incompatible component leaves a small fallback panel with **Open xIPs** and **Reload xIPs**.

## AppSuite

xIPs is an independent AppSuite application with a coral X and stacked-asset icon.
Its Windows icon, window title, library header, and ZeroSlack panel use the same mark.
The portable component lives at **AppSuite/Apps/xIPs/** and uses the shared
**Apps/Runtime/suite-runtime.exe** for optional discovery and routing.

When built with the SuiteApp SDK, startup registers **xips** as a provider for
**xips://show** and **xips://asset/<id>?revision=<n>**. Suite actions open the
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
The release tag **v2.3.0** must identify HEAD.
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
Do not create ZIP archives or backups of the old formal package. **build/packages/xIPs** is only
the staging location. The package does not include or modify the user's library.
See [release notes](CHANGELOG.md).
See [Ela capabilities and validation](docs/ela-integration.md) for the shared ABI,
component boundaries, interaction regression, and catalog performance measurements.

See [Qt Creator](docs/qt-creator.md), [manifest format](docs/manifest-format.md), and [integration contract](docs/cli-and-integration.md).
