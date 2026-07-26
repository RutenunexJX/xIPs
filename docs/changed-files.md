# Source-controlled file inventory

构建目录 `build*`、Qt Creator 本机配置和部署产物均不属于源码清单。

## 项目与文档

- `CMakeLists.txt`
- `README.md`
- `PLAN.md`
- `GOAL.md`
- `docs/manifest-format.md`
- `docs/cli-and-integration.md`
- `docs/qt-creator.md`
- `docs/changed-files.md`

## 核心与应用

- `src/CMakeLists.txt`
- `src/assetcore/Asset.h`
- `src/assetcore/JsonUtil.h`
- `src/assetcore/JsonUtil.cpp`
- `src/manifest/ManifestService.h`
- `src/manifest/ManifestService.cpp`
- `src/library/AssetScanner.h`
- `src/library/AssetScanner.cpp`
- `src/library/AssetLibraryService.h`
- `src/library/AssetLibraryService.cpp`
- `src/library/FileSystemUtil.h`
- `src/library/FileSystemUtil.cpp`
- `src/integration/IntegrationService.h`
- `src/integration/IntegrationService.cpp`
- `src/app/AssetTableModel.h`
- `src/app/AssetTableModel.cpp`
- `src/app/LibraryController.h`
- `src/app/LibraryController.cpp`
- `src/app/MainWindow.h`
- `src/app/MainWindow.cpp`
- `src/app/main.cpp`
- `src/cli/main.cpp`

## 示例资产

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

## 自动测试

- `tests/CMakeLists.txt`
- `tests/tst_core.cpp`
- `tests/tst_gui_smoke.cpp`

旧的 AssetIndex、Slang、DependencyResolver、ImportService、GitService、DiffService、TestRunner、ManagedAssetService 及五阶段测试源码已删除。当前仅保留 IP 核心库、Qt Widgets 应用层、桌面程序和只读 CLI。
