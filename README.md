# xIPs 2.1

xIPs is a small, local library for reusable FPGA assets: Module, IP, Artifact, or Other.
It collects files, keeps explicit immutable revisions, and copies a selected revision into a project.
The interface uses Ela controls and shares ZeroSlack's compact panel layout. All application text is English.

## Everyday workflow

1. Choose a library folder from **More > Choose library**. No hidden default library is created.
2. Click **Collect**, select files or a folder, confirm the name and category. Dropping local files or folders is also supported. The first collection creates **rev1**.
3. Search names, descriptions, tags retained from older libraries, or files. Category buttons narrow the results.
4. Select a revision and click **Use**. In ZeroSlack, this becomes **Use in project**. Confirm the complete new destination path.
5. Click **Update** and select the complete current source to create **rev2**, **rev3**, and so on. An optional note describes the change.

Updating captures a complete snapshot, not an incremental patch. Identical content creates no revision.
Editing a name, category, or description creates no revision. There are no branches, merges, working copies, dependency resolution, or automatic project updates.

**More** contains metadata editing, refresh, deletion, and reported issues.
Deleting a revision moves it to the Recycle Bin; the last revision can only be removed with the whole asset.
Deletion never renumbers subsequent revisions. Existing project copies stay unchanged.

## Files and revisions

Each asset has a stable ID and a small JSON manifest. Payload exists only under its revisions:

```text
Library/
  uart/
    .xips.json
    .xips/revisions/
      1/uart.sv
      2/uart.sv
```

The highest retained revision is the default selection. There is no duplicate mutable "latest" payload.
Every use verifies the recorded file list and SHA-256 digest before publishing a copy.
Source files remain untouched. Existing destination files are never overwritten.

Module and IP collections omit common generated folders, including build and ip_user_files.
Artifact collections retain generated output folders. All categories exclude .git, .xips and xIPs metadata.
The collection dialog identifies this policy before copying.

A single-file asset is exported to the specified **file path**. A multi-file asset is exported to the specified **new directory**.
Only payload is exported; internal manifests and revisions are not copied to the project.

## Existing libraries

Schema 1 assets remain visible and can be used without conversion.
**Convert legacy asset** imports their healthy saved versions, followed by a distinct working copy, into the linear revision model.
Original version labels are retained in revision notes. Saved timestamps are preserved.
The entire original asset is retained in a sibling **.xips-legacy-...** backup directory; its path remains listed in Issues.
Conversion is explicit, and refuses unhealthy legacy version records.

Unrecognized staging directories, retained deletions, and orphaned revision payloads are reported by path.
They are preserved for inspection. There is no background cleanup or cloud synchronization service.
Use Refresh after external or synchronized-folder changes.

## ZeroSlack

The standalone app and ZeroSlack load the same **BrowserPanel** through **xips-browser.dll**.
There is no embedded process, database, or local HTTP service.

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
and the SuiteApp SDK enabled. The release tag **v2.2.1** must identify HEAD.
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
