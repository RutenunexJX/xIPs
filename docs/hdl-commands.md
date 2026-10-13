# HDL commands and source packages

Choose an asset and open **HDL commands** in the browser toolbar. **New command**
creates a task or function in a new `xx_package.sv`, or binds an existing real
package symbol. Give it a searchable name, aliases, description and its package
source inputs in compile order. **Open source** uses the normal source-file
handler. Implement the behavior there, then **Save version** to validate and
archive source and mapping together. Invalid drafts retain their source and
diagnostics; the previous saved version stays usable. **Edit mapping** changes
search information or the source symbol, without inventing a second signature.

The ordinary asset file `xips-commands.json` has schema `xips.hdl-commands/v1`:

```json
{
  "schema": "xips.hdl-commands/v1",
  "origin": "user",
  "commands": [{
    "id": "3776a99f-41ea-4db6-8bd4-8c3f3a643a9c",
    "name": "Wait and return a value",
    "aliases": ["delay_value"],
    "description": "Wait 3 ns, then return 42.",
    "file": "user_package.sv",
    "package": "user_package",
    "symbol": "user_package::delay_value",
    "files": ["user_package.sv"]
  }]
}
```

The signature, kind, return type and parameters come from the actual source,
checked with Slang 10.0. Package-level subroutines and public static class methods
are supported. Generic class methods need a default specialization for catalog
inspection; the consuming editor resolves the actual signal type specialization.
Private, pure or DPI-only declarations are unavailable. A function containing a
delay is invalid. Metadata never executes a shell command.

Inputs are same-asset `.sv` files; `.svh` includes must also be in the archived
payload. Paths are relative to the asset. Missing/conflicting packages, dependency
cycles, incorrect compile order or escaping includes are errors. There is no
implicit cross-asset dependency resolver. The analysis limit is 256 compilation
inputs, 16 MiB per input and 30 seconds per input set; the UI remains responsive
and Cancel stops the helper. Saving or using revalidates the concrete source.

## Builtin behavior packages

Search exposes four builtin commands without writing the selected library.
**Add built-ins**, or the first explicit **Use**, imports them as an ordinary
versioned module asset. This does not replace existing user files. Updates to
application resources do not silently update previously imported copies.

Compile the packages in this order:

1. `tb_signal_package.sv`: typed adapter, cancellation and status.
2. `tb_reset_package.sv`: `reset_driver#(T)::run`.
3. `tb_sequence_package.sv`: `ramp_driver#(T)::run`.
4. `tb_random_package.sv`: `random_driver#(T)::run` and `window`.

Bind a concrete adapter in the real testbench scope. Its edge task observes the
live clock and its function directly assigns the live signal. It must not return
without waiting for an edge, add hidden workers or advance time in `drive`.

```systemverilog
module example;
  timeunit 1ns; timeprecision 1ps;
  logic clk = 0;
  logic signed [64:0] data;
  always #5ns clk = ~clk;
  class data_adapter extends tb_signal_package::signal_driver#(logic signed [64:0]);
    virtual task wait_edge(input bit rising);
      if (rising) @(posedge clk); else @(negedge clk);
    endtask
    virtual function void drive(input logic signed [64:0] value);
      data = value;
    endfunction
  endclass
  initial begin
    data_adapter target;
    tb_signal_package::status_t status;
    target = new;
    tb_sequence_package::ramp_driver#(logic signed [64:0])::run(
      target, 10, -65'sd5, 65'sd2, 1, null, status);
  end
endmodule
```

`T` must match a packed integral signal's width and signedness exactly; reset
requires one bit. The host must reject unknown/nonintegral/unpacked/enum signal
types instead of guessing a cast. The target needs one selected procedural driver.
Count zero is a no-op after argument validation; a null target, unknown values,
inverted random range or invalid window returns `TB_INVALID_ARGUMENT`. Reset
asserts immediately and releases after the specified real edges. Ramp wraps at
the declared width. Random uses typed constraints over an inclusive range and
does not reseed on each edge. `TB_RANDOM_FAILED` reports solver failure.

