# Asset and history storage

## Working files and local index

Scanning lists explicitly registered definitions and references. Loose files are
excluded. It creates no files inside the library and does not hash or copy source
content. Registered source definitions include the mutable `current` selection.

SQLite stores the asset/search and revision index under
`QStandardPaths::GenericCacheLocation/xIPs/catalog/<library-path-hash>.sqlite`.
It stays outside the selected library, is rebuilt on Rescan and is never authoritative.
A corrupt cache is recreated; unavailable or busy caches fall back to the in-memory catalog.
Each connection belongs to its calling thread. Test builds accept `XIPS_TEST_CACHE_ROOT`.

## Schema 3 asset metadata

New collected assets have `<asset>/.xips.json`. Save revision on a scanned source
stores metadata at `.xips/assets/<source-key>/.xips.json`, with a library-relative
source location. The directory key identifies the source path; the metadata ID is a
separate globally unique UUID. Moving the whole library preserves its saved identity. Renaming
an individual source does not silently reassign its existing history.

```json
{
  "schemaVersion": 3,
  "id": "asset-uuid",
  "name": "UART",
  "category": "module",
  "source": { "path": "rtl/uart.sv", "directory": false },
  "indexes": {
    "category": ["Communication/UART", "Diagnostics"],
    "tag": ["serial"],
    "interface": ["AXI", "UART"],
    "purpose": ["telemetry"]
  }
}
```

`source` is present only for original-file histories. Optional description and tags
are retained. Categories are module, ip, artifact or other. New writes require
schema 3; this change does not migrate existing libraries.

Index values are many-to-many metadata, independent of physical directories.
SQLite stores normalized facet rows and rebuilds them from these definitions.
Field search combines category/tag/interface/purpose terms with AND; category paths
also match descendants. One asset remains one definition across every index view.

## Cross-library and project references

`.xips/references/<asset-id>.json` records schema `xips.reference/v1`, assetId,
revision (a saved UUID), library and definition. The latter two are paths relative
to the reference file's directory; definition points to the owning metadata folder.
References to references are flattened to the original owner. They copy no payload
or objects and pin the selected version. Both library roots and ordinary project
folders can contain the same reference format.

Reading reloads the owning definition and validates its identity and pinned revision.
Missing owners or versions are reported as catalog issues. Referenced entries cannot
edit, save or delete their owner through the receiving catalog. Existing reference
files are never silently replaced when adding another reference.

## Independent revision manifests

Each immutable revision is a separate UTF-8 JSON document at
`<asset-metadata-directory>/.xips/revisions/<revision-uuid>.json`:

```json
{
  "schemaVersion": 1,
  "assetId": "asset-uuid",
  "id": "revision-uuid",
  "sequence": 2,
  "parents": ["previous-revision-uuid"],
  "created": "2026-09-27T12:00:00.000Z",
  "note": "Verified on board",
  "hash": "sha256:<tree-digest>",
  "files": ["uart.sv"],
  "objects": {
    "uart.sv": {"hash": "<64-lowercase-hex-digits>", "size": 1234}
  }
}
```

Files are normalized relative paths in sorted order. Absolute paths, traversal,
links and duplicates differing only in case are rejected. Object hashes cover raw
file bytes. The tree digest frames paths, sizes and hashes; see `ContentStore::treeHash`.
Consumers should obtain hashes from the API/CLI.

Sequence numbers are display labels. Concurrent revisions retain unique UUIDs even
when their sequence numbers match. Numeric selectors reject ambiguity; project
receipts always pin the UUID. A later save records all known heads as parents and
captures the supplied source content; it does not merge source text.

## Catalog groups

The left catalog tree stores each group in `.xips/groups/<group-uuid>.json`:

```json
{
  "schema": "xips.group/v1",
  "id": "<group-uuid>",
  "name": "Bus",
  "members": ["<asset-id>"]
}
```

Membership refers to stable asset IDs. An IP can belong to multiple groups; empty
groups persist. Ungrouped IPs appear at the root. Group edits never move source
files or change revision history. Deleting a group removes only that group file.
Group updates use atomic file replacement and a cooperative local lock; refresh
loads synchronized changes from other machines.

## Shared content objects

Objects live at `.xips/objects/<first-two-hash-digits>/<remaining-digits>.obj`.
Identical bytes share one object across paths, revisions and assets. New versions
write only new objects and their manifest, leaving existing manifests and asset
metadata unchanged. Unchanged content with a single history head creates no revision.

The format starts with `XIPSOBJ1\n`, followed by blocks: a four-byte big-endian
compressed length, then a Qt `qCompress` payload for at most 1 MiB of raw data.
A zero length terminates the stream. Decoding bounds compressed and expanded sizes,
then checks total raw length and SHA-256. Large-file memory use stays bounded.

Publication uses temporary files and new destination names. A version is published
only after storing/verifying its objects and rechecking source files. Mutations
use a cooperative local library lock. This is not a cloud transaction: manifests
can arrive before objects. Resolution/export verifies every referenced object and
fails safely while synchronization is incomplete.

Export streams verified content into a temporary destination, then publishes the
result. Existing destinations and original files are never overwritten. Saved
history remains exportable after an original source is removed.

## Deletion and retention

Removing a schema 3 revision writes an immutable
`.xips/deleted-revisions/<revision-uuid>.json` containing assetId, revision and deleted
timestamp. The manifest and shared objects remain intact. Deleted sequence numbers
are not reused. Removing a collected asset removes its metadata tree; shared objects
remain available to other assets.

There is no automatic garbage collection or pack-file compaction. Orphaned objects
can remain after canceled saves or deletion; retaining them avoids invalidating
another revision or a client whose synchronization is incomplete.
