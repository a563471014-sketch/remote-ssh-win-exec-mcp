# WinExec MCP for Remote-SSH

自包含扩展（exe + 源码 + 自动配置）：让 **Remote-SSH（Linux）端**运行的 AI agent（Copilot、Claude Code 等）在 **Windows 客户端**执行任意命令——CLI、脚本、Windows 特有命令、串口/硬件工具均可——立即返回 stdout/stderr/exit code（UTF-8）。

## English Summary

Self-contained extension (bundled exe + full C source code) that lets AI agents running on the **Remote-SSH (Linux) side** (Copilot, Claude Code, …) execute commands on the **Windows client** — any CLI, script, or Windows-specific tool (including serial-port/hardware utilities) — and get stdout/stderr/exit code back immediately over the existing SSH channel.

**Explicit consent first.** On first activation the extension shows a consent dialog. Until you agree, it **does not** modify any file and **does not** start any process.

### What this extension may modify (only after you consent, all opt-out)

| Target | What | Default |
|---|---|---|
| `<user-data>/User/mcp.json` | Adds/removes its own `win-exec-mcp` stdio server entry (bundled exe, runs locally) | **Off** until consent |
| Local process | Starts `win-exec-mcp.exe --http` listening on `127.0.0.1:38848` (Bearer-token protected, token auto-generated and stored locally) when a Remote-SSH window opens | **Off** until consent |
| `~/.ssh/config` | Appends one `Host` block with `RemoteForward 127.0.0.1:28848 → 127.0.0.1:38848` (backs up the file as `config.bak-winexec` first; never touches existing entries) | **Off** (`winExecMcp.ssh.autoForward`, default `false`) |
| Project `.vscode/mcp.json` + `.mcp.json` | Adds its HTTP server entry for agent clients | **Off** (`winExecMcp.project.autoRegister`, default `false`) |

### Security model

- The HTTP service binds to `127.0.0.1` by default and requires a Bearer token (auto-generated random token, persisted only in VS Code global storage; configurable via `winExecMcp.http.token`)
- The bundled `win-exec-mcp.exe` is fully open source in this package (`src/win-exec-mcp.c`) — build it yourself with mingw if you prefer
- No telemetry, no network calls other than the localhost MCP traffic

## 安装即用

```
code --install-extension win-exec-mcp-0.3.1.vsix
```

→ **Reload Window** → 首次激活会弹窗征得同意，点 **同意并启用** 即自动完成配置 → 无需其他手动配置。

> 拒绝后不会写入任何文件、不会拉起进程；之后可用命令 `WinExec MCP: Enable / Re-run setup (consent)` 或 `Register user mcp.json` 随时启用。

## 双模式（自动注册到用户级 mcp.json）

- **stdio（`win-exec-mcp`）**：location: local，Windows 客户端拉起进程，走 Remote-SSH 通道
- **http（`win-exec-mcp-http`）**：URL=127.0.0.1（客户端本机回环，不依赖局域网 IP）；进程仅远端窗口自动拉起；MCP 连接由扩展自动启动（`workbench.mcp.startServer`）
- 工具：`windows_exec(command, timeout_ms?=30000)`，支持 `&&`、管道等 cmd 语法

## 命令

- `WinExec MCP: Register user mcp.json` — 重写用户级注册
- `WinExec MCP: 注册到本项目 / 从本项目移除注册` — 项目级 http 注册（URL=局域网 IP，供 Linux 侧/外部客户端连接）
- `WinExec MCP: Start / Stop HTTP service`

## 设置

`winExecMcp.stdio.enabled` / `winExecMcp.http.enabled` / `winExecMcp.http.port` / `winExecMcp.http.token`（留空自动生成）