Window parameters are absolute picosecond ticks, for example `150ns/1ps` and
`200ns/1ps` even when the caller uses nanoseconds. The interval is half open:
start can drive, stop cannot. A separate deadline returns at stop even if the
clock stops. An already expired window returns `TB_EXPIRED`. Cancellation uses
a per-invocation `cancellation` object; calling `cancel()` returns `TB_CANCELLED`
and cannot terminate unrelated caller workers. Completion/cancellation keeps the
last value. Count-based calls can wait indefinitely without a clock or cancellation.

## Native consumption

The ABI remains `xips-browser/v1`; discover `hdl-commands-v1` via the existing
capability export. Match Qt/compiler/private-Ela ABI as before. The browser
exposes these QApplication-thread invokables:

```cpp
QVariantMap hdlCommandCatalog() const;
QVariantMap useHdlCommand(const QVariantMap &request);
void openHdlCommands();
```

`hdlCommandCatalogChanged(QString generation)` publishes a new catalog generation.
The catalog schema is `xips.hdl-catalog/v1`: `generation` (opaque string), `state`
(`loading`, `ready`, `unavailable`), `library`, `entries`, `errors`. Querying this
snapshot performs no filesystem or compiler work. Refresh uses the existing
library watcher in a background worker and caches validated source identities.
Each entry includes stable `commandId` and `mappingId`, name/aliases/description,
origin, assetId/assetName, revision/contentHash/sourceIdentity, sourceFile/package/
symbol, source-derived kind/returnType/parameters/typeParameters, compileOrder,
includeDirectories and availability/diagnostics. The latest saved revision and
a differing working draft are distinct entries. Disambiguate equal names by
asset and package identity. A draft is never an implicitly publishable version.

Use request: `{requestId,generation,commandId,revision}`. The immediate
`xips.hdl-use-result/v1` result is `accepted`, `busy`, `stale`, `not_found`,
`unavailable` or `invalid_request`. Accepted requests finish once while the panel
lives, via `hdlCommandUseFinished(QVariantMap)`: `exported`, `cancelled` or `error`,
with `requestId`, optional `code/message`, and a `receipt` only on success. Keep
the panel alive while `isCatalogBusy()`; destruction cancels and joins its own
workers. Context changes close review dialogs and cannot attribute an export to
a different workspace.

Use reuses **Copy to project**, destination validation and the existing host
`exportCompleted(QVariantMap)` callback. Its `xips.use/v1` receipt adds:

```json
"hdlCommand": {
  "schema": "xips.hdl-use/v1",
  "commandId": "asset-uuid/mapping-uuid",
  "mappingId": "mapping-uuid",
  "symbol": "user_package::delay_value",
  "kind": "task",
  "sourceFile": "user_package.sv",
  "compileOrder": ["user_package.sv"],
  "includeDirectories": ["."],
  "sourceIdentity": "sha256:..."
}
```

The host records that receipt, adds its relative compilation inputs under the
export path, and resolves actual source symbols before insertion. If host receipt
recording fails, files remain at the reported destination and completion is an
error. Exported source is fixed to the selected revision and has no dependency
on xIPs or its source checkout. Old components without the capability retain
normal browsing but cannot supply HDL commands.

Deploy `xips-hdl-check.exe` beside the component, together with its license
material. It statically links pinned Slang and its dependencies, dynamically
links the matching QtCore/compiler runtime, and uses a child-only environment
to locate shared runtime files beside the host. The asset directory is never its
working directory. No process-wide DLL search path is changed.

Build fetches pinned [Slang 10.0](https://github.com/MikePopoloski/slang/tree/ace09c5d7c9f4e28eed654d2f353c6dc792ebf67).
An offline checkout can be supplied with `FETCHCONTENT_SOURCE_DIR_SLANG` and the
usual FetchContent dependency source overrides. Run the `hdl_commands` and
`hdl_native` CTest targets in addition to the affected existing workflow tests.
