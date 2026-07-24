# CLI and cross-application integration

`xips-cli` is a stable process/file boundary for automation and ZeroSlack
integration. It does not read another application's database.

Every non-help command writes one JSON object to standard output:

```json
{
  "schemaVersion": 1,
  "command": "catalog",
  "ok": true,
  "data": {},
  "errors": []
}
```

Exit code `0` means success, `2` means invalid CLI usage, `3` means a blocked
plan/catalog conflict, and `4` means execution or I/O failed. Use `--pretty` for
indented output. Paths in JSON are resolved absolute paths unless the underlying
manifest/lockfile contract specifies relative paths.

`--help` and `--version` are plain-text exceptions to the JSON envelope and
exit immediately. The desktop executable implements the same non-interactive
startup paths without opening its main window, which permits silent deployment
checks.

## Catalog and ZeroSlack open URI

```powershell
.\build\src\xips-cli.exe catalog `
  --library .\examples\library --pretty

.\build\src\xips-cli.exe open-uri `
  --library .\examples\library --asset reset_gen
```

`open-uri` returns a versioned `zeroslack://open?...` URI containing asset ID,
manifest, source, top, language, and content hash. The CLI prints but does not
launch the URI. The desktop **Integration > Open selected asset in ZeroSlack**
action launches it through the operating-system URI handler.

## Register a module supplied by ZeroSlack

```powershell
.\build\src\xips-cli.exe register-module `
  --asset-root C:\work\rtl\counter `
  --source C:\work\rtl\counter\counter.sv `
  --source C:\work\rtl\counter\counter_pkg.sv `
  --id counter --name Counter --top counter
```

This returns the proposed `.xips.json` without writing. Repeat with `--execute`
to create the manifest atomically. The asset root and sources must already
exist; sources are never moved or copied. A source inside the root is recorded
relatively. `--source` is repeatable. Use repeated `--include-dir`, `--define`, and
`--dependency id@constraint` options as needed. Prefix a dependency with `?` to
mark it optional. Supplying catalog roots also enables duplicate-ID rejection.
Execution revalidates the manifest path, asset root, top, and every source, and
rejects link-backed or stale plans before writing.

The desktop **File > Register SystemVerilog source in place** workflow accepts
one source file or a directory/Git checkout. It excludes Git/xIPs/build trees,
passes discovered `.sv/.v` files and `.svh/.vh` include directories to Slang,
then asks the user to select a reported top. Slang instance and package-import
facts are shown as dependency-unit candidates; uniquely matching catalog asset
IDs are suggestions that the user can edit or remove before reviewing the
manifest. No same-name source or unit is merged automatically. If the directory
is outside registered roots, it is added as an external root after the manifest
has been written. No source is copied or moved.

## Create a managed Module

```powershell
.\build\src\xips-cli.exe create-managed `
  --library C:\fpga\assets `
  --id counter --name Counter --top counter `
  --seed empty
```

`--seed` accepts `empty`, `source`, or `directory`. Source mode uses `--source`;
directory mode uses `--existing-directory`. The preview reports every generated
or copied file. `--execute` builds the asset below a private staging directory,
writes the manifest safely, and publishes the completed asset directory with
one rename. Cancellation or failure removes the staging tree. Git, xIPs,
build/cache, and generated-tool directories are excluded from directory seeds.
Symbolic links and Windows junctions are not traversed. Execution revalidates
that every destination remains confined to the managed library and that each
manifest source is present in the previewed file plan.
The desktop workflow additionally offers the currently selected Module as a
directory seed.

## Import into a project

```powershell
.\build\src\xips-cli.exe import `
  --library .\examples\library `
  --asset reset_gen `
  --target C:\work\project `
  --mode vendor `
  --target-tool vivado=2022.2 `
  --target-tool slang=6.0.0
```

Without `--execute`, `import` returns dependency order, issues, asset upgrades,
all file actions, and the proposed output document without mutation. Add
`--execute` to perform the already described Reference write or confirmed
Vendor transaction. Multiple `--asset` options are accepted.
`--target-tool name=version` is repeatable and enables semantic-version checks
against every tool constraint in the resolved dependency closure. If target
versions are omitted while assets declare tools, the plan contains an explicit
“compatibility unverified” warning.

The desktop **File > Inspect or repair Reference configuration** action reads a
project's `.xips/references.json`, shows valid, missing, hash-mismatched, and
repairable states, and can atomically rewrite paths by matching registered
asset ID and expected content hash. A repair is rejected if the configuration
changes after inspection.

## Code Block handoff

```powershell
.\build\src\xips-cli.exe code-block `
  --library .\examples\library `
  --asset always_ff_reset `
  --handoff C:\work\handoff\always_ff_reset.json
```

The response contains the ordered slot payload and a
`zeroslack://insert-code-block?...` URI. No file is written until `--execute` is
present. The handoff is written with safe replacement and contains protocol
version, operation, ID, version/hash, template, scope, ordered slots, required
symbols, example input/output, and workspace-override policy. The URI carries
only identity and the handoff path, avoiding oversized template data in the
URI.
