# win-exec-mcp

**中文** | [English](#english)

---

## 中文

Windows 命令执行 MCP 服务器，面向 **Remote-SSH**：让运行在 **Linux 端**的 AI agent（Copilot、Claude Code、任意 MCP 客户端）在 **Windows 客户端**执行命令并取回 stdout/stderr/exit code —— 任何 Windows 命令、CLI 或脚本都行。

- 单个 C 文件 → 一个自包含 exe（stdio + streamable-http 双模式）
- VS Code 扩展：一次同意弹窗、两种范围 —— **仅 VS Code**，或 **+ 服务器端 agent**（同时追加 SSH RemoteForward 并注册项目文件，无需手动编辑）

界面文案跟随 VS Code 显示语言（简体中文 / English）。

### 为什么

Remote-SSH 场景里 agent 跑在 Linux，但很多东西只存在于 Windows 客户端：Windows 专有 CLI 与脚本、串口/COM 与硬件工具（esptool、adb…）、贴近 GUI 的实用程序等等。本扩展把两端接起来：agent 调用 `windows_exec("...")`，命令在 Windows 执行，输出经 MCP 通道（可选走 SSH 加密隧道）返回。

### 工具

| 工具 | 参数 | 返回 |
|---|---|---|
| `windows_exec` | `command`（必填，cmd 语法，支持 `&&` / 管道）、`timeout_ms`（默认 30000，上限 24h）、`cwd`（工作目录，可选）、`shell`（`cmd` 默认 / `gitbash` / `powershell`——pwsh 7 优先、自动回退 5.1）、`env`（可选，`{"KEY":"VAL"}` 叠加环境变量，`null` 删除）、`stdin`（可选，文本喂给子进程 stdin） | 首行机器可读哨兵 `[exit-json: {"code":0,"ms":625,"shell":"pwsh","timedOut":false,"cancelled":false,"clientGone":false,"spawnError":false,"spilled":false,"log":null}]`（`code` 在超时/取消/断连/spawn 失败时为 `null`）+ 正文 stdout/stderr 从第二行开始（UTF-8，逐行判定编码） |
| `windows_fs_put` | `path`（必填，Windows 绝对路径，父目录自动创建）、`content` 或 `content_base64`（二选一，文本/base64）、`offset`（可选，默认 0=创建/覆盖；>0 分块续传） | 写入确认；结构化字段：`path`/`bytesWritten`/`offset`/`size` |
| `windows_fs_get` | `path`（必填）、`offset`/`length`（可选，单次上限 4MB）、`as`（`base64` 默认（无损）/`text`（编码探测）） | 文件内容（base64 或文本）+ 元数据行 `[win-exec meta]`：`path`/`size`/`offset`/`bytesRead`/`eof`/`as` |
| `windows_bg_start` | `command`（必填）、`shell`/`cwd`/`env`（可选） | `jobId`/`pid`/`logPath`；完整输出落盘 |
| `windows_bg_poll` | `job_id`（必填）、`wait_ms`（可选 0–30000 长轮询）、`max_bytes`（可选，默认 64KB） | 增量输出（`content`）+ `{running, exitCode, killed, unreadBytes, logPath, elapsedMs}` |
| `windows_bg_kill` | `job_id`（必填） | 终止整棵进程树 |
| `windows_bg_list` | —（无参数） | 所有后台作业及状态 |
| `windows_env` | —（无参数） | OS/主机/管理员、git-bash/pwsh 路径、工具链存在性、串口列表、磁盘余量、代理 |

### 行为要点

- **实时输出**：长命令通过 `notifications/progress` 流式回报（客户端带 `progressToken` 时，VS Code 会带）。消息按完整行、限流发送（0.3.9 起默认 30ms / 单条最多 60000 字节）；溢出部分标注 `…[跳过 N 行]…`；最终结果仍含完整输出。
- **超大结果**：超过 512KB 落盘到 `%TEMP%\win-exec-mcp\out-<时间>-<pid>-<seq>.log`，工具只回传尾部 + 完整日志路径；输出在**运行中**超过 16MB 时也会立即改为边跑边写同一个落盘文件（**不再丢弃数据**，结果同样只回"路径 + 末尾"）；旧文件在每次写入时惰性回收（保留最新 20 个、删除超过 7 天的；`WINEXEC_SPILL_KEEP` / `WINEXEC_SPILL_KEEP_DAYS`，`0` = 关闭该规则）。
- **文件互传**：`windows_fs_put` / `windows_fs_get` 在 Linux ↔ Windows 间直传文件（脚本、配置、固件、日志、二进制）——不再受 32K 命令行上限与 echo 转义限制。二进制走 `content_base64`（读取默认返回无损 base64）；单次 ≤4MB，大文件按 `offset` 分块续传（`put` 的 `offset=0` 为创建/覆盖、重试幂等；`get` 返回 `eof` 供循环读取）；`put` 的父目录不存在会自动创建；`get` 的 `as:"text"` 做逐行编码探测（GBK/UTF-8 混排文本直接可读）。
- **后台作业**：`windows_bg_start` 起长驻进程（服务器/监听/串口监控）并立即返回 `job_id`；`windows_bg_poll(job_id, wait_ms?)` 增量取输出（长轮询：有新输出或退出即返回；等待结束用 `running`/`exitCode` 判断）；`windows_bg_kill` 杀整树；`windows_bg_list` 总览。完整输出落盘 `bg-*.log`（与结果落盘同池回收）；同时最多 16 个作业，**服务退出时全部作业进程树一并回收**。
- **终止**：超时、客户端取消（`notifications/cancelled`）或断连会终止**整棵进程树**（Job Object，兜底 `taskkill /T`）。断连检测不依赖"有输出可推"：服务在等待期间每 5 秒主动探活（HTTP = `select`+`MSG_PEEK` 判对端关闭，并向 SSE 流写保活注释；stdio = stdin 断了**且** stdout 写不进去才判定，`echo … | win-exec-mcp.exe` 这类一次性调用不会被误杀，`WINEXEC_PROBE_MS` 可调）——串口监听这类**静默长命令**在客户端被强杀 / SSH 掉线后也会被回收。刻意 `start /b` 的后台任务在服务存活期间保留，但**服务（exe）退出时会被一并终止**（自 0.4.4 起用 root Job + kill-on-close 实现）。
- **HTTP 服务**：默认只绑 `127.0.0.1`（`--bind 0.0.0.0` 或 `winExecMcp.http.host` 可到局域网），并用 `SO_EXCLUSIVEADDRUSE`：同端口第二个实例直接 bind 失败退出，不会堆叠；扩展 watchdog 在子进程存活时绝不重复拉起，并在连续失败时退避。
- **环境变量**：`WINEXEC_PROGRESS_MS`（默认 30）、`WINEXEC_PROGRESS_BYTES`（默认 60000）、`WINEXEC_MAX_RESULT_BYTES`（默认 524288，`0` 关闭落盘）、`WINEXEC_SPILL_KEEP`（默认 20）、`WINEXEC_SPILL_KEEP_DAYS`（默认 7）、`WINEXEC_PROBE_MS`（默认 5000，断连探活间隔毫秒）、`WINEXEC_TOKEN`（HTTP 鉴权 token，替代 `--token`，避免出现在命令行）、`WINEXEC_GITBASH`（显式指定 `bash.exe`）、`WINEXEC_DEBUG`（启动诊断到 stderr）。

### 安装（三种方式）

**A. VS Code 扩展（.vsix）** —— 同意弹窗后全自动配置：

```
code --install-extension win-exec-mcp-<ver>.vsix
```

→ Reload Window → 在同意弹窗里选 **仅 VS Code** 或 **完整配置（含服务器端 agent）**。完整配置同时追加 SSH RemoteForward 并向项目根写入 `.mcp.json`（VS Code 与 Claude Code / Cursor 都读这一个文件，单一来源避免重复注册），服务器端 Claude Code / Cursor 无需任何手动编辑即可用；之后也可随时用 `WinExec MCP: 配置服务器端 agent` 补齐。授权一次（含旧版本已同意）后，远端窗口会自动启用并修复整条链路，只有真的改了文件才提示重载。

**防火墙说明（为什么以前升级会坏）**：Windows 防火墙与安全套件按**程序路径**记忆网络授权 —— 每个新的扩展目录都像一个新程序（重新弹窗，更糟的是静默阻止：连回环连接都被丢包，表现为 "Waiting for server to respond to initialize" → "other side closed"）。自 **0.3.8** 起服务始终从固定路径 `%LOCALAPPDATA%\win-exec-mcp\bin\win-exec-mcp.exe` 运行（升级原地替换），放行一次即可长期有效。若服务连不上，运行命令 **`WinExec MCP: 修复防火墙权限（管理员）`**（或扩展目录里的 `fix-firewall.cmd`）：清除陈旧的 win-exec BLOCK 规则、为固定路径加放行、清理残留进程，然后重载窗口。若第三方安全套件（360、火绒、腾讯电脑管家…）用自己的网络过滤而不写 Windows 防火墙规则，请把固定路径加入它的信任/放行列表。

**B. 独立 exe（仅 stdio，无需 VS Code）：**

```
win-exec-mcp.exe                      # stdio 模式，换行 JSON-RPC
win-exec-mcp.exe --http <port> --token <token>   # streamable-http 模式
```

**C. 从源码构建** —— 需要 mingw gcc（用 Strawberry Perl 的 `x86_64-w64-mingw32-gcc` 测试过）：

```
build.cmd        # 编译 -> 刷新扩展内置文件 -> 打包 vsix
```

### 双 MCP 模式（由扩展注册）

| server | 类型 | 说明 |
|---|---|---|
| `win-exec-mcp` | stdio, location: local | Windows 客户端拉起 exe；远端窗口经 Remote-SSH 通道使用 |
| `win-exec-mcp-http` | http, `http://127.0.0.1:<localPort>/mcp` | 回环 URL（不需要局域网 IP）；HTTP 进程只在远端窗口自动拉起 |

SSH RemoteForward（`winExecMcp.ssh.localPort`，默认 28848 → Windows `127.0.0.1:<http.port>`）让**服务器端**工具（Claude Code 等）通过 `http://127.0.0.1:28848/mcp` 访问 Windows 侧的 MCP —— 流量全程在 SSH 隧道内，不依赖防火墙/局域网 IP。

> **哪个 agent 能看到哪个 server**：**stdio** server 注册在 VS Code 自己的用户级 `mcp.json`（VS Code 专有格式，含 `location: local`），只有 VS Code 内建 agent（Copilot Chat）会读；第三方 agent 不读该文件。任何 VS Code 之外的 agent（Claude Code、Cursor…，无论在服务器还是别处）都应改用 **http**：`http://127.0.0.1:28848/mcp` + Bearer token（项目 `.mcp.json` 里已有现成条目）。

### 设置（`winExecMcp.*`）

| 键 | 默认值 | 含义 |
|---|---|---|
| `stdio.enabled` | true | 在用户级 mcp.json 注册 stdio MCP server |
| `http.enabled` | true | HTTP 服务开关（仅远端窗口） |
| `http.port` | 38848 | HTTP 监听端口 |
| `http.token` | *(自动)* | **Bearer token —— 留空即可**：首次激活生成随机 token 并持久化；需要固定 token 时再显式设置 |
| `http.host` | *(自动)* | 手动局域网 IP（多网卡/热点）；留空 = 自动探测 |
| `ssh.localPort` | 28848 | 服务器端回环端口（RemoteForward） |

### 安全说明

- `windows_exec` 会在 **Windows 机器上执行任意命令** —— 只连接你信任的 MCP 客户端。
- HTTP 服务由 Bearer token 保护（`http.token`）；默认是**每次安装随机生成的 token**，不是硬编码密钥。
- 扩展进程若在 UNC 工作目录启动，exe 会检测到并从系统盘执行子命令，避免 "UNC path not supported" 噪音警告。

### 命令

- `WinExec MCP: Register user mcp.json`
- `WinExec MCP: 配置服务器端 agent（SSH 转发 + 项目注册）`（一键 = 同意弹窗里的“+ 服务器端 agent”）
- `WinExec MCP: 注册到本项目 / 从本项目移除注册`（项目级 http；默认 SSH 隧道 URL，设置 `http.host` 后为局域网 IP）
- `WinExec MCP: Start / Stop HTTP service`
- `WinExec MCP: 修复防火墙权限（管理员）`（清除 win-exec 防火墙阻止规则 + 放行固定路径；UAC 确认后重载）
- `WinExec MCP: 诊断` / `重新生成 HTTP token`；远端窗口状态栏显示服务状态与调用活动（空闲/启动/调用中/正在输出四种图标，不用数字也不改底色），点击出操作菜单

### 源码结构

```
win-exec-mcp.c        # 唯一源码（exe，stdio + http）
extension/            # VS Code 扩展（package.json / extension.js / 内置 exe）
build.cmd             # 编译 -> 刷新扩展内置文件 -> 打包 dist/*.vsix
```

版本号必须同步（全部一致）：`extension/package.json` 的 `version`、`build.cmd` 的 `VER`、`pack.ps1` 的 `$Version` 默认值、`vsix/extension.vsixmanifest` 的 `Version`、`extension/README.md` 里的示例文件名、以及 `win-exec-mcp.c` 的 `serverInfo.version`。

---

## English

Windows command execution MCP server for **Remote-SSH**: lets an AI agent
running on the **Linux side** (Copilot, Claude Code, any MCP client) run
commands on the **Windows client** and get stdout/stderr/exit code back — for
any Windows command, CLI or script.

- Single C file → one self-contained exe (stdio + streamable-http dual mode)
- VS Code extension: one consent dialog, two scopes — **VS Code only**, or
  **+ server-side agents** (also appends the SSH RemoteForward and registers
  the project files; no manual editing required)
- UI text follows the VS Code display language (English / Simplified Chinese)

### Why

In a Remote-SSH setup the agent runs on Linux, but plenty of things only exist
on the Windows client: Windows-only CLIs and scripts, serial/COM ports and
hardware tools (esptool, adb, …), GUI-adjacent utilities, whatever else the
Windows machine has and the Linux server doesn't. This extension bridges the
two: agent calls `windows_exec("...")`, the command executes on Windows, output
returns over the MCP channel (optionally through the SSH encrypted tunnel).

### Tool

| tool | params | returns |
|---|---|---|
| `windows_exec` | `command` (required, cmd syntax, `&&` / pipes ok), `timeout_ms` (default 30000, max 24h), `cwd` (working directory, optional), `shell` (`cmd` default / `gitbash` / `powershell` — pwsh 7 preferred, auto-falls back to 5.1), `env` (optional `{\"KEY\":\"VAL\"}` overlay; `null` removes), `stdin` (optional text fed to the child's stdin) | first line is a machine-readable sentinel `[exit-json: {"code":0,"ms":625,"shell":"pwsh","timedOut":false,"cancelled":false,"clientGone":false,"spawnError":false,"spilled":false,"log":null}]` (`code` is `null` on timeout/cancel/disconnect/spawn failure) + body stdout/stderr from the second line on (UTF-8, per-line encoding detection) |
| `windows_fs_put` | `path` (required, Windows absolute path, parent dirs auto-created), `content` or `content_base64` (one of, text/base64), `offset` (optional, default 0 = create/overwrite; >0 for chunked upload) | write ack; structured fields: `path`/`bytesWritten`/`offset`/`size` |
| `windows_fs_get` | `path` (required), `offset`/`length` (optional, ≤4MB per call), `as` (`base64` default (lossless) / `text` (encoding detection)) | file content (base64 or text) + metadata line `[win-exec meta]`: `path`/`size`/`offset`/`bytesRead`/`eof`/`as` |
| `windows_bg_start` | `command` (required), `shell`/`cwd`/`env` (optional) | `jobId`/`pid`/`logPath`; full output spilled to disk |
| `windows_bg_poll` | `job_id` (required), `wait_ms` (optional 0–30000 long-poll), `max_bytes` (optional, 64KB default) | incremental output (`content`) + `{running, exitCode, killed, unreadBytes, logPath, elapsedMs}` |
| `windows_bg_kill` | `job_id` (required) | terminates the whole process tree |
| `windows_bg_list` | — | all background jobs and their status |
| `windows_env` | — | OS/host/admin, git-bash/pwsh paths, toolchain presence, COM ports, disks, proxy |

### Behavior notes

- **Live output**: long-running commands stream output to the client via `notifications/progress` (when the client sends a `progressToken` — VS Code does). Messages are line-aligned and rate-limited (30 ms, up to 60000 bytes per message by default since 0.3.9 — typical output arrives without gaps); when more accumulates than one window holds, the omitted part is marked as `…[skipped N lines]…`. The final result still carries the full output.
- **Large results**: outputs over 512KB are spilled to `%TEMP%\win-exec-mcp\out-<timestamp>-<pid>-<seq>.log`; the tool result returns the tail plus the full log path. Output that exceeds 16MB **while still running** switches to streaming into the same spill file as it arrives (**nothing is dropped**; the result again carries the path plus the tail). Old spill files are reclaimed lazily on each write (keep the newest 20, drop anything older than 7 days — `WINEXEC_SPILL_KEEP` / `WINEXEC_SPILL_KEEP_DAYS`, `0` disables either rule).
- **File transfer**: `windows_fs_put` / `windows_fs_get` move files between Linux ↔ Windows directly (scripts, configs, firmware, logs, binaries) — no more 32K command-line limit or echo escaping. Binary goes through `content_base64` (reads default to lossless base64); ≤4MB per call, larger files chunked via `offset` (`put` with `offset=0` creates/overwrites and first-chunk retry is idempotent; `get` reports `eof` for loop reads); missing parent directories are auto-created on `put`; `get` with `as:"text"` applies per-line encoding detection (mixed GBK/UTF-8 text reads correctly).
- **Background jobs**: `windows_bg_start` launches a long-running process (server/listener/serial monitor) and immediately returns a `job_id`; `windows_bg_poll(job_id, wait_ms?)` streams incremental output (long-poll: returns on new output or exit; use `running`/`exitCode` to wait for completion); `windows_bg_kill` terminates the whole tree; `windows_bg_list` shows all jobs. Full output spilled to `bg-*.log` (reclaimed together with tool-result spill files); up to 16 concurrent jobs, and all job trees are reclaimed **when the service exits**.
- **Termination**: timeout, client cancellation (`notifications/cancelled`) or client disconnect terminate the **whole process tree** (Job Object, `taskkill /T` fallback). Disconnect detection no longer depends on having output to push: while waiting, the service probes every 5s (HTTP = `select` + `MSG_PEEK` for a closed peer plus a keepalive SSE comment; stdio = only when stdin is broken **and** stdout is unwritable, so one-shot `echo … | win-exec-mcp.exe` calls are not killed by mistake; tune with `WINEXEC_PROBE_MS`) — so silent long-running commands (serial monitors) are reclaimed even if the client is force-killed or the SSH link drops. Deliberately-detached background jobs (`start /b …`) survive normal completion but are terminated **when the service (exe) exits** (0.4.4+: root job + kill-on-close).
- **HTTP service**: binds `127.0.0.1` by default (`--bind 0.0.0.0` or `winExecMcp.http.host` for LAN) and uses `SO_EXCLUSIVEADDRUSE` — a second instance on the same port fails to bind and exits immediately (no instance pile-up; the extension watchdog never spawns a duplicate while its own child is alive, and backs off on repeated failures).
- **Environment variables**: `WINEXEC_PROGRESS_MS` (default 30), `WINEXEC_PROGRESS_BYTES` (default 60000), `WINEXEC_MAX_RESULT_BYTES` (default 524288, `0` disables spilling), `WINEXEC_SPILL_KEEP` (default 20, spill files kept per directory, `0` disables count-based cleanup), `WINEXEC_SPILL_KEEP_DAYS` (default 7, max age in days, `0` disables age-based cleanup), `WINEXEC_PROBE_MS` (default 5000, liveness probe interval in ms), `WINEXEC_TOKEN` (HTTP bearer token — preferred over `--token`, keeps it out of the process command line), `WINEXEC_GITBASH` (explicit `bash.exe` path override), `WINEXEC_DEBUG` (startup diagnostics to stderr).

### Install (three ways)

**A. VS Code extension (.vsix)** — consent dialog, then full auto config:

```
code --install-extension win-exec-mcp-<ver>.vsix
```

→ Reload Window → pick a scope in the consent dialog: **仅 VS Code** or
**完整配置（含服务器端 agent）**. The full option also appends the SSH
RemoteForward and writes the project-root `.mcp.json` (read by both VS Code and
Claude Code / Cursor, a single source of truth), so
server-side Claude Code / Cursor connect without any manual editing. It can
also be enabled later with `WinExec MCP: 配置服务器端 agent`. Once authorized
(including consent given in older versions), remote windows auto-enable and
repair the chain (RemoteForward + project registration incl. token), asking
for a window reload only when files were fixed.
**Firewall note (why upgrades used to break).** Windows Firewall and security
suites remember network permissions **per executable path** — each new
extension folder looks like a brand-new program (popup again, or worse: a
silent block that drops even loopback connections, surfacing as "Waiting for
server to respond to initialize" → "other side closed"). Since **0.3.8** the
service always runs from a stable path,
`%LOCALAPPDATA%\win-exec-mcp\bin\win-exec-mcp.exe` (replaced in place on
upgrades), so a single allow lasts forever. If the service ever becomes
unreachable, run the command **`WinExec MCP: 修复防火墙权限（管理员）`** (or
`fix-firewall.cmd` shipped in the extension folder): it deletes stale win-exec
BLOCK rules, adds an inbound allow for the stable path, and kills leftover
processes — then reload the window. If a third-party security suite (360,
Huorong, Tencent PC Manager, …) enforces its own network filter instead of
Windows Firewall rules, add the stable path above to that suite's trust/allow
list if the one-click fix alone does not clear the block.
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

### Dual MCP modes (registered by the extension)

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

### Settings (`winExecMcp.*`)

| key | default | meaning |
|---|---|---|
| `stdio.enabled` | true | register the stdio MCP server in user mcp.json |
| `http.enabled` | true | HTTP service switch (remote windows only) |
| `http.port` | 38848 | HTTP listen port |
| `http.token` | *(auto)* | **Bearer token — leave empty**: a random token is generated on first activation and persisted. Set one explicitly if you need a fixed token. |
| `http.host` | *(auto)* | manual LAN IP (multi-NIC / hotspot); empty = auto-detect |
| `ssh.localPort` | 28848 | server-side loopback port (RemoteForward) |

### Security notes

- `windows_exec` runs **arbitrary commands on the Windows machine** — only
  connect MCP clients you trust.
- HTTP service is protected by a Bearer token (`http.token`); the default is a
  **random per-install token**, not a hardcoded secret.
- When the extension process starts in a UNC working directory, the exe
  detects it and runs child commands from the system drive instead, avoiding
  the noisy "UNC path not supported" warning.

### Commands

- `WinExec MCP: Register user mcp.json`
- `WinExec MCP: 配置服务器端 agent（SSH 转发 + 项目注册）` (one-click = the “+ server-side agents” consent choice)
- `WinExec MCP: 注册到本项目 / 从本项目移除注册` (project-level http; SSH-tunnel URL by default, LAN IP when `http.host` is set)
- `WinExec MCP: Start / Stop HTTP service`
- `WinExec MCP: 修复防火墙权限（管理员）` (clears win-exec firewall blocks + allows the stable path; UAC prompt, then reload)

### Source layout

```
win-exec-mcp.c        # the only source (exe, stdio + http)
extension/            # VS Code extension (package.json / extension.js / bundled exe)
build.cmd             # compile -> refresh extension bundle -> pack dist/*.vsix
```

Version bump (all must match): `version` in `extension/package.json`, `VER`
in `build.cmd`, the `$Version` default in `pack.ps1`, `Version` in
`vsix/extension.vsixmanifest`, the example filename in `extension/README.md`,
and `serverInfo.version` in `win-exec-mcp.c`.
