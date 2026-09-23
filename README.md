# win-exec-mcp

Windows command execution MCP server for **Remote-SSH**: lets an AI agent
running on the **Linux side** (Copilot, Claude Code, any MCP client) run
commands on the **Windows client** and get stdout/stderr/exit code back — for
any Windows command, CLI or script.

- Single C file → one self-contained exe (stdio + streamable-http dual mode)
- VS Code extension: one consent dialog, two scopes — **VS Code only**, or
  **+ server-side agents** (also appends the SSH RemoteForward and registers
  the project files; no manual editing required)

## Why

In a Remote-SSH setup the agent runs on Linux, but plenty of things only exist
on the Windows client: Windows-only CLIs and scripts, serial/COM ports and
hardware tools (esptool, adb, …), GUI-adjacent utilities, whatever else the
Windows machine has and the Linux server doesn't. This extension bridges the
two: agent calls `windows_exec("...")`, the command executes on Windows, output
returns over the MCP channel (optionally through the SSH encrypted tunnel).

## Tool

| tool | params | returns |
|---|---|---|
| `windows_exec` | `command` (required, cmd syntax, `&&` / pipes ok), `timeout_ms` (default 30000), `shell` (`cmd` default / `gitbash` for Linux-style commands and .sh scripts) | `[exit: N]` + stdout/stderr (UTF-8) |

## Behavior notes

- **Live output**: long-running commands stream output to the client via `notifications/progress` (when the client sends a `progressToken` — VS Code does). Messages are line-aligned, rate-limited (~10/s), and mark skipped content as `…[skipped N lines]…`; the final result still carries the full output.
- **Large results**: outputs over 512KB are spilled to `%TEMP%\win-exec-mcp\out-<timestamp>.log`; the tool result returns the tail plus the full log path.
- **Termination**: timeout, client cancellation (`notifications/cancelled`) or client disconnect terminate the **whole process tree** (Job Object, `taskkill /T` fallback). Deliberately-detached background jobs (`start /b …`) survive normal completion.
- **HTTP service**: binds `127.0.0.1` by default (`--bind 0.0.0.0` or `winExecMcp.http.host` for LAN) and uses `SO_EXCLUSIVEADDRUSE` — a second instance on the same port fails to bind and exits immediately (no instance pile-up; the extension watchdog never spawns a duplicate while its own child is alive, and backs off on repeated failures).
- **Environment variables**: `WINEXEC_PROGRESS_MS` (default 100), `WINEXEC_PROGRESS_BYTES` (default 1200), `WINEXEC_MAX_RESULT_BYTES` (default 524288, `0` disables spilling), `WINEXEC_GITBASH` (explicit `bash.exe` path override), `WINEXEC_DEBUG` (startup diagnostics to stderr).

## Install (three ways)

**A. VS Code extension (.vsix)** — consent dialog, then full auto config:

```
code --install-extension win-exec-mcp-<ver>.vsix
```

→ Reload Window → pick a scope in the consent dialog: **仅 VS Code** or
**完整配置（含服务器端 agent）**. The full option also appends the SSH
RemoteForward and writes the project `.vscode/mcp.json` / `.mcp.json`, so
server-side Claude Code / Cursor connect without any manual editing. It can
also be enabled later with `WinExec MCP: 配置服务器端 agent`. Once authorized
(including consent given in older versions), remote windows auto-enable and
repair the chain (RemoteForward + project registration incl. token), asking
for a window reload only when files were fixed.

**B. Standalone exe (stdio only, no VS Code):**

```
win-exec-mcp.exe                      # stdio mode, newline JSON-RPC
win-exec-mcp.exe --http <port> --token <token>   # streamable-http mode
```

**C. From source** — needs mingw gcc (tested with Strawberry Perl's
`x86_64-w64-mingw32-gcc`):

```
build.cmd        # compile -> refresh extension bundle -> pack vsix
```

## Dual MCP modes (registered by the extension)

| server | type | notes |
|---|---|---|
| `win-exec-mcp` | stdio, location: local | Windows client spawns the exe; usable from the remote window via the Remote-SSH channel |
| `win-exec-mcp-http` | http, URL `http://127.0.0.1:<localPort>/mcp` | loopback URL (no LAN IP needed); HTTP process auto-started only in remote windows |

SSH RemoteForward (`winExecMcp.ssh.localPort`, default 28848 → Windows
`127.0.0.1:<http.port>`) lets **server-side** tools (Claude Code etc.) reach
the Windows MCP over `http://127.0.0.1:28848/mcp` — traffic stays inside the
SSH tunnel, no firewall/LAN-IP dependency.

> **Which agent sees which server.** The **stdio** server is registered in
> VS Code's own user-level `mcp.json` (a VS Code-specific format, incl.
> `location: local`), so only VS Code built-in agents (Copilot Chat) see it —
> third-party agents do not read that file. Any agent outside VS Code (Claude
> Code, Cursor, …), whether on the server or elsewhere, should connect over
> **http** instead: `http://127.0.0.1:28848/mcp` + the Bearer token (see the
> project `.mcp.json` for a ready-made entry).

## Settings (`winExecMcp.*`)

| key | default | meaning |
|---|---|---|
| `stdio.enabled` | true | register the stdio MCP server in user mcp.json |
| `http.enabled` | true | HTTP service switch (remote windows only) |
| `http.port` | 38848 | HTTP listen port |
| `http.token` | *(auto)* | **Bearer token — leave empty**: a random token is generated on first activation and persisted. Set one explicitly if you need a fixed token. |
| `http.host` | *(auto)* | manual LAN IP (multi-NIC / hotspot); empty = auto-detect |
| `ssh.localPort` | 28848 | server-side loopback port (RemoteForward) |

## Security notes

- `windows_exec` runs **arbitrary commands on the Windows machine** — only
  connect MCP clients you trust.
- HTTP service is protected by a Bearer token (`http.token`); the default is a
  **random per-install token**, not a hardcoded secret.
- When the extension process starts in a UNC working directory, the exe
  detects it and runs child commands from the system drive instead, avoiding
  the noisy "UNC path not supported" warning.

## Commands

- `WinExec MCP: Register user mcp.json`
- `WinExec MCP: 配置服务器端 agent（SSH 转发 + 项目注册）` (one-click = the “+ server-side agents” consent choice)
- `WinExec MCP: 注册到本项目 / 从本项目移除注册` (project-level http; SSH-tunnel URL by default, LAN IP when `http.host` is set)
- `WinExec MCP: Start / Stop HTTP service`

## Source layout

```
win-exec-mcp.c        # the only source (exe, stdio + http)
extension/            # VS Code extension (package.json / extension.js / bundled exe)
build.cmd             # compile -> refresh extension bundle -> pack dist/*.vsix
```

Version bump: change `version` in `extension/package.json` **and** `VER` in
`build.cmd`.
