# xIPs lean implementation plan

## 1. 产品边界

状态：完成。

- 只管理 IP，不解析 HDL，不执行外部工具，不管理依赖或 Git。
- 不使用 SQLite；资产目录与 manifest 是事实来源。
- 主界面只保留资产表、基本信息、文件清单和版本记录。

## 2. 最小数据模型

状态：完成。

- manifest 只建模 `schemaVersion`、`id`、`name`、`description`、`version` 和 `tags`。
- 版本只能通过 **Save version** 更新，元数据编辑不修改版本。
- 文件从目录实际内容枚举，不维护来源、约束、文档、语言或工具分类。
- 内容哈希只在保存版本或 resolve 工作副本时按需计算。

## 3. 文件与版本流程

状态：完成。

- 复制导入已有目录，并排除 Git、构建和 FPGA 工具缓存。
- 整个工作副本生成不可变快照，记录创建时间和 SHA-256。
- 工作副本或指定快照通过暂存目录导出，禁止覆盖已有目标。
- 坚果云只同步普通文件；云端变化后由用户显式刷新。

## 4. 集成边界

状态：完成基础边界。

- 桌面参数：`--open-asset`、`--search` 和 `xips://` URI。
- 只读 CLI：`list`、`resolve`。
- Windows URI 注册、单实例 IPC 和 ZeroSlack 嵌入控件属于后续适配层。

## 5. 验收

状态：完成。

- Debug、严格警告和全新 Release 构建均通过。
- `core` 与 Qt offscreen `gui_smoke` 共 10 个用例全部通过。
- 精简部署树为 32.9 MiB，只包含程序、Core/Gui/Widgets、MinGW 运行库、Windows 平台与样式插件及 README。
- 部署版 GUI/CLI 在移除 Qt/MinGW 开发环境 PATH 后返回 0.3.0，CLI 成功执行 `list` 与 `resolve`。
- 全程未操作可见桌面；最终界面快照由 offscreen 测试生成。
