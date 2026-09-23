# xIPs implementation

## 2.2 Ela capability integration

Implemented in the existing compact panel: interruptible combos and menus,
English edit-menu lifecycle, precision/smooth ordinary scrolling, splitter ratio
restoration, real busy feedback, bounded tooltips, and cached catalog filtering.
Modern metadata selection performs no filesystem work; legacy reads retain one
active request and the latest pending selection. Snapshot transactions are unchanged.
Shared vendor patches 25–27 and the focused-list style lifetime fix 28 are replayable.
See [capability and validation record](docs/ela-integration.md).

## Delivered scope

- English Ela browser with search, category filters, asset details, revision selection, Collect, Use, and Update.
- Responsive side-by-side layout in a standalone window and vertical layout in a narrow host panel.
- Immutable full snapshots, content deduplication on update, metadata-only editing, and bounded deletion.
- Schema 1 read compatibility and explicit conversion with preserved originals.
- Metadata queries and explicit payload materialization through the existing JSON CLI envelope.
- Public native browser API and ZeroSlack context provider.
- Saved-current-file collection, workspace destination validation, and pinned project provenance.
- AppSuite provider, native surface descriptor, and application icon.
- Reproducible formal packaging with clean-source and release-tag checks.

## Validation

- Existing legacy core and CLI regression tests remain.
- Snapshot tests cover version creation, unchanged updates, metadata changes, pinned copies, artifact outputs, tampering, conflicts, deletion numbering, and migration.
- GUI and journey tests cover filtering, revision selection, unavailable paths, collection and project use.
- A native DLL test loads the exported ABI and invokes the public Qt surface.
- ZeroSlack integration tests cover workspace boundaries, preservation of invalid provenance data, and native collection/use when XIPS_BROWSER_LIBRARY is supplied.
- Visual checks use offscreen Qt renders at wide and narrow panel sizes.

## Explicit limits

There are no branches, automatic updates, background synchronization, previews that expose library payloads to editors, or automated recovery cleanup.
Large revisions are full snapshots and may consume significant disk space.
Native embedding requires coordinated Qt/compiler/Ela builds. Standalone use remains available if that requirement is not met.
