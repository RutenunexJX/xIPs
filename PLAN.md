# xIPs lean implementation plan

## 1. 产品边界

状态：完成。

- 只管理可复用资产；来源可以是完整 IP 目录或单个文件，不解析 HDL，不执行外部工具，不管理依赖或 Git。
- 不使用 SQLite；资产目录与 manifest 是事实来源。
- 主界面只保留资产表、基本信息、文件清单和版本记录。

## 2. 最小数据模型

状态：完成。

- manifest 只建模 `schemaVersion`、`id`、`name`、`description`、`version` 和 `tags`。
- `tags` 直接作为用户分组；界面按分组汇总、计数并与文本搜索组合过滤。
- 版本只能通过 **Save version** 更新，元数据编辑不修改版本。
- 文件从目录实际内容枚举，不维护来源、约束、文档、语言或工具分类。
- 内容哈希只在保存版本或 resolve 工作副本时按需计算。

## 3. 文件与版本流程

状态：完成。

- 复制导入已有目录或单文件；目录导入排除 Git、构建和 FPGA 工具缓存。
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

- 严格 Debug 构建通过 `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`，无编译警告。
- `core` 与 Qt offscreen `gui_smoke` 共 11 个用例全部通过，包括单文件导入、分组汇总与组合过滤、快照和 CLI 具体文件路径解析。
- CLI 版本输出为 0.5.0。
- 全程未操作可见桌面；验证只使用一次性构建目录，未生成部署树或界面截图。
