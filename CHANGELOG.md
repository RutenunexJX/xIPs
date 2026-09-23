# Changelog

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
