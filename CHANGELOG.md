# Changelog

## 2.13.0 — 2026-10-08

- Start Working files with folders collapsed and preserve explicitly expanded folders in browser state.
- Leave the working-file area blank after all changes are archived; detect additions, modifications and removals in the selected entry in a cancellable background read, including external edits.
- Preserve the review/adoption workflow for parallel revision heads and keep archived file browsing stable during working-file checks.
- Distinguish group headings from members with theme-aware text color and weight, and add a Hide empty groups toggle that retains groups and memberships.


## 2.12.1 — 2026-10-08

- Keep Working files focused on the latest imported selection and unarchived file paths; retain earlier imported files on disk and in Versions, and preserve the working selection across refreshes and reopening.
- Read older per-version folder imports without changing their metadata; use Add files / Add folder to reactivate earlier files explicitly.
- Rename the working directory when editing a Module, IP or Project name, including entries renamed by earlier releases; retain stable history locations, group membership and pinned references.
- Reject invalid or occupied folder names and nested registered sources, and restore the original directory if metadata publication fails, including Windows case-only renames.


## 2.12.0 — 2026-10-08

- Set a version name while importing a dropped folder into a new or existing Module, IP or Project; leave it empty for automatic naming.
- Persist import names using the existing revision-label metadata, and create a separate version when identical content is imported with a new name.
- Reject invalid or duplicate names before publishing or removing sources; roll back imported files and labels if metadata or archival publication fails.

## 2.11.0 — 2026-10-08

- Edit archived version names with Edit version, a double-click on the version, or F2; keep custom names after refresh and reopening.
- Store version names separately from immutable archive manifests, preserving revision IDs, content hashes, creation order and pinned references.
- Reject empty, duplicate, invalid and stale version-name edits, and retain compact actions in narrow embedded panels.

## 2.10.0 — 2026-10-07

- Add Project as a distinct type alongside Module and IP in creation, editing, filtering and persisted definitions.
- Import dropped folders into a new or existing writable Module, IP or Project, with an exact file selection and a choice of copy or move; confirmation imports the selected files and immediately creates an archived version.
- Publish and verify the archive before removing moved sources, preserve unchecked or changed source files, reject conflicting destination content, and roll back newly imported files when archive creation fails.
- Make dropdown rows compact and allow a click on the text, blank area or arrow to expand or collapse the list.

## 2.9.1 — 2026-10-07

- Enable SuiteApp SDK 1.0.1 in the formal build while retaining independent operation when Runtime is absent.
- Share revision selection between the library and SDK provider; resolve unambiguous numeric aliases to stable IDs and reject ambiguous selectors.

## 2.9.0 — 2026-10-04

- Refresh affected IPs automatically after external edits or synchronized changes, coalescing file events and retrying incomplete writes without losing operation errors.
- Update individual catalog entries after imports and saved versions, and filter cached catalog data without rebuilding unchanged tree nodes.
- Preview added, modified and omitted files before creating an immutable version.
- Preserve each IP's working-file selection, collapsed folders and scroll position; keep the selected archived version and file in place during refresh.
- Monitor managed working folders, including hidden and build directories, without locking source directories against rename or replacement on Windows.

## 2.8.0 — 2026-10-03

- Add the native browser component with a private Ela runtime, host-controlled context, theme and lifetime, and usable narrow layouts.
- Preserve unchanged Working files model identity instead of rebuilding the tree, including checked files and selection.
- Keep standalone creation, file/folder import, version deletion and group workflows available without the SuiteApp SDK.

## 2.7.0 — 2026-10-03

- Delete any archived version from the visible Versions action, including the last, while retaining the IP, working files, groups, immutable history and shared content. Preserve revision numbering and ancestry through deletions, and reject stale targets or incomplete synchronized history.
- Accept copied file/folder drops throughout Working files, including the empty area and tree rows, using the same import checks as Add files / Add folder without automatically saving a version.
- Keep checked, unchecked and partial file checkboxes visible in both themes on native Windows/high-DPI displays, using Ela indicator colors with Qt item layout and input handling. Update the everyday workflow documentation for empty creation and checked-file versions.
- Create empty IP/module workspaces without templates or automatic revisions. Add files/folders or drop them into the workspace, check an exact file set, and explicitly create immutable versions; separate working files from saved history and preserve checks across refreshes.
- Show a selected group's IPs and modules in the detail pane, including when the tree group is collapsed; open an item with a click or Enter and keep search/filter results in sync.

## 2.6.0 — 2026-10-01

- Simplify the Ela browser with a tinted group sidebar, a two-column revision list, subtle file/revision selection, and visible outline actions. Put Copy to project beside the IP name; retain error/progress feedback while hiding repeated idle statistics.
- Show working and archived revisions together in an Ela table with a Status column; make edit/archive actions available on the working copy and open archived content read-only.
- Open files by double-click, Enter or the visible Open file button: current sources open in place; saved and referenced revisions open as verified read-only local copies.
- Replace the More menu with visible library controls and compact contextual asset/group actions, including source folders, references, editing and removal.

## 2.5.0 — 2026-09-30

- Keep healthy saved revisions accessible when another independent manifest is damaged or a registered working source is empty/unavailable; block unsafe history mutations and show the affected paths.
- Review file sets, byte counts, exclusions and file-level changes before collection, saving and new-format export; recheck reviewed content and destination shape before publication.
- Add source unregistration, retained local-reference removal, and explicit owner relocation/version selection with identity and stale-record checks.
- Offer standalone export-origin JSON receipts and shared GUI/CLI quoted field queries.
- Show parallel heads and require explicit adoption of reviewed files, with fresh review if the parent set changes.
- Report background phases and per-file bytes, allow browsing during work, and support cooperative cancellation before publication.
- Drag an IP onto a group to add membership, preserving other groups and source locations.
- Add catalog-scoped search, create, save, refresh and group-rename shortcuts, with hints in tooltips and menus.
- Clarify the compact toolbar labels: New IP, + Group, Export and Save revision.

## 2.4.0 — 2026-09-30

- Organize the left Ela tree with persistent groups: create, rename, delete, and add/remove IP memberships without moving source files; one IP can appear in multiple groups.

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
