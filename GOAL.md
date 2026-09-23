# xIPs product goal

A small local catalog of reusable Module, IP, and Artifact payloads.
The same English Ela panel serves standalone use and ZeroSlack embedding.

## Invariants

- One asset, one stable identity, one linear revision history.
- Collect creates rev1. Explicit Update captures the full source as the next revision.
- Identical payload creates no revision. Metadata edits create no revision.
- Revisions are immutable; there is no mutable library working copy.
- A project receives an ordinary editable copy pinned to the selected revision.
- Library updates never silently change a project.
- The filesystem and JSON manifest are authoritative; no database or service is required.
- New destinations never overwrite files, and library paths cannot be used as export destinations.
- File lists and digests are verified before use; links and unsafe paths are rejected.
- Original import sources are never modified.
- Revision numbers are monotonic and never reused after deletion.
- Legacy conversion is explicit and retains an inspectable original backup.
- Interrupted data remains discoverable and is not silently removed.
- An unavailable library is not recreated.
- ZeroSlack owns its workspace, editor state, destination validation, and provenance records.
- Shared native UI uses a versioned public Qt interface with ABI validation.
- Git, build tools, dependency graphs, cloud protocols, approvals, and project-wide lifecycle management stay outside the product.
