# `xips-lock.json` format

Vendor import writes `xips-lock.json` at the target project root. The lockfile is
generated data: source manifests and Git repositories remain authoritative.

Schema version 1 has this shape:

```json
{
  "schemaVersion": 1,
  "mode": "vendor",
  "assets": [
    {
      "id": "reset_gen",
      "version": "1.2.0",
      "commit": "abcdef123456",
      "contentHash": "sha256:...",
      "source": "../../library/reset_gen",
      "files": [
        {
          "path": "vendor/xips/reset_gen/rtl/reset_gen.sv",
          "hash": "sha256:..."
        }
      ]
    }
  ]
}
```

| Field | Meaning |
| --- | --- |
| `schemaVersion` | Lockfile schema; currently `1`. |
| `mode` | Always `vendor`. |
| `assets` | Resolved dependency closure in deterministic dependency-first order. |
| `id` | Stable asset ID. |
| `version` | Manifest semantic version, if declared. |
| `commit` | Git commit known when the plan was created, if available. |
| `contentHash` | Hash of the complete declared asset content. |
| `source` | Preferably relative provenance path from the target project. |
| `files` | Files owned by this Vendor import, sorted by destination path. |
| `files[].path` | Relative path below the target project. |
| `files[].hash` | SHA-256 of the installed source content. |

The per-file hashes define ownership. On a later plan, xIPs may overwrite a file
only when its current hash still equals the previously owned hash. A differing
hash is a conflict and blocks execution. An unchanged source/destination pair is
reported as `skip`.

Before using an existing lockfile, xIPs validates schema version and mode,
requires unique non-empty asset IDs, requires each `files` value to be an
array, and rejects empty hashes, duplicate ownership, absolute paths, or `..`
escapes. The lockfile hash is captured during planning and rechecked before
execution so a concurrent edit cannot be silently replaced.

The lockfile is staged and published in the same transaction as copied files.
On failure, prior files and the prior lockfile are restored. An interrupted
transaction is recorded below `.xips/transactions/` and is recovered before the
next Vendor execution. Destination, metadata, staging, backup, and recovery
paths must not traverse symbolic links or Windows junctions.
