# xIPs lean implementation plan

## 1. 产品收缩

状态：完成。

- 用户可见资产统一为 IP。
- 删除 Slang、依赖求解、Reference/Vendor、Git、差异、测试运行器和 Code Block 交付功能。
- 删除 SQLite；资产库刷新直接读取 manifest 和文件，搜索使用内存目录。
- 主界面从十列表格、三栏过滤器和五个详情页缩减为五列表格及 Files/Versions 两个详情页。

## 2. 资产与版本流程

状态：完成。

- 将已有目录复制到受管资产库，排除 Git、构建和 FPGA 工具缓存。
- 新资产统一生成 `type: ip` 的最小 `.xips.json`。
- 编辑名称、当前版本、说明和标签；稳定 ID 不允许修改。
- 对整个工作副本生成不可变版本快照，保存时间和 SHA-256 内容哈希。
- 工作副本或指定快照通过同目录暂存后导出，禁止覆盖已有目标。

## 3. 同步与兼容

状态：完成。

- 资产及 `.xips/versions` 可由坚果云按普通文件同步。
- 不生成需要同步的 SQLite 数据库。
- 旧 `module`、`code-block` 类型和未知 manifest 字段仍能读取；编辑后类型归一为 IP。
- 无效 manifest、重复 ID、路径越界和缺失声明文件在扫描结果中报告。

## 4. 集成边界

状态：完成基础边界。

- `xips://asset/<id>`、`xips://search?q=...`。
- 桌面参数 `--open-asset` 与 `--search`。
- 只读 CLI：list、resolve、link、parse-uri。
- 未实现 Windows 协议注册、单实例 IPC 和 ZeroSlack 嵌入控件；这些属于后续适配层。

## 5. 验收

状态：完成。

- Debug 全新构建通过；`core` 与 Qt offscreen `gui_smoke` 均通过。
- 严格警告构建使用 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`，无新增警告，两套测试再次通过。
- 全新 Release 构建完成 43 个步骤；CTest 2/2 通过，共 10 个功能用例，耗时 0.65 秒。
- 离屏首屏截图由测试进程生成，首屏、搜索、文件选择、版本列表和二进制保护断言均通过。
- `cmake --install` 与 `windeployqt` 生成 `build-lean-release/install`。
- 在 PATH 中移除 Qt/MinGW 开发目录后，部署版 `xips.exe --version` 和 `xips-cli.exe --version` 均返回 0.2.0；CLI 成功列出 3 个安装示例并生成 `xips://asset/reset_gen`。
- `git diff --check` 通过；旧功能实现、旧测试和旧行为文档已从源码中删除。
