# CLI 与唤起协议

集成层只提供资产定位，不执行导入、脚本、测试或其他应用的业务。ZeroSlack 后续可以调用该边界，也可以直接复用非 Widgets 核心库实现一个 IP 选择器。

## JSON envelope

除 `--help` 和 `--version` 外，`xips-cli` 每次输出一个 JSON 对象：

```json
{
  "schemaVersion": 1,
  "ok": true,
  "action": "resolve",
  "data": {}
}
```

失败时 `ok` 为 `false`，并包含 `error`。退出码：

- `0`：成功
- `2`：参数或协议错误
- `3`：资产库读取错误
- `4`：IP 或指定版本不存在

## list

```powershell
xips-cli --action list --library E:\Nutstore\xIPs
xips-cli --action list --library E:\Nutstore\xIPs --query uart
```

返回匹配 IP 的 ID、名称、当前版本、标签、路径、内容哈希、文件数和 `xips://` 链接。`XIPS_LIBRARY` 可替代 `--library`。

## resolve

```powershell
xips-cli --action resolve `
  --library E:\Nutstore\xIPs `
  --asset uart_ip

xips-cli --action resolve `
  --library E:\Nutstore\xIPs `
  --asset uart_ip `
  --asset-version 1.0.0
```

未指定 `--asset-version` 时返回工作副本路径；指定后返回不可变快照路径与对应哈希。该动作只读文件，不更新“最近使用”状态。

## URI

稳定 URI 只有三种动作：

```text
xips://show
xips://asset/<stable-id>
xips://search?q=<query>
```

生成或解析 URI：

```powershell
xips-cli --action link --asset uart_ip
xips-cli --action link --query "axi fifo"
xips-cli --action parse-uri --uri xips://asset/uart_ip
```

桌面程序接受相同 URI，也接受等价参数：

```powershell
xips.exe --open-asset uart_ip
xips.exe --search "axi fifo"
```

当前版本不写 Windows 注册表、不注册 `xips://` handler，也不将请求转发到已有进程。后续实现全局唤起时，应继续使用 `ActivationRequest` 的 `schemaVersion: 1` JSON，而不是扩展资产模型或读取 ZeroSlack 数据库。
