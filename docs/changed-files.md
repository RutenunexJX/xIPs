# Changed files

The repository initially contained only `.gitattributes` and `.gitignore`.
`.gitattributes` was left unchanged. The following source-controlled artifacts
were created or updated for xIPs; generated files under `build/` are excluded.
The same exclusion applies to acceptance/deployment directories named
`build-*`.

## Project and documentation

- `.gitignore`
- `CMakeLists.txt`
- `README.md`
- `PLAN.md`
- `GOAL.md`
- `docs/manifest-format.md`
- `docs/lockfile-format.md`
- `docs/import-modes.md`
- `docs/versioning-and-tests.md`
- `docs/cli-and-integration.md`
- `docs/changed-files.md`

## Application and libraries

- `src/CMakeLists.txt`
- `src/app/main.cpp`
- `src/app/MainWindow.h`
- `src/app/MainWindow.cpp`
- `src/app/AssetTableModel.h`
- `src/app/AssetTableModel.cpp`
- `src/app/LibraryController.h`
- `src/app/LibraryController.cpp`
- `src/assetcore/Asset.h`
- `src/assetcore/Asset.cpp`
- `src/assetcore/JsonUtil.h`
- `src/assetcore/JsonUtil.cpp`
- `src/manifest/ManifestService.h`
- `src/manifest/ManifestService.cpp`
- `src/assetindex/AssetScanner.h`
- `src/assetindex/AssetScanner.cpp`
- `src/assetindex/AssetIndex.h`
- `src/assetindex/AssetIndex.cpp`
- `src/semantic/SlangService.h`
- `src/semantic/SlangService.cpp`
- `src/dependency/DependencyResolver.h`
- `src/dependency/DependencyResolver.cpp`
- `src/importer/ImportService.h`
- `src/importer/ImportService.cpp`
- `src/gitservice/GitService.h`
- `src/gitservice/GitService.cpp`
- `src/diff/DiffService.h`
- `src/diff/DiffService.cpp`
- `src/testrunner/TestRunner.h`
- `src/testrunner/TestRunner.cpp`
- `src/integration/IntegrationService.h`
- `src/integration/IntegrationService.cpp`
- `src/managed/ManagedAssetService.h`
- `src/managed/ManagedAssetService.cpp`
- `src/cli/main.cpp`

## Example asset library

- `examples/library/always_ff_reset/.xips.json`
- `examples/library/common_types/.xips.json`
- `examples/library/common_types/rtl/common_types_pkg.sv`
- `examples/library/reset_gen/.xips.json`
- `examples/library/reset_gen/rtl/reset_gen.sv`
- `examples/library/reset_gen/rtl/include/reset_config.svh`
- `examples/library/reset_gen/constraints/reset_gen.xdc`
- `examples/library/reset_gen/tb/reset_gen_tb.sv`
- `examples/library/reset_gen/README.md`
- `examples/library/vivado_clocking_ip/.xips.json`
- `examples/library/vivado_clocking_ip/ip/clk_wiz_0.xci`
- `examples/library/vivado_clocking_ip/scripts/recreate_ip.tcl`
- `examples/library/vivado_clocking_ip/constraints/clocking.xdc`

## Automated tests and fixtures

- `tests/CMakeLists.txt`
- `tests/tst_phase1.cpp`
- `tests/tst_phase2.cpp`
- `tests/tst_phase3.cpp`
- `tests/tst_phase4.cpp`
- `tests/tst_phase5.cpp`
- `tests/tst_gui_smoke.cpp`
- `tests/fake_slang.cpp`
- `tests/fake_test_tool.cpp`
