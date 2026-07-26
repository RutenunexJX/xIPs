# xIPs

xIPs 是本地优先的个人 FPGA 可复用资产管理器。它统一管理完整 IP 目录和单个 `.v`、`.sv`、`.svh` 等文件，只负责收集、查找、保存版本和导出，不解析 HDL，不运行仿真或综合，不管理依赖，也不替代 ZeroSlack、Vivado 或 Git。

## 使用流程

1. **Library...** 选择本地或坚果云同步目录。
2. **Add folder** 导入完整 IP 目录；**Add file** 直接导入单个文件。两者都会复制到资产库并生成最小 `.xips.json`。
3. 使用搜索框按名称、ID、版本、分组或说明查找资产。
4. 从左侧 **Groups** 选择用户分组；分组与搜索条件可以同时生效。
5. 在详情区查看基本信息、实际文件和版本记录。
6. **Save version** 创建不可变的完整资产快照。
7. **Export** 导出工作副本或选中的历史版本；已有目标不会被覆盖。

版本号只由 **Save version** 维护，普通元数据编辑不会改变版本。

## 资产库与坚果云

资产库是普通目录，可直接放入坚果云同步位置：

```text
xIPs Library/
  reset_gen/
    .xips.json
    rtl/
    constraints/
    .xips/
      versions/
        1.0.0/
        1.1.0/
  uart_rx_sv/
    .xips.json
    uart_rx.sv
    .xips/
      versions/
        1.0.0/
```

单文件也使用独立资产目录。xIPs 自动保留原始文件名，并以同一套搜索、版本和导出逻辑管理；不会为它增加 Module、Code Block 或 HDL 类型字段。

## 自定义分组

新增或编辑资产时，在 **Groups** 中输入逗号分隔的名称，例如 `AXI, UART, Common`。这些名称仍存储在 manifest 的 `tags` 数组中，左侧 Groups 列表会自动汇总并显示资产数量。

- 一个资产可以属于多个分组。
- 分组名称不区分大小写，重复名称会自动合并。
- 新名称随任一资产保存后自动出现；从所有资产中移除后自动消失。
- 分组只是视图和筛选，不改变资产目录，也不引入数据库或新的 manifest 字段。

xIPs 不实现云账号或上传协议。资产、manifest 和版本快照由坚果云按普通文件同步。`.git`、构建目录、`.Xil`、`ip_user_files`、缓存和内部版本目录不会进入新快照。

云端文件发生变化后，使用 **Refresh** 重新读取资产库。xIPs 不自动合并多个设备同时修改产生的冲突。

## Manifest

每个资产目录只需要一个最小 `.xips.json`：

```json
{
  "schemaVersion": 1,
  "id": "reset_gen",
  "name": "Reset Generator",
  "description": "Synchronizes reset into a target clock domain.",
  "version": "1.2.0",
  "tags": ["cdc", "reset"]
}
```

载荷由资产目录中的实际文件决定，不再维护 `type`、`top`、`language`、`sources`、`constraints`、`documentation` 或 `tools` 分类字段。详细规则见 [Manifest 格式](docs/manifest-format.md)。

## 构建

要求：

- Qt 6.5 或更新版本，包含 Core、Concurrent、Widgets 和 Test
- CMake 3.25 或更新版本
- Ninja
- C++20 编译器

```powershell
$env:PATH = "E:\QT6\Tools\mingw1310_64\bin;E:\QT6\6.10.2\mingw_64\bin;$env:PATH"
E:\QT6\Tools\CMake_64\bin\cmake.exe -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_PREFIX_PATH=E:\QT6\6.10.2\mingw_64 `
  -DCMAKE_MAKE_PROGRAM=E:\QT6\Tools\Ninja\ninja.exe `
  -DCMAKE_CXX_COMPILER=E:\QT6\Tools\mingw1310_64\bin\g++.exe
E:\QT6\Tools\CMake_64\bin\cmake.exe --build build
E:\QT6\Tools\CMake_64\bin\ctest.exe --test-dir build --output-on-failure
```

精简 Windows 部署：

```powershell
E:\QT6\Tools\CMake_64\bin\cmake.exe --install build --prefix build\install
E:\QT6\6.10.2\mingw_64\bin\windeployqt.exe `
  --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
  --compiler-runtime `
  --skip-plugin-types generic,imageformats,iconengines,networkinformation,tls `
  build\install\bin\xips.exe
```

Qt Creator 中打开仓库根目录的 `CMakeLists.txt`，选择匹配的 Desktop MinGW Kit。可运行目标只有 `xips` 和 `xips-cli`。详细配置见 [Qt Creator 构建与调试](docs/qt-creator.md)。

## 最小集成边界

桌面应用支持：

```text
xips.exe --open-asset reset_gen
xips.exe --search clocking
xips.exe xips://asset/reset_gen
xips.exe "xips://search?q=clocking"
```

只读 CLI 支持：

```powershell
xips-cli --action list --library E:\Nutstore\xIPs --query uart
xips-cli --action resolve --library E:\Nutstore\xIPs --asset uart_ip
xips-cli --action resolve --library E:\Nutstore\xIPs --asset uart_ip --asset-version 1.0.0
```

CLI 只提供 `list` 和 `resolve`，用于未来 ZeroSlack 选择器或脚本查询。URI 注册、单实例转发和嵌入控件属于后续适配层，协议说明见 [CLI 与唤起协议](docs/cli-and-integration.md)。

## 数据规则

- `.xips.json` 只保存基本元数据，实际文件是资产事实来源。
- manifest 使用 `QSaveFile` 原子替换，未知用户字段在编辑时保留。
- 导入、版本和导出拒绝符号链接及 Windows junction。
- 写入先进入暂存目录，再通过目录重命名发布。
- 版本快照不可修改；工作副本可以继续编辑。
- 内容哈希只在保存版本或明确解析工作副本时计算，不在每次刷新时读取全部文件内容。

## 测试

`core` 覆盖最小 manifest、目录与单文件导入、分组名称规范化、文件枚举、按需哈希、稳定 ID、版本、导出、URI 和真实 CLI 子进程。`gui_smoke` 覆盖 Groups 汇总与筛选、文件导入入口、文件清单、版本列表和按 ID 唤起。GUI 测试固定使用 Qt offscreen 平台，不操作桌面。

当前版本尚未提供安装器、代码签名、自动更新、Windows URI 注册、单实例 IPC 或 ZeroSlack 嵌入控件。
