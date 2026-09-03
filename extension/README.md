# WinExec MCP

自包含扩展（exe + 源码 + 自动配置）：让 **Remote-SSH（Linux）端**运行的 AI agent（Copilot、Claude Code 等）在 **Windows 客户端**执行任意命令——CLI、脚本、Windows 特有命令、串口/硬件工具均可——立即返回 stdout/stderr/exit code（UTF-8）。不局限于嵌入式开发。

## 安装即用

```
code --install-extension win-exec-mcp-0.3.1.vsix
```

→ **Reload Window** → 弹"已更新"提示时**再点一次 Reload** → 无需任何手动配置。

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
