# win-exec-mcp

Windows command execution MCP server for **Remote-SSH**: lets an AI agent
running on the **Linux side** (Copilot, Claude Code, any MCP client) run
commands on the **Windows client** and get stdout/stderr/exit code back — for
any Windows command, CLI or script.

- Single C file → one self-contained exe (stdio + streamable-http dual mode)
- VS Code extension: auto-registers MCP servers, auto-starts the HTTP service,
  auto-configures SSH RemoteForward

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
| `windows_exec` | `command` (required, cmd syntax, `&&` / pipes ok), `timeout_ms` (default 30000) | `[exit: N]` + stdout/stderr (UTF-8) |

## Install (three ways)

**A. VS Code extension (.vsix)** — full auto config:

```
code --install-extension win-exec-mcp-<ver>.vsix
```

→ Reload Window → if it says "updated", Reload again → done. The extension
auto-registers both MCP servers and (in a remote window) starts the HTTP
service + SSH forwarding.

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

## Dual MCP modes (auto-registered by the extension)

| server | type | notes |
|---|---|---|
| `win-exec-mcp` | stdio, location: local | Windows client spawns the exe; usable from the remote window via the Remote-SSH channel |
| `win-exec-mcp-http` | http, URL `http://127.0.0.1:<localPort>/mcp` | loopback URL (no LAN IP needed); HTTP process auto-started only in remote windows |

SSH RemoteForward (`winExecMcp.ssh.localPort`, default 28848 → Windows
`127.0.0.1:<http.port>`) lets **server-side** tools (Claude Code etc.) reach
the Windows MCP over `http://127.0.0.1:28848/mcp` — traffic stays inside the
SSH tunnel, no firewall/LAN-IP dependency.

## Settings (`winExecMcp.*`)

| key | default | meaning |
|---|---|---|
| `stdio.enabled` | true | register the stdio MCP server in user mcp.json |
| `http.enabled` | true | HTTP service switch (remote windows only) |
| `http.port` | 38848 | HTTP listen port |
| `http.token` | *(auto)* | **Bearer token — leave empty**: a random token is generated on first activation and persisted. Set one explicitly if you need a fixed token. |
| `http.host` | *(auto)* | manual LAN IP (multi-NIC / hotspot); empty = auto-detect |
| `ssh.localPort` | 28848 | server-side loopback port (RemoteForward) |
| `ssh.autoForward` | true | auto-append RemoteForward to `~/.ssh/config` (backs up first) |
| `project.autoRegister` | true | register http entry in project `.vscode/mcp.json` and `.mcp.json` |

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
- `WinExec MCP: 注册到本项目 / 从本项目移除注册` (project-level http, LAN IP URL)
- `WinExec MCP: Start / Stop HTTP service`

## Source layout

```
win-exec-mcp.c        # the only source (exe, stdio + http)
extension/            # VS Code extension (package.json / extension.js / bundled exe)
build.cmd             # compile -> refresh extension bundle -> pack dist/*.vsix
```

Version bump: change `version` in `extension/package.json` **and** `VER` in
`build.cmd`.
