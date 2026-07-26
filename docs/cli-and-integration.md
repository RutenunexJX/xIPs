# CLI 与唤起协议

xIPs 保留两个稳定且只读的外部边界：CLI 查询和 `xips://` URI。它们不修改资产，也不包含 ZeroSlack 专属数据结构。

## CLI

列出或搜索资产：

```powershell
xips-cli --action list --library E:\Nutstore\xIPs
xips-cli --action list --library E:\Nutstore\xIPs --query uart
```

解析工作副本或历史版本：

```powershell
xips-cli --action resolve --library E:\Nutstore\xIPs --asset uart_ip
xips-cli --action resolve --library E:\Nutstore\xIPs --asset uart_ip --asset-version 1.0.0
```

`XIPS_LIBRARY` 可替代 `--library`。所有结果均为单行 JSON：

```json
{
  "schemaVersion": 1,
  "ok": true,
  "action": "resolve",
  "data": {}
}
```

`list` 返回 ID、名称、版本、说明、分组（JSON 字段仍为 `tags`）、相对文件列表、路径、文件数量和资产 URI。`resolve` 额外返回解析版本、目录、绝对文件列表和按需计算的内容哈希；只有一个载荷文件时还返回 `resolvedFile`，供 ZeroSlack 等调用方直接定位该文件。错误写入标准错误流并返回非零退出码。

## URI 与桌面参数

```text
xips://show
xips://asset/<stable-id>
xips://search?q=<query>
```

等价桌面参数：

```text
xips.exe --open-asset <stable-id>
xips.exe --search <query>
```

当前版本不注册 Windows URI handler，也不把请求转发到已有进程。后续实现全局唤起或 ZeroSlack 嵌入时，继续复用稳定 ID、`list`、`resolve` 和上述 URI，不向资产 manifest 增加集成专属字段。
