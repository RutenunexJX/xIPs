# `.xips.json` manifest 格式

每个 IP 目录包含一个 `.xips.json`。manifest 只描述资产身份和用户元数据，目录中的实际文件始终是载荷事实来源。

## 格式

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

| 字段 | 必需 | 含义 |
| --- | --- | --- |
| `schemaVersion` | 是 | 当前固定为整数 `1`。 |
| `id` | 是 | 资产库内稳定且唯一的 ID，只使用字母、数字、`_`、`-`、`.`。 |
| `name` | 是 | 显示名称。 |
| `description` | 否 | 简要用途。 |
| `version` | 否 | 最近一次成功保存的快照版本，由 **Save version** 更新。 |
| `tags` | 否 | 用于搜索的标签数组。 |

新写入不再生成 `type`、`top`、`language`、`sources`、`constraints`、`documentation` 或 `tools`。读取旧 manifest 时这些字段不会参与业务逻辑；下一次写入会清除它们。其他未知用户字段会保留，避免普通元数据编辑造成无关数据丢失。

## 文件与版本

xIPs 枚举 IP 目录中的全部实际文件，同时排除：

- `.git`
- `.xips`
- `.cache`
- `.Xil`
- `ip_user_files`
- `build` 和 `build-*`

版本快照位于 `.xips/versions/<version>`。每个快照包含完整载荷、快照自己的 `.xips.json` 和内部 `.snapshot.json`；后者记录创建时间与 SHA-256 内容哈希。工作副本哈希不包含 `.xips`，因此新增历史版本不会改变工作副本内容身份。

manifest 通过 `QSaveFile` 原子替换。无效 JSON、错误 schema、无效 ID、无名称和重复 ID 会导致对应资产被扫描器跳过。
