# `.xips.json` manifest format

Every asset directory contains a `.xips.json` JSON object. Paths are resolved
relative to that asset directory unless explicitly absolute. Relative paths are
preferred because the complete directory can move without invalidating the
asset.

Required fields in schema version 1:

| Field | Type | Meaning |
| --- | --- | --- |
| `schemaVersion` | integer | Must be `1`; version `0` can be migrated in memory. |
| `id` | string | Stable unique library ID using letters, digits, `_`, `-`, `.`. |
| `type` | string | `code-block`, `module`, or `ip`. |
| `name` | string | Human-readable name. |

Common optional fields include `description`, `version`, `top`, `language`,
`sources`, `includeDirs`, `defines`, `constraints`, `dependencies`, `tests`,
`testCommands`, `tools`, `tags`, `examples`, and `documentation`.

A dependency object contains `id`, an optional `version` constraint, and an
optional Boolean `optional`. String dependency IDs are accepted for compatibility
and normalized to objects when the manifest is rewritten.

Code Block fields are `template`, ordered `slots`, `scope`, `requiredSymbols`,
`exampleInput`, `exampleOutput`, and `allowWorkspaceOverride`. Example values
may be any JSON value, allowing either a rendered string or structured slot
bindings. A slot can be a string or an object with `name`, `type`, `default`,
`description`, and `required`.

A `testCommands` entry is an object containing `name`, `program`, string-array
`arguments`, and an optional `workingDirectory` relative to the asset root.
xIPs never treats the command as a shell string and requires confirmation before
execution.

Unknown fields are retained in the in-memory raw object and survive writes.
Writes use `QSaveFile` with direct-write fallback disabled, so an interrupted
replacement does not deliberately degrade to an in-place write.

Schema version 0 migration maps `kind` to `type`, `displayName` to `name`, and
`files` to `sources`. A missing `schemaVersion` or a future schema version is an
error rather than an implicit guess.
