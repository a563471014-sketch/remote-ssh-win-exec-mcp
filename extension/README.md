# WinExec MCP for Remote-SSH

自包含扩展（exe + 源码 + 自动配置）：让 **Remote-SSH（Linux）端**运行的 AI agent（Copilot、Claude Code 等）在 **Windows 客户端**执行任意命令——CLI、脚本、Windows 特有命令、串口/硬件工具均可——立即返回 stdout/stderr/exit code（UTF-8）。

## English Summary

Self-contained extension (bundled exe + full C source code) that lets AI agents running on the **Remote-SSH (Linux) side** (Copilot, Claude Code, …) execute commands on the **Windows client** — any CLI, script, or Windows-specific tool (including serial-port/hardware utilities) — and get stdout/stderr/exit code back immediately over the existing SSH channel.

**Explicit consent first.** On first activation the extension shows a consent dialog with two scopes: **完整配置（含服务器端 agent）** — also appends `~/.ssh/config` and writes the project files below — or **仅 VS Code** — only the first two rows. Until you pick one, it **does not** modify any file and **does not** start any process. Once authorized (including consent given in older versions) it auto-enables and repairs that setup (RemoteForward + project registration incl. token) on every remote window start and only asks for a window reload when files were fixed; **仅 VS Code** keeps the external part off (command available to add it later).

### What this extension may modify (only after you consent, all opt-out)

| Target | What | Default |
|---|---|---|
| `<user-data>/User/mcp.json` | Adds/removes its own `win-exec-mcp` stdio server entry (bundled exe, runs locally) | **Off** until consent |
| Local process | Starts `win-exec-mcp.exe --http` listening on `127.0.0.1:38848` (Bearer-token protected, token auto-generated and stored locally) when a Remote-SSH window opens | **Off** until consent |
| `~/.ssh/config` | Appends one `Host` block with `RemoteForward 127.0.0.1:28848 → 127.0.0.1:38848` (backs up the file as `config.bak-winexec` first; never touches existing entries) | **Off** until authorized; maintained automatically afterwards (opt-out: 仅 VS Code / 暂不) |
| Project `.vscode/mcp.json` + `.mcp.json` | Adds its HTTP server entry for agent clients (contains the Bearer token) | **Off** until authorized; same automatic maintenance |

### Security model

- The HTTP service binds to `127.0.0.1` by default and requires a Bearer token (auto-generated random token, persisted only in VS Code global storage; configurable via `winExecMcp.http.token`)
- The bundled `win-exec-mcp.exe` is fully open source in this package (`src/win-exec-mcp.c`) — build it yourself with mingw if you prefer
- No telemetry, no network calls other than the localhost MCP traffic

## 安装即用

```
code --install-extension win-exec-mcp-0.3.8.vsix
```

→ **Reload Window** → 首次激活弹窗，按用途二选一：

- **完整配置（含服务器端 agent）**：除 VS Code 内建 agent 外，同时追加 `~/.ssh/config` 的 RemoteForward（先备份）并向当前项目写入 `.vscode/mcp.json` / `.mcp.json`（含 token）—— 服务器端的 Claude Code / Cursor 等直接可用，无需手改任何文件
- **仅 VS Code**：只注册用户级 `mcp.json` + 自动启动/连接本机 HTTP 服务；外部 agent 部分可随时用命令补齐（不会自动启用）

> **授权一次，之后全自动**：点过「同意/完整配置」的用户（含从旧版本升级）在远端窗口会**自动维护** SSH RemoteForward 与项目注册（URL/token）：缺失就补、过期就改，修复后提示「已修复配置文件，需重载窗口生效」（未打开项目时项目注册等打开后自动完成）。选「仅 VS Code」或点过「暂不」则外部配置保持关闭；重新运行 `WinExec MCP: Enable / Re-run setup (consent)` 可随时改选。排查看「输出 → WinExec MCP」日志。

> 拒绝（暂不）不会写入任何文件、不会拉起进程；之后可用命令 `WinExec MCP: Enable / Re-run setup (consent)` 或 `Register user mcp.json` 随时启用。

## 双模式

- **stdio（`win-exec-mcp`）**：注册到用户级 `mcp.json`（location: local），Windows 客户端拉起进程，走 Remote-SSH 通道 —— VS Code 内建 agent 可用
- **http**：注册到项目级 `.vscode/mcp.json` / `.mcp.json`，默认 URL `http://127.0.0.1:<ssh.localPort>/mcp`（SSH 回环隧道，与 IP 无关；设置 `http.host` 后改为局域网直连）—— 供 VS Code 之外的客户端（Claude Code 等）使用；进程仅远端窗口自动拉起
- 工具：`windows_exec(command, timeout_ms?=30000, shell?="cmd")`，支持 `&&`、管道等 cmd 语法；`shell:"gitbash"` 可用 git-bash 执行 Linux 风格命令 / .sh 脚本（自动检测本机 git-bash，未安装时明确报错）
- 长命令输出实时推送（客户端带 progressToken 时；按完整行 + 限流 + `跳过 N 行` 标注），最终结果仍含完整输出
- 结果超 512KB 自动落盘（`%TEMP%\win-exec-mcp\out-*.log`），只回传尾部与日志路径
- 超时 / 客户端取消 / 断连 → 终止**整棵进程树**（Job Object；兜底 taskkill /T）
- HTTP 服务默认只绑 `127.0.0.1`（`http.host` 可指定局域网），`SO_EXCLUSIVEADDRUSE` 单实例：同端口第二个实例直接 bind 失败退出，不会堆叠
- 环境变量：`WINEXEC_PROGRESS_MS` / `WINEXEC_PROGRESS_BYTES` / `WINEXEC_MAX_RESULT_BYTES`（0=关闭落盘）/ `WINEXEC_GITBASH` / `WINEXEC_DEBUG`

> **固定服务路径（0.3.8+）**：exe 始终从 `%LOCALAPPDATA%\win-exec-mcp\bin\win-exec-mcp.exe` 运行（升级时原地替换）——防火墙/安全软件按“路径”记授权，放行一次后升级不再重复弹窗、也不会被误拦。若服务连不上（端口连接超时＝被拦截）：运行命令 `WinExec MCP: 修复防火墙权限（管理员）`（UAC 确认后自动清除阻止规则、放行路径并清理残留进程），然后重载窗口。

## 命令

- `WinExec MCP: Register user mcp.json` — 重写用户级注册
- `WinExec MCP: 配置服务器端 agent（SSH 转发 + 项目注册）` — 一键开启 `ssh.autoForward` + `project.autoRegister`（= 首次弹窗的“完整配置”）
- `WinExec MCP: 注册到本项目 / 从本项目移除注册` — 项目级 http 注册（默认 URL 走 SSH 隧道 `127.0.0.1:28848`；设置 `http.host` 后为局域网直连，供 Linux 侧/外部客户端连接）
- `WinExec MCP: Start / Stop HTTP service`
- `WinExec MCP: 修复防火墙权限（管理员）` — 清除 win-exec 的防火墙阻止规则、放行固定路径并清理残留进程（UAC 确认；完成后重载窗口）

## 设置

`winExecMcp.stdio.enabled` / `winExecMcp.http.enabled` / `winExecMcp.http.port` / `winExecMcp.http.token`（留空自动生成）/ `winExecMcp.ssh.localPort`
