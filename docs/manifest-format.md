# Asset manifest

## Schema 2

.xips.json is UTF-8 JSON. An example (digest abbreviated for readability):

```json
{
  "schemaVersion": 2,
  "id": "stable-uuid",
  "name": "UART RX",
  "category": "module",
  "description": "UART receiver",
  "nextRevision": 3,
  "revisions": [
    {
      "id": "1",
      "note": "",
      "created": "2026-09-23T12:00:00.000Z",
      "hash": "sha256:<64 hex digits>",
      "files": ["uart_rx.sv"]
    },
    {
      "id": "2",
      "note": "Verified on board",
      "created": "2026-09-23T13:00:00.000Z",
      "hash": "sha256:<64 hex digits>",
      "files": ["uart_rx.sv"]
    }
  ]
}
```

Categories are module, ip, artifact, or other. Optional tags are retained for legacy compatibility and search.
Revision IDs are positive decimal strings; nextRevision is an integer greater than every retained revision.
Deleting the highest revision does not lower nextRevision. The highest retained ID is the default version.

Payload for a revision resides exclusively at **.xips/revisions/<id>/**.
File names are normalized relative paths. Traversal, links, duplicates differing only in case, and absolute paths are rejected.
The digest covers framed relative paths, byte lengths, and file contents in sorted order.
The exact implementation is SnapshotLibrary::digest; consumers should obtain hashes from the public API or CLI.

Metadata is published with QSaveFile. Library mutations acquire a cooperative lock.
New assets and new revisions are copied into staging directories, validated, then renamed into place.
Manifest bytes are compared again before replacing metadata. A detected conflict preserves published-but-unreferenced data.
This does not provide a distributed lock across cloud clients; operations refuse detected changes and retain uncertain data.

## Schema 1 compatibility

The legacy model has a mutable root payload and optional .xips/versions snapshots.
Those assets remain available through the compatibility reader.
Conversion imports saved versions in the legacy inventory order, retaining labels in notes and saved timestamps.
The working copy becomes the final revision unless its content matches the preceding snapshot.
Conversion publishes schema 2 at the original root and retains the entire schema 1 folder at .xips-legacy-<unique suffix>.
Stable asset identity is preserved.

## Recovery visibility

Hidden .xips-* sibling directories, unreferenced revision directories, and retained revision deletions are listed in Issues.
xIPs does not infer that they are disposable. Inspect or restore them explicitly outside the app.
