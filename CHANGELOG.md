# Changelog

## 2.3.0 — 2026-09-27

- Use one compact Ela toolbar, 28px catalog rows and version actions beside the content; default to a 720x420 window.
- Remove the duplicate title-bar margin and the default always-on-top state.
- Replace native file/folder dialogs with an Ela picker supporting path entry, drive navigation and multiple source files.
- Simplify the catalog toolbar and New form; show type/index filters and optional index fields on demand.
- Use consistent neutral light/dark palettes, readable secondary text and a single blue accent; fix mixed theme backgrounds.
- Add New IP/module with a generated HDL skeleton or in-place registration of existing sources.
- List explicitly registered definitions and references only; loose files are no longer catalog entries.
- Support multiple category paths, tags, interfaces and purposes per IP, with combined field search and index browsing.
- Add version-pinned references across catalogs and project directories without copying source/history content.
- Save new revisions as independent immutable JSON manifests with unique IDs and parent references.
- Share compressed SHA-256 content objects across files, versions and assets; unchanged saves are skipped.
- Add explicit Save revision for original source items, preserving their directory layout and source files.
- Keep a rebuildable SQLite catalog/search index in the local user cache, outside the synchronized library.
- Verify streamed history exports, preserve concurrent revisions and retain shared objects on deletion.
- Choose the library folder on first use; keep refresh and source-folder actions in the More menu.
  Distinguish working files from saved revisions.
- Keep scanning free of source content hashing and writes inside the selected library. Preserve saved assets,
  skip generated/internal directories, and protect original files from library mutations.
- Support explicit verified exports of current source contents without claiming
  they are immutable revisions.

## 2.2.1 — 2026-09-24

- Include Ela popup padding in combo height so the first and last revision rows
  remain fully visible. Repeated show requests keep the existing popup size.
- Import shared implementation patch 30 while retaining the p27 public ABI.
- Add regression coverage for 1, 3 and 5 rows, selection changes and repeated opening.

## 2.2.0 — 2026-09-24

- Use interruptible Ela combo and menu reveals, smooth ordinary list scrolling,
  precise touchpad input, and English editing menus that close with their owner.
- Restore separate horizontal/vertical splitter ratios and show real background
  activity, accessible status text, bounded tooltips, and scrollable library issues.
- Cache catalog search text, preserve unchanged selections, use lightweight list
  models, and keep only the latest pending legacy detail request.
- Preserve pending workspace/library changes while a transaction completes.
- Require the shared p27 Ela API, export capability/source provenance, and include
  replayable vendor patches and DLL checksums in the formal directory package.
- Fix focused ElaListView teardown by retaining its style through Qt cleanup.
- Guard overlay scrollbar origins and retain Qt text units for touchpad gestures.

## 2.1.0 — 2026-09-24

- Replace the working-copy workflow with explicit immutable revisions: Collect,
  Use, and Update. Existing schema 1 libraries retain read and migration support.
- Share one compact English Ela panel between standalone xIPs and ZeroSlack.
- Add a versioned native API, current-file collection, and pinned project provenance.
- Add the coral X and stacked-module icon to Windows, the library, and ZeroSlack.
- Join AppSuite with library/asset resources, UI actions, and a native surface
  with an external fallback. Runtime availability does not affect local library use.
- Package a clean Release build with source revision, release tag, and SHA-256 inventory.
