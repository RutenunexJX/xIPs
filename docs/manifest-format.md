# `.xips.json` manifest 格式

每个 IP 目录包含一个 `.xips.json`。所有文件路径均应相对 IP 根目录，使整个目录能够在坚果云、移动磁盘或其他电脑之间整体迁移。

## 最小对象

```json
{
  "schemaVersion": 1,
  "id": "reset_gen",
  "type": "ip",
  "name": "Reset Generator",
  "description": "Synchronizes reset into a target clock domain.",
  "version": "1.2.0",
  "top": "reset_gen",
  "language": "SystemVerilog",
  "sources": [
    "rtl/reset_gen.sv",
    "rtl/include/reset_config.svh"
  ],
  "constraints": [
    "constraints/reset_gen.xdc"
  ],
  "documentation": [
    "README.md"
  ],
  "tools": {
    "vivado": "2022.2"
  },
  "tags": [
    "cdc",
    "reset"
  ]
}
```

必需字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `schemaVersion` | integer | 当前为 `1`。 |
| `id` | string | 资产库内稳定且唯一的 ID，只使用字母、数字、`_`、`-`、`.`。 |
| `type` | string | 新资产固定为 `ip`。 |
| `name` | string | 显示名称。 |

可选字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `description` | string | 简要用途。 |
| `version` | string | 当前工作副本的版本标签。 |
| `top` | string | 顶层对象或包名，仅作为元数据。 |
| `language` | string | 例如 `SystemVerilog`、`VHDL`、`Mixed`。 |
| `sources` | string array | HDL、脚本、XCI/DCP 或其他载荷文件。 |
| `constraints` | string array | XDC、SDC 等约束文件。 |
| `documentation` | string array | 随 IP 保存的说明文件。 |
| `tools` | object | 工具及版本说明，不触发工具执行。 |
| `tags` | string array | 搜索和筛选标签。 |

xIPs 的文件页和内容哈希覆盖 IP 目录中的全部实际载荷，不依赖 `sources` 是否完整；上述列表用于提供可读元数据和缺失文件诊断。

## 兼容与写入

- 旧 `type: "module"` 和 `type: "code-block"` 能够读取；通过精简版界面编辑后写为 `ip`。
- schema 0 可在内存中迁移：`kind` → `type`、`displayName` → `name`、`files` → `sources`。
- 未知字段保存在原始 JSON 对象中，编辑基本元数据时不会被静默删除。
- 缺少 `schemaVersion`、使用未来 schema、无效 ID 或无名称均为错误。
- manifest 通过 `QSaveFile` 原子替换，不退化为直接覆盖。
- 路径逃逸 IP 根目录、声明文件缺失和重复 ID 会出现在扫描诊断中。

## 版本目录

版本快照位于 `.xips/versions/<version>`。每个快照包含完整载荷、快照自己的 `.xips.json` 和内部 `.snapshot.json`，后者记录创建时间与 SHA-256 内容哈希。`.xips` 不参与工作副本哈希，避免新增历史版本改变当前内容身份。
