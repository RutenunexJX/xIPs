# xIPs 1.1

xIPs 是个人使用的 FPGA 可复用资产管理器。它只处理三件事：收集代码资产、按名称或文件名查找资产、保存和取用历史版本。资产可以是完整 IP 目录，也可以是单个 `.v`、`.sv`、`.svh`、约束或脚本文件。

xIPs 不解析 HDL，不执行仿真或综合，不管理依赖，不接管 Git，也不实现云盘客户端。

## 开始使用

首次启动时选择一个资产库文件夹。该文件夹可以位于坚果云同步目录中；xIPs 不会自行创建隐藏的默认库。

主界面只保留三个高频入口：

- **Add**：选择多个文件、选择一个文件夹，或把多个文件和文件夹直接拖入窗口。导入会复制内容，原文件不变；名称和内部 ID 自动推断，重名时自动生成不冲突的名称。导入完成后可直接 **Undo**，只移除刚加入资产，不修改来源。
- **Search**：按资产名称、说明、分组、版本、实际文件名或相对路径查找。开始新搜索时自动回到全部资产；主动选择分组后，界面会明确显示当前范围。文件名命中直接显示在资产行内。
- **Copy working copy...**：在对话框中明确选择工作副本或保存版本、目标文件夹和最终名称，并在执行前显示完整目标路径。内部 `.xips.json` 和版本元数据不会带入工程，也不会覆盖已有目标。

选中资产后，详情区提供 **Open file/Open folder**、**Edit details** 和 **Save version**。版本号默认建议下一个补丁版本，例如 `1.2.9` 后建议 `1.2.10`。低频的 **Update selected asset from...** 和 **Delete selected asset...** 位于 **More**。

## 更新与删除

**Update selected asset from...** 用一个文件或目录替换当前工作副本。执行前会列出新增、替换、移除和未变化的文件数量，并明确显示将从工作副本移除的文件；资产名称、分组、说明、内部 ID 和保存版本保持不变。生产环境中旧工作副本移入系统回收站，正常失败会回滚，不能完全回滚时保留恢复位置并显示问题提示。

**Delete selected asset...** 会在确认框中显示工作文件数和保存版本数，然后把整个资产移入系统回收站。它不会删除最初导入的来源文件。删除单个保存版本仍只影响该版本，不影响工作副本。

## 资产与分组

每个资产在资产库中使用一个独立目录，最少包含 `.xips.json` 和实际载荷文件：

```text
xIPs Library/
  reset_gen/
    .xips.json
    rtl/reset_gen.sv
    constraints/reset_gen.xdc
  uart_rx_sv/
    .xips.json
    uart_rx.sv
```

左侧 Groups 来自各资产 manifest 的 `tags`。一个资产可以属于多个分组。编辑资产时直接勾选已有分组，或在单独输入框中创建一个新分组，不需要记忆逗号格式。表格支持多选，**Manage groups** 可以把所选资产加入新分组、重命名当前分组，或从所有资产中移除当前分组；这些操作不会移动或删除资产文件。

## 版本

**Save version** 保存完整、不可变的文件快照，位置为 `.xips/versions/<version>`。详情区只在选中资产时异步检查当前内容，并显示：

- 尚未保存版本；
- 与最近版本一致；
- 自最近版本后已修改。

内容没有变化时不会创建重复版本。Versions 页只列出实际保存版本，不再把工作副本伪装成一个版本。复制对话框始终明确列出工作副本和保存版本；保存版本也可以移入系统回收站，工作副本不会被删除。

版本不是 Git 提交。它是代码文件的完整副本，适合个人资产量和“保留少量有效版本、删除过时版本”的使用方式。

## 坚果云同步

xIPs 只读写普通文件和目录，因此可把整个资产库放在坚果云同步位置。应用启动和重新获得焦点时会防抖刷新，仍可从 **More > Refresh now** 手动刷新。无法读取的 manifest、重复 ID、部分导入失败或恢复副本会通过非模态提示条显示，并保留在 **More > Problems**，直到用户查看。

xIPs 不自动合并两台设备对同一文件的并发修改。发生云盘冲突时，应先在文件系统中确认需要保留的副本，再回到 xIPs 刷新。

## Manifest

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

详细规则见 [Manifest 格式](docs/manifest-format.md)。

## 构建与测试

要求 Qt 6.5 或更新版本（Core、Concurrent、Widgets、Test）、CMake 3.25、Ninja 和 C++20 编译器。

```powershell
$env:PATH = "E:\QT6\Tools\mingw1310_64\bin;E:\QT6\6.10.2\mingw_64\bin;$env:PATH"
E:\QT6\Tools\CMake_64\bin\cmake.exe -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DBUILD_TESTING=ON `
  -DCMAKE_PREFIX_PATH=E:\QT6\6.10.2\mingw_64 `
  -DCMAKE_MAKE_PROGRAM=E:\QT6\Tools\Ninja\ninja.exe `
  -DCMAKE_CXX_COMPILER=E:\QT6\Tools\mingw1310_64\bin\g++.exe
E:\QT6\Tools\CMake_64\bin\cmake.exe --build build
E:\QT6\Tools\CMake_64\bin\ctest.exe --test-dir build --output-on-failure
```

`core` 验证导入、更新、完整资产删除、版本和精确目标复制的边界；`gui_smoke` 在 Qt offscreen 平台验证主界面；`user_journey` 在临时坚果云目录中完成“导入—查找—保存版本—更新工作副本—复制指定版本—分组—撤销导入—同步刷新—删除资产”的完整任务。测试不会操作可见桌面，也不会向系统回收站写入测试数据。

Qt Creator 配置见 [Qt Creator 构建与调试](docs/qt-creator.md)。只读 CLI 和未来全局唤起边界见 [CLI 与唤起协议](docs/cli-and-integration.md)。
