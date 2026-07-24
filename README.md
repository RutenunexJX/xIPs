# xIPs

xIPs 是一个本地优先的个人 FPGA IP 资产管理器。它只解决四件事：收集 IP、查找 IP、保存版本、导出 IP。它不解析 HDL，不运行仿真或综合，不管理依赖，也不替代 ZeroSlack、Vivado 或 Git。

## 核心流程

桌面应用启动后直接显示 IP 资产库：

1. **Add IP** 将已有目录复制到受管资产库，并生成 `.xips.json`。
2. 搜索框按名称、ID、版本、标签、说明和路径过滤。
3. 详情区显示基本信息、实际文件、安全预览和版本记录。
4. **Save version** 创建不可变的完整快照。
5. **Export** 将工作副本或选中版本复制到目标目录，已有目标不会被覆盖。

旧 manifest 中的 `module`、`code-block` 和未知扩展字段仍可读取和保留；新导入或编辑的资产统一写为 `type: "ip"`。

## 坚果云同步

将应用的资产库目录选择为坚果云同步目录即可。xIPs 不实现云账号或上传协议，坚果云只负责文件传输。

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
```

资产、manifest 和版本快照会同步。构建目录、`.git`、`.Xil`、`ip_user_files` 和缓存目录在导入及版本快照时被排除。当前版本不使用 SQLite，因此不存在需要跨设备同步的数据库。

不要同时用 Git 和坚果云修改同一个工作目录。若多个设备并发修改同一 manifest，xIPs 会在下次扫描时报告无效 JSON、重复 ID 或缺失文件，但冲突内容仍需用户选择保留版本。

## 构建

要求：

- Qt 6.5 或更新版本，包含 Core、Concurrent、Widgets 和 Test
- CMake 3.25 或更新版本
- Ninja
- C++20 编译器

已验证的 Windows 配置：

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

运行示例资产库：

```powershell
.\build\src\xips.exe --library .\examples\library
```

### Qt Creator

在 Qt Creator 中打开仓库根目录的 `CMakeLists.txt`，选择与 Qt 安装匹配的 Desktop MinGW Kit 和 Debug 配置。可运行目标只有：

- `xips`：桌面应用
- `xips-cli`：只读集成桥接

桌面目标可设置参数：

```text
--library %{Project:DirName}/examples/library
```

详细 Kit 配置见 [Qt Creator 构建与调试](docs/qt-creator.md)。`CMakeLists.txt.user*` 是机器本地文件，不提交到仓库。

### Windows 部署

```powershell
E:\QT6\Tools\CMake_64\bin\cmake.exe --install build --prefix build\install
E:\QT6\6.10.2\mingw_64\bin\windeployqt.exe `
  --no-translations --compiler-runtime build\install\bin\xips.exe
```

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
xips-cli --action link --asset uart_ip
xips-cli --action parse-uri --uri xips://asset/uart_ip
```

所有 CLI 结果使用带 `schemaVersion`、`ok`、`action` 和 `data` 的 JSON envelope。该边界用于未来的全局唤起和 ZeroSlack IP 选择器；当前版本不安装操作系统 URI handler，也不嵌入 ZeroSlack。

## 数据与安全规则

- `.xips.json` 是资产元数据；源码文件仍是事实来源。
- manifest 使用 `QSaveFile` 原子替换，并保留未知字段。
- 导入、版本和导出拒绝符号链接或 Windows junction，防止复制越界。
- 导入、版本和导出先写暂存目录，再以目录重命名发布。
- 目标目录已存在时导出失败，不静默覆盖。
- `.xips/versions/<version>` 中的版本不可修改；工作副本仍可继续编辑。
- 搜索只读取启动或文件变化时构建的内存目录，不扫描 HDL 语义。

最小 manifest 格式见 [Manifest 格式](docs/manifest-format.md)，集成协议见 [CLI 与唤起协议](docs/cli-and-integration.md)。

## 自动验收

当前测试分为两套：

- `core`：manifest 兼容、复制导入、全文件哈希、稳定 ID、版本快照、导出、URI 和真实 CLI 子进程。
- `gui_smoke`：精简首屏、搜索、文件预览、二进制保护和按 ID 唤起；强制使用 Qt offscreen 平台。

2026-07-25 的最终验收使用 Qt 6.10.2、MinGW 13.1.0、CMake 3.30.5 和 Ninja。全新 Release 构建完成 43 个步骤；CTest 2/2 通过，两个套件共 10 个功能用例。严格警告构建无新增警告。安装树位于 `build-lean-release/install`，部署版 GUI/CLI 在不使用开发工具 PATH 时通过 0.2.0 加载检查，CLI 正确列出 3 个安装示例。

## 当前限制

- 尚未注册 Windows `xips://` 协议，也未实现已有实例的进程间转发。
- 尚未提供供 ZeroSlack 直接嵌入的选择器控件；当前稳定边界是核心库、CLI 和 URI。
- 版本是完整目录快照，不做增量压缩或版本差异展示。
- 并发云同步冲突只做结构诊断，不自动合并文件。
- 没有安装器、代码签名或自动更新。
