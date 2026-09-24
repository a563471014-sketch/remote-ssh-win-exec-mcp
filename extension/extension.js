// WinExec MCP：Windows 命令执行 MCP 的安装器与托管器
// - stdio 模式：内置 exe 注册到用户级 mcp.json（location: local），Windows 客户端拉起，走 Remote-SSH 通道
// - http 模式：专职服务 VS Code 之外的客户端（SSH 隧道/局域网）；进程仅远端窗口自动拉起；
//   服务器本机工具经 ~/.ssh/config 的 RemoteForward 回环使用；项目级注册为可选命令
//   （http 条目不进用户级 mcp.json——VS Code 网关会绑定其回环端口遮蔽 exe，详见 ensureUserMcp 注释）
const vscode = require('vscode');
const { spawn } = require('child_process');
const net = require('net');
const fs = require('fs');
const path = require('path');
const os = require('os');
const crypto = require('crypto');

const STDIO_ID = 'win-exec-mcp';
const HTTP_ID = 'win-http';
const CONSENT_KEY = 'consentGranted';
const EXTERNAL_OPTOUT_KEY = 'externalOptOut'; // 用户“暂不”过的外部配置项（不再自动维护）：{ ssh?: true, project?: true }

let child = null;
let autoToken = null;
let watchdog = null; // 定时检查：服务被其他窗口关闭/崩溃时自动恢复
let outChannel = null;

// 输出面板日志（“输出 → WinExec MCP”），便于排查“到底跑没跑/修没修”
function logMsg(...parts) {
    if (!outChannel) outChannel = vscode.window.createOutputChannel('WinExec MCP');
    outChannel.appendLine('[' + new Date().toISOString().slice(11, 19) + '] ' + parts.join(' '));
}

// 修复了需要重载才生效的配置文件后，合并成一条提示（2 秒窗口内多次修复合并）
let repairTimer = null;
const repairedItems = new Set();
function noteRepaired(what) {
    repairedItems.add(what);
    logMsg('repaired:', what);
    if (repairTimer) clearTimeout(repairTimer);
    repairTimer = setTimeout(() => {
        const list = Array.from(repairedItems).join('、');
        repairedItems.clear();
        vscode.window.showInformationMessage('WinExec MCP: 已修复配置文件（' + list + '），需重载窗口生效', '重载窗口')
            .then((pick) => { if (pick === '重载窗口') vscode.commands.executeCommand('workbench.action.reloadWindow'); });
    }, 2000);
}

function exePath(context) {
    return path.join(context.extensionPath, 'bin', 'win-exec-mcp.exe');
}

// 固定服务路径：%LOCALAPPDATA%\win-exec-mcp\bin\win-exec-mcp.exe
// 为什么：防火墙/安全软件按“程序路径”记忆网络授权（弹窗放行、或某次误答的阻止，都绑定在路径上）。
// 升级=换扩展目录=系统眼中的“全新程序”→ 又要重新弹窗，甚至被静默拦截。
// exe 固定从同一路径运行后：授权/放行一次即永久有效，升级不再产生新的“程序身份”。
function stableExePath() {
    const base = process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
    return path.join(base, 'win-exec-mcp', 'bin', 'win-exec-mcp.exe');
}

// 把内置 exe 同步到固定路径（内容不同才覆盖）。正在运行的旧副本会锁住文件 → 保留旧副本继续用，
// 下次激活再试；任何失败都回退内置路径，保证功能可用。
function ensureStableExe(context) {
    const src = exePath(context);
    const dst = stableExePath();
    try {
        if (!fs.existsSync(src)) return dst;
        const sha = (p) => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
        let needCopy = true;
        try { needCopy = !fs.existsSync(dst) || sha(src) !== sha(dst); } catch (e) { needCopy = true; }
        if (needCopy) {
            fs.mkdirSync(path.dirname(dst), { recursive: true });
            fs.copyFileSync(src, dst);
            logMsg('stable exe updated: ' + dst);
        }
        return dst;
    } catch (e) {
        try { if (fs.existsSync(dst)) { logMsg('stable exe in use / update failed, keep existing: ' + (e && e.message)); return dst; } } catch (e2) { }
        logMsg('stable exe unavailable, fallback to bundled copy: ' + (e && e.message));
        return src;
    }
}

// 服务实际使用的 exe 路径（固定路径优先）
function serviceExe(context) {
    try { return ensureStableExe(context); } catch (e) { return exePath(context); }
}

// 当前宿主应用（VS Code / Trae 等）用户级 mcp.json：从 globalStorageUri 反推用户数据目录
// （<userData>/User/globalStorage/<publisher>.<name> → <userData>/User/mcp.json）。
// 不硬编码 %APPDATA%\Code——VS Code 与 Trae 同装时各写各的 mcp.json，避免互相覆盖引发"用户级 mcp 更改"提示
function userMcpPath(context) {
    return path.join(path.dirname(path.dirname(context.globalStorageUri.fsPath)), 'mcp.json');
}

// 本机局域网 IPv4（项目级注册用——远端发起的连接要走局域网）
// 跳过虚拟/热点网卡：169.254.*（APIPA）、192.168.137.*（Windows 移动热点）；支持 http.host 覆盖
function lanIp() {
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const override = cfg.get('http.host', '');
    if (override) return override;
    const candidates = [];
    for (const list of Object.values(os.networkInterfaces())) {
        for (const ni of list || []) {
            if (ni.family !== 'IPv4' || ni.internal) continue;
            if (ni.address.indexOf('169.254.') === 0) continue;
            if (ni.address.indexOf('192.168.137.') === 0) continue;
            candidates.push(ni.address);
        }
    }
    return candidates[0] || '127.0.0.1';
}

// 识别自家条目：按 win-exec 前缀匹配键名；值兜底再查一次
function isOurEntry(id, entry) {
    if (typeof id === 'string' && id.indexOf('win-exec') !== -1) return true;
    return !!entry && JSON.stringify(entry).indexOf('win-exec') !== -1;
}

function currentToken(cfg) {
    return autoToken || cfg.get('http.token', '');
}

// 项目级 http 条目：默认走 SSH 隧道（127.0.0.1:<ssh.localPort>，IP 无关、可移植）；
// 显式配置 http.host 则改用局域网直连（host:<http.port>）
function projectHttpEntry(cfg) {
    const port = cfg.get('http.port', 38848);
    const localPort = cfg.get('ssh.localPort', 28848);
    const host = cfg.get('http.host', '');
    const url = host
        ? 'http://' + host + ':' + port + '/mcp'
        : 'http://127.0.0.1:' + localPort + '/mcp';
    return {
        type: 'http',
        url: url,
        headers: { Authorization: 'Bearer ' + currentToken(cfg) }
    };
}

// 用户级 mcp.json：只保证 stdio 条目，并清理 http 历史残留
// （http 条目一旦进入用户级，VS Code 的 MCP 网关会绑定其回环端口，遮蔽 exe 并与
//   SSH 隧道/外部客户端冲突——http 只属于项目级注册与外部客户端）
// 容器兼容：VS Code 用 servers；Trae 等 fork 用 Claude Code 风格的 mcpServers——文件里存在哪个就维护哪个
function ensureUserMcp(context, cfg) {
    const p = userMcpPath(context);
    // 文件不存在（全新用户/卸载后重装）不是错误：视为空配置，走下方"按需创建"流程；
    // 仅"读到了内容却解析失败"（可能含注释）才提示手动配置——避免把 ENOENT 误报成解析失败
    let raw = '';
    try {
        raw = fs.readFileSync(p, 'utf8');
    } catch (e) {
        if (!e || e.code !== 'ENOENT') {
            vscode.window.showErrorMessage('WinExec MCP: 用户级 mcp.json 读取失败：' + p);
            return;
        }
    }
    let doc;
    try {
        doc = raw.trim() ? JSON.parse(raw) : {};
    } catch (e) {
        vscode.window.showErrorMessage('WinExec MCP: 用户级 mcp.json 解析失败（可能含注释），请手动配置：' + p);
        return;
    }
    if (!doc.servers && !doc.mcpServers) {
        // 新建/无容器：VS Code 用 servers；Trae 等 Claude Code 风格 fork 用 mcpServers
        if (/trae/i.test(vscode.env.appName || '')) doc.mcpServers = {};
        else doc.servers = {};
    }
    const containers = ['servers', 'mcpServers'].filter((k) => doc[k]);
    let changed = false;
    if (cfg.get('stdio.enabled', false)) {
        for (const k of containers) {
            const want = k === 'mcpServers'
                ? { command: serviceExe(context), args: [] }
                : { type: 'stdio', command: serviceExe(context), args: [], location: 'local' };
            if (JSON.stringify(doc[k][STDIO_ID]) !== JSON.stringify(want)) {
                doc[k][STDIO_ID] = want;
                changed = true;
            }
        }
    } else {
        for (const k of containers) {
            if (doc[k][STDIO_ID] && isOurEntry(STDIO_ID, doc[k][STDIO_ID])) {
                delete doc[k][STDIO_ID];
                changed = true;
            }
        }
    }
    for (const k of containers) {
        if (doc[k][HTTP_ID] && isOurEntry(HTTP_ID, doc[k][HTTP_ID])) {
            delete doc[k][HTTP_ID];
            changed = true;
        }
    }
    if (!changed) return;
    fs.writeFileSync(p, JSON.stringify(doc, null, '\t'));
    vscode.window.showInformationMessage('WinExec MCP: 用户级 mcp.json 已更新，Reload Window 后生效', 'Reload Window')
        .then((pick) => { if (pick) vscode.commands.executeCommand('workbench.action.reloadWindow'); });
}

// 当前窗口是否 SSH 远端（VS Code 1.138 起 remoteName='ssh-remote'，旧版为 'ssh-remote+host'，都兼容）
function isSshRemote() {
    return !!vscode.env.remoteName && vscode.env.remoteName.indexOf('ssh-remote') === 0;
}

// SSH 主机名：优先取工作区 URI authority（ssh-remote+host），remoteName 带主机时兜底；取不到返回 ''
function sshHost() {
    const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    const auth = (folder && folder.uri && folder.uri.authority) || '';
    const m = auth.match(/^ssh-remote\+(.+)$/);
    if (m) return m[1];
    const m2 = (vscode.env.remoteName || '').match(/^ssh-remote\+(.+)$/);
    return m2 ? m2[1] : '';
}

// RemoteForward 现状：ok=已正确配置；conflict=同一入口端口已有指向别处的转发（不静默改）；
// missing=没有（可安全追加）。按 Host 段判定：name 匹配当前主机或 * 才生效（无 Host 段的全局行视为生效）
function sshForwardStatus(cfg, content) {
    const localPort = cfg.get('ssh.localPort', 28848);
    const port = cfg.get('http.port', 38848);
    const host = sshHost(); // 1.138：remoteName 无主机后缀，统一走 sshHost（工作区 URI authority）
    if (content === undefined) {
        try { content = fs.readFileSync(path.join(os.homedir(), '.ssh', 'config'), 'utf8'); } catch (e) { return 'missing'; }
    }
    const exact = new RegExp('^\\s*RemoteForward\\s+(?:127\\.0\\.0\\.1:)?' + localPort + '\\s+(?:127\\.0\\.0\\.1|localhost):' + port + '\\s*(?:#.*)?$');
    const anyLocal = new RegExp('^\\s*RemoteForward\\b[^\\r\\n#]*(?<![0-9])' + localPort + '(?![0-9])');
    const applies = (ctx) => !ctx || ctx.split(/\s+/).some((n) => n === '*' || n.toLowerCase() === host.toLowerCase());
    let hostCtx = null, ok = false, conflict = false;
    for (const line of content.split(/\r?\n/)) {
        const hm = line.match(/^\s*Host\s+(.+?)\s*$/i);
        if (hm) { hostCtx = hm[1]; continue; }
        if (!applies(hostCtx)) continue;
        if (exact.test(line)) ok = true;
        else if (anyLocal.test(line)) conflict = true;
    }
    return ok ? 'ok' : (conflict ? 'conflict' : 'missing');
}

// 远端窗口：自动在 ~/.ssh/config 追加 RemoteForward，让服务器本机工具经回环够到 Windows
// 只追加独立 Host 块（不改现有内容），先备份；下次 SSH 连接生效
function ensureSshForward(context) {
    if (!isSshRemote()) return;
    const host = sshHost();
    if (!host) return;
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    if (!cfg.get('http.enabled', false)) return;
    // 已授权即维护；用户“暂不”过（internal opt-out）则不再动
    if (context && (context.globalState.get(EXTERNAL_OPTOUT_KEY, {}) || {}).ssh) return;
    const port = cfg.get('http.port', 38848);
    const localPort = cfg.get('ssh.localPort', 28848);
    const sshDir = path.join(os.homedir(), '.ssh');
    const sshConfig = path.join(sshDir, 'config');
    try {
        let content = '';
        try { content = fs.readFileSync(sshConfig, 'utf8'); } catch (e) { content = ''; }
        const status = sshForwardStatus(cfg, content);
        if (status !== 'missing') return; // ok=已配置；conflict=同入口端口已有其它转发，不静默改（体检会提示）
        const block = '\nHost ' + host + '\n  RemoteForward 127.0.0.1:' + localPort + ' 127.0.0.1:' + port + '\n';
        if (content.indexOf(block) !== -1) return; // 防重复追加（即使状态判定异常，同一块也不会堆叠）
        if (!fs.existsSync(sshDir)) fs.mkdirSync(sshDir, { recursive: true });
        if (fs.existsSync(sshConfig)) {
            try { fs.copyFileSync(sshConfig, sshConfig + '.bak-winexec'); } catch (e) { }
        }
        fs.appendFileSync(sshConfig, block);
        noteRepaired('SSH RemoteForward 127.0.0.1:' + localPort + ' → 127.0.0.1:' + port);
    } catch (e) {
        // ssh config 读写受限时静默跳过（可手动配置）
    }
}

// 关闭 VS Code 对指定端口的自动转发：RemoteForward 让远端出现监听后，autoForwardPorts 会把它
// 转发回客户端并绑定同名回环口，绕路甚至形成转发环路；若与 exe 的端口重合还会遮蔽 exe
// （Windows 特定回环绑定优先于通配绑定）。入参必须是"远端出现监听的那个端口"= ssh.localPort
// 注意：portsAttributes 是读-改-写，多端口必须一次传入（多次调用会因异步落盘相互覆盖）
function ensurePortsIgnore(...ports) {
    try {
        const rcfg = vscode.workspace.getConfiguration('remote');
        const attrs = rcfg.get('portsAttributes', {});
        let changed = false;
        for (const port of ports) {
            const key = String(port);
            if (attrs[key] && attrs[key].onAutoForward === 'ignore') continue;
            attrs[key] = { onAutoForward: 'ignore' };
            changed = true;
        }
        if (!changed) return;
        rcfg.update('portsAttributes', attrs, vscode.ConfigurationTarget.Global);
    } catch (e) { }
}

// 远端窗口：自动启动 http MCP 连接（等价替用户按 Start；定义异步加载，多轮重试）
function autoStartHttpConnection() {
    if (!vscode.env.remoteName) return;
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    if (!cfg.get('http.enabled', false)) return;
    [5000, 15000, 30000].forEach((delay) => {
        setTimeout(() => {
            // '*' = 启动全部服务器：definition id 是复合 ID（集合URI:键名），硬编码键名匹配不到
            vscode.commands.executeCommand('workbench.mcp.startServer', '*', { waitForLiveTools: true })
                .catch(() => { });
        }, delay);
    });
}

// 项目级 .vscode/mcp.json 的 http 注册/移除（workspace.fs 对远端工作区同样生效）
function workspaceMcpPaths() {
    const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (!folder) return null;
    const dir = vscode.Uri.joinPath(folder.uri, '.vscode');
    return { dir, file: vscode.Uri.joinPath(dir, 'mcp.json') };
}

// 比较两个 http 条目是否相同（避免重复写入/提示）
function sameHttpEntry(a, b) {
    return !!a && !!b && a.url === b.url &&
        !!a.headers && !!b.headers && a.headers.Authorization === b.headers.Authorization;
}

async function registerProject() {
    const paths = workspaceMcpPaths();
    if (!paths) { vscode.window.showErrorMessage('WinExec MCP: 当前没有打开的工作区'); return; }
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const entry = projectHttpEntry(cfg);
    const mEntry = { type: 'http', url: entry.url, headers: entry.headers };
    let changed = false;

    // 1) .vscode/mcp.json（VS Code servers 格式）——幂等：已有且相同则跳过
    // 不存在（FileNotFound）与空文件都视为空配置，直接走创建流程；仅"有内容但解析失败"才提示手动添加
    let doc = {};
    try {
        const text = new TextDecoder().decode(await vscode.workspace.fs.readFile(paths.file)).trim();
        if (text) doc = JSON.parse(text);
    } catch (e) {
        if (e && e.code !== 'FileNotFound') {
            vscode.window.showErrorMessage('WinExec MCP: ' + paths.file.fsPath + ' 解析失败（可能含注释），请手动添加：\n' + JSON.stringify({ [HTTP_ID]: entry }, null, 2));
            return;
        }
    }
    if (!doc.servers) doc.servers = {};
    if (!sameHttpEntry(doc.servers[HTTP_ID], entry)) {
        doc.servers[HTTP_ID] = entry;
        await vscode.workspace.fs.createDirectory(paths.dir);
        await vscode.workspace.fs.writeFile(paths.file, new TextEncoder().encode(JSON.stringify(doc, null, '\t')));
        changed = true;
    }

    // 2) 项目根 .mcp.json（Claude Code mcpServers 格式，VS Code 1.102+ 也兼容）——幂等
    // 与上同理：不存在/空文件按空配置处理
    const rootFile = vscode.Uri.joinPath(paths.dir, '..', '.mcp.json');
    let mdoc = {};
    try {
        const text = new TextDecoder().decode(await vscode.workspace.fs.readFile(rootFile)).trim();
        if (text) mdoc = JSON.parse(text);
    } catch (e) {
        if (e && e.code !== 'FileNotFound') {
            vscode.window.showErrorMessage('WinExec MCP: ' + rootFile.fsPath + ' 解析失败（可能含注释），请手动添加');
            return;
        }
    }
    if (!mdoc.mcpServers) mdoc.mcpServers = {};
    if (!sameHttpEntry(mdoc.mcpServers['win-exec-mcp'], mEntry)) {
        mdoc.mcpServers['win-exec-mcp'] = mEntry;
        await vscode.workspace.fs.writeFile(rootFile, new TextEncoder().encode(JSON.stringify(mdoc, null, '\t')));
        changed = true;
    }

    if (changed) {
        noteRepaired('项目注册（.vscode/mcp.json + .mcp.json）');
        setTimeout(autoStartHttpConnection, 3000);
    }
}

async function unregisterProject() {
    const paths = workspaceMcpPaths();
    if (!paths) return;
    let removed = false;
    // 清理 .vscode/mcp.json——不存在/空文件/坏 JSON 都跳过，不影响下一处
    try {
        const text = new TextDecoder().decode(await vscode.workspace.fs.readFile(paths.file)).trim();
        if (text) {
            const doc = JSON.parse(text);
            if (doc.servers && doc.servers[HTTP_ID]) {
                delete doc.servers[HTTP_ID];
                await vscode.workspace.fs.writeFile(paths.file, new TextEncoder().encode(JSON.stringify(doc, null, '\t')));
                removed = true;
            }
        }
    } catch (e) { }
    // 同步清理项目根 .mcp.json——独立处理：一个文件缺失不影响另一个的清理
    const rootFile = vscode.Uri.joinPath(paths.dir, '..', '.mcp.json');
    try {
        const text = new TextDecoder().decode(await vscode.workspace.fs.readFile(rootFile)).trim();
        if (text) {
            const mdoc = JSON.parse(text);
            if (mdoc.mcpServers && mdoc.mcpServers['win-exec-mcp']) {
                delete mdoc.mcpServers['win-exec-mcp'];
                await vscode.workspace.fs.writeFile(rootFile, new TextEncoder().encode(JSON.stringify(mdoc, null, '\t')));
                removed = true;
            }
        }
    } catch (e) { }
    vscode.window.showInformationMessage(removed ? 'WinExec MCP: 已从本项目移除注册' : 'WinExec MCP: 本项目未找到注册记录');
}

function isPortListening(port) {
    return new Promise((resolve) => {
        const s = net.connect(port, '127.0.0.1');
        s.on('connect', () => { s.destroy(); resolve(true); });
        s.on('error', () => { resolve(false); });
    });
}

// 连接探测：ok=可连；refused=端口无人监听；timeout=SYN 被静默丢弃（防火墙/安全软件拦截的典型特征）
function probePort(port, timeoutMs) {
    return new Promise((resolve) => {
        const s = net.connect(port, '127.0.0.1');
        let done = false;
        const finish = (r) => { if (!done) { done = true; try { s.destroy(); } catch (e) { } resolve(r); } };
        s.setTimeout(timeoutMs || 1500, () => finish('timeout'));
        s.on('connect', () => finish('ok'));
        s.on('error', () => finish('refused'));
    });
}

let httpSpawnFails = 0;   // 连续启动失败次数（退避用，避免 watchdog 反复堆进程）
let httpNextTryAt = 0;    // 退避期内不允许再次启动的时间戳
let blockedWarned = false; // “回环被拦截”的告警只提示一次（连接成功后复位）

function startHttp(context) {
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const port = cfg.get('http.port', 38848);
    return isPortListening(port).then((listening) => {
        if (listening) { httpSpawnFails = 0; httpNextTryAt = 0; return; }
        // 已有实例还活着（可能只是正忙/暂不可连）：先等它，绝不再叠加新进程
        if (child && child.exitCode === null) return;
        if (Date.now() < httpNextTryAt) return;
        const exe = serviceExe(context);
        if (!fs.existsSync(exe)) {
            vscode.window.showErrorMessage('WinExec MCP: 内置 exe 缺失 ' + exe);
            return;
        }
        const args = ['--http', String(port), '--token', currentToken(cfg), '--parent-pid', String(process.pid)];
        const bindHost = cfg.get('http.host', '');
        if (bindHost) args.push('--bind', bindHost); // 默认 exe 只绑 127.0.0.1；显式配置局域网 IP 时才绑它
        child = spawn(exe, args, { windowsHide: true, stdio: 'ignore' });
        child.on('error', (err) => vscode.window.showErrorMessage('WinExec MCP 启动失败: ' + err.message));
        child.on('exit', () => { child = null; });
        // 3 秒后复查：ok=正常；refused=进程没起来（退避重试）；timeout=回环被拦截（提示一键修复）
        setTimeout(() => {
            probePort(port, 1500).then((r) => {
                if (r === 'ok') { httpSpawnFails = 0; httpNextTryAt = 0; blockedWarned = false; return; }
                if (r === 'timeout') noteFirewallBlocked(context, port);
                httpSpawnFails++;
                httpNextTryAt = Date.now() + Math.min(15000 * Math.pow(2, Math.min(httpSpawnFails - 1, 3)), 120000);
                logMsg('startHttp: port ' + port + ' probe=' + r + ' after spawn (fail #' + httpSpawnFails + '), backoff');
                if (httpSpawnFails === 5 && r !== 'timeout') {
                    vscode.window.showWarningMessage('WinExec MCP: HTTP 服务多次启动后仍不可连（端口 ' + port + '），已放慢重试；详见“输出 → WinExec MCP”');
                }
            });
        }, 3000);
    });
}

function stopHttp() {
    if (child) { child.kill(); child = null; }
}

// 回环连接超时 = 本机防火墙/安全软件把该程序的入站连接静默丢弃的典型特征（连 127.0.0.1 都连不上）。
// 提示一次，并提供一键修复（清除 win-exec 阻止规则 + 放行固定路径 + 清理残留进程；需管理员/UAC）
function noteFirewallBlocked(context, port) {
    if (blockedWarned) return;
    blockedWarned = true;
    logMsg('probe timeout on 127.0.0.1:' + port + ' - likely blocked by firewall / security suite');
    vscode.window.showWarningMessage(
        'WinExec MCP: 服务端口 ' + port + ' 连接超时——本机防火墙/安全软件拦截了 win-exec-mcp（连本机回环都被丢包）。可运行一键修复。',
        '运行修复（需管理员）', '打开输出'
    ).then((pick) => {
        if (pick === '运行修复（需管理员）') runFirewallFix(context);
        else if (pick === '打开输出' && outChannel) outChannel.show();
    });
}

// 以管理员身份运行随扩展分发的 fix-firewall.ps1（弹 UAC；脚本会清除阻止规则、放行固定路径、清理残留进程）
function runFirewallFix(context) {
    const script = path.join(context.extensionPath, 'fix-firewall.ps1');
    if (!fs.existsSync(script)) {
        vscode.window.showErrorMessage('WinExec MCP: 修复脚本缺失 ' + script);
        return;
    }
    const psArg = '-NoProfile -ExecutionPolicy Bypass -File "' + script.replace(/"/g, '""') + '"';
    const psCmd = "Start-Process -FilePath powershell.exe -Verb RunAs -ArgumentList '" + psArg.replace(/'/g, "''") + "'";
    try {
        const p = spawn('powershell.exe', ['-NoProfile', '-Command', psCmd], { windowsHide: true, stdio: 'ignore' });
        p.on('error', (err) => vscode.window.showErrorMessage('WinExec MCP: 无法启动修复脚本: ' + err.message));
        vscode.window.showInformationMessage('WinExec MCP: 正在请求管理员权限运行修复（请在 UAC 弹窗点“是”）；完成后重载窗口生效', '重载窗口')
            .then((pick) => { if (pick === '重载窗口') vscode.commands.executeCommand('workbench.action.reloadWindow'); });
    } catch (e) {
        vscode.window.showErrorMessage('WinExec MCP: 无法启动修复脚本: ' + e.message);
    }
}

// 用户是否已同意：显式点击过同意，或手动开启了 stdio/http 开关（开关本身就是明确同意）
function consented(context, cfg) {
    if (context.globalState.get(CONSENT_KEY) === true) return true;
    return !!cfg.get('stdio.enabled', false) || !!cfg.get('http.enabled', false);
}

// 服务器端 agent（Claude Code / Cursor 等）完整链路：HTTP 服务 + SSH 回环转发 + 项目注册。
// 显式触发（授权弹窗“启用”/命令）即视为对 ~/.ssh/config 与项目文件写入的明确同意
async function enableExternalSetup(context) {
    if (context) await context.globalState.update(EXTERNAL_OPTOUT_KEY, {}); // 授权“启用”/完整配置/命令 = 重新启用全部，清掉历史的“暂不”记录
    logMsg('enableExternalSetup: clear opt-out');
    let cfg = vscode.workspace.getConfiguration('winExecMcp');
    if (!cfg.get('http.enabled', false)) {
        await cfg.update('http.enabled', true, vscode.ConfigurationTarget.Global);
    }
    // 直接执行一轮修复（外部配置已无开关：未 opt-out 即维护）
    ensureSshForward(context);
    const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (vscode.env.remoteName && folder) {
        registerProject().catch(() => { });
    }
    // 开关变化触发 onDidChangeConfiguration → autoSetup：远端窗口随即追加 RemoteForward 并注册项目文件
    if (!vscode.env.remoteName) {
        vscode.window.showInformationMessage('WinExec MCP: 已开启服务器端 agent 配置（HTTP 服务 + SSH 回环转发 + 项目注册），Remote-SSH 窗口打开时自动生效');
    }
}

// 首次运行征得同意：明确列出将修改的文件；“完整配置”额外覆盖服务器端 agent 链路
const CONSENT_FULL = '完整配置（含服务器端 agent）';
const CONSENT_BASIC = '仅 VS Code';

async function askConsent(context) {
    const pick = await vscode.window.showWarningMessage(
        'WinExec MCP 需要你的同意才会修改配置，请选择范围：\n' +
        '· ' + CONSENT_FULL + '：注册用户级 mcp.json（stdio）；Remote-SSH 窗口自动启动本机 HTTP 服务并连接；追加 ~/.ssh/config（独立 Host 块，原文件先备份）；向当前项目写入 .vscode/mcp.json 和 .mcp.json（含 Bearer token）——服务器端 Claude Code / Cursor 等经 SSH 回环直连\n' +
        '· ' + CONSENT_BASIC + '：只做前两项，VS Code 内建 agent 即可使用；外部 agent 之后可随时用命令 “WinExec MCP: 配置服务器端 agent” 一键补齐',
        { modal: true }, CONSENT_FULL, CONSENT_BASIC, '暂不');
    if (pick !== CONSENT_FULL && pick !== CONSENT_BASIC) {
        await context.globalState.update(CONSENT_KEY, false);
        return false;
    }
    await context.globalState.update(CONSENT_KEY, true);
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    await cfg.update('stdio.enabled', true, vscode.ConfigurationTarget.Global);
    await cfg.update('http.enabled', true, vscode.ConfigurationTarget.Global);
    if (pick === CONSENT_FULL) {
        await enableExternalSetup(context); // 完整配置：外部 agent 链路全开
    } else {
        // 仅 VS Code：明确不要外部配置（不再自动启用；命令可随时补齐）
        await context.globalState.update(EXTERNAL_OPTOUT_KEY, { ssh: true, project: true });
    }
    logMsg('consent choice:', pick);
    return true;
}

// —— 外部 agent 配置巡检（远端窗口每次激活执行）——
// 唯一提示 = 授权提示（启用 / 暂不）；已授权后缺失/过期直接修复（ensureSshForward /
// registerProject + noteRepaired），修不了的（端口冲突 / 文件解析失败）才报错

// 项目两级 mcp 文件现状：缺条目/条目过期=可自动重写；解析失败=需手动处理
async function projectFilesStatus(cfg) {
    const paths = workspaceMcpPaths();
    if (!paths) return 'na';
    const entry = projectHttpEntry(cfg);
    const mEntry = { type: 'http', url: entry.url, headers: entry.headers };
    const rank = { ok: 0, missing: 1, wrong: 2, broken: 3 };
    let worst = 'ok';
    const bump = (s) => { if (rank[s] > rank[worst]) worst = s; };
    const readJson = async (uri) => {
        const text = new TextDecoder().decode(await vscode.workspace.fs.readFile(uri)).trim();
        return text ? JSON.parse(text) : {};
    };
    try {
        const doc = await readJson(paths.file);
        const cur = doc.servers && doc.servers[HTTP_ID];
        if (!cur) bump('missing');
        else if (!sameHttpEntry(cur, entry)) bump('wrong');
    } catch (e) {
        bump(e && e.code === 'FileNotFound' ? 'missing' : 'broken');
    }
    try {
        const mdoc = await readJson(vscode.Uri.joinPath(paths.dir, '..', '.mcp.json'));
        const cur = mdoc.mcpServers && mdoc.mcpServers['win-exec-mcp'];
        if (!cur) bump('missing');
        else if (!sameHttpEntry(cur, mEntry)) bump('wrong');
    } catch (e) {
        bump(e && e.code === 'FileNotFound' ? 'missing' : 'broken');
    }
    return worst;
}

// —— 外部 agent 配置巡检（远端窗口每次激活）——
// 外部配置已无开关：点过同意（含旧版本升级）且未“暂不”即自动维护（缺失补写、过期重写）；
// “暂不”记录于 globalState；修不了的（冲突/解析失败）才报错
async function externalSetupSweep(context) {
    if (!isSshRemote()) return;
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const optOut = context.globalState.get(EXTERNAL_OPTOUT_KEY, {}) || {};
    const consentGranted = context.globalState.get(CONSENT_KEY) === true;
    const wantSsh = !optOut.ssh;
    const wantProject = !optOut.project;
    logMsg('sweep: consent=' + consentGranted + ' ssh=' + wantSsh + ' project=' + wantProject);
    // 没走过同意弹窗（仅手动开了部分开关）：提示一次授权
    if (!consentGranted) {
        if (!wantSsh && !wantProject) return;
        const hasFolder = !!(vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length);
        const pick = await vscode.window.showWarningMessage(
            'WinExec MCP: 服务器端 agent（Claude Code / Cursor 等）配置未启用（SSH 回环转发 / 项目注册），需要授权' +
            (hasFolder ? '' : '。当前未打开项目，项目注册会在打开后自动完成') +
            '。授权后每次启动自动检查并修复，修复后提示重载生效',
            '启用', '暂不');
        if (pick === '启用') {
            await enableExternalSetup(context);
        } else if (pick === '暂不') {
            await context.globalState.update(EXTERNAL_OPTOUT_KEY, { ssh: true, project: true });
        }
        return;
    }
    // 已授权：检查 + 兜底修复；只报告修不了的（冲突 / 解析失败 / 写入失败）
    const problems = [];
    if (wantSsh && cfg.get('http.enabled', false)) {
        if (sshForwardStatus(cfg) !== 'ok') {
            ensureSshForward(context); // 缺失则补写；conflict 时不碰（下面报告）
            const s = sshForwardStatus(cfg);
            if (s === 'conflict') problems.push('~/.ssh/config 同一入口端口已有其它 RemoteForward，需手动调整');
            else if (s !== 'ok' && sshHost()) problems.push('无法写入 ~/.ssh/config（检查文件权限）');
        }
    }
    if (wantProject) {
        let st = await projectFilesStatus(cfg);
        if (st === 'missing' || st === 'wrong') {
            try { await registerProject(); } catch (e) { }
            st = await projectFilesStatus(cfg);
        }
        const stName = { missing: '条目缺失', wrong: '条目过期' };
        if (st === 'broken') problems.push('项目 mcp 文件解析失败（可能含注释），需手动处理');
        else if (st !== 'ok' && st !== 'na') problems.push('无法写入项目 mcp 文件（' + (stName[st] || st) + '）');
    }
    if (problems.length) {
        logMsg('sweep problems: ' + problems.join('；'));
        vscode.window.showWarningMessage('WinExec MCP: ' + problems.join('；'));
    } else {
        logMsg('sweep: ok');
    }
}

// 同意后的自动配置（幂等，配置变化时可重复执行）
function autoSetup(context) {
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const optOut = context.globalState.get(EXTERNAL_OPTOUT_KEY, {}) || {};
    ensureUserMcp(context, cfg);
    if (vscode.env.remoteName && cfg.get('http.enabled', false)) {
        setTimeout(() => startHttp(context), 3000);
        // watchdog：服务被其他窗口关闭/崩溃时自动恢复（多窗口场景保证可用）
        if (watchdog) clearInterval(watchdog);
        watchdog = setInterval(() => {
            const c2 = vscode.workspace.getConfiguration('winExecMcp');
            if (!c2.get('http.enabled', false)) return;
            const port = c2.get('http.port', 38848);
            isPortListening(port).then((listening) => {
                if (!listening) startHttp(context);
            });
        }, 15000);
    }
    autoStartHttpConnection();
    if (!optOut.ssh) ensureSshForward(context);
    // 远端被 autoForward 误转发的端口是 RemoteForward 的入口端（ssh.localPort），不是 exe 的 http.port；
    // http.port 一并忽略是兜底：localPort 若配成与 exe 相同端口，转发监听会遮蔽 exe
    ensurePortsIgnore(cfg.get('ssh.localPort', 28848), cfg.get('http.port', 38848));
    // 远端窗口自动注册到项目级 .vscode/mcp.json（“暂不”过则不动）
    // 未打开项目（无工作区）时跳过——不报错；打开/添加项目后由 workspaceFolders 变化事件补注册
    // 无条件写条目（声明式配置：服务暂时不可用时 VS Code 会自行重试，服务恢复即可用）
    const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (!optOut.project && vscode.env.remoteName && folder) {
        setTimeout(() => registerProject().catch(() => { }), 4000);
    }
}

function activate(context) {
    outChannel = vscode.window.createOutputChannel('WinExec MCP');
    context.subscriptions.push(outChannel);
    logMsg('activate v' + context.extension.packageJSON.version + (vscode.env.remoteName ? ' remote=' + vscode.env.remoteName : ' local'));
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    // 迁移：0.3.5 起移除 ssh.autoForward / project.autoRegister 两个设置，
    // 曾显式设为 false 的转为内部“不启用”记录（避免把用户主动关过的功能又静默打开）
    try {
        const optOut = Object.assign({}, context.globalState.get(EXTERNAL_OPTOUT_KEY, {}) || {});
        let migrated = false;
        for (const [key, setting] of [['ssh', 'ssh.autoForward'], ['project', 'project.autoRegister']]) {
            const v = cfg.get(setting);
            if (v === false && !optOut[key]) { optOut[key] = true; migrated = true; }
            else if (v === true && optOut[key]) { delete optOut[key]; migrated = true; }
        }
        if (migrated) {
            context.globalState.update(EXTERNAL_OPTOUT_KEY, optOut);
            logMsg('migrated legacy switches → optOut=' + JSON.stringify(optOut));
        }
    } catch (e) { }
    // token 留空时自动生成并持久化（默认 test123；显式留空才触发自动生成）
    if (!cfg.get('http.token', '')) {
        autoToken = context.globalState.get('httpToken') || crypto.randomBytes(8).toString('hex');
        context.globalState.update('httpToken', autoToken);
    }
    context.subscriptions.push(
        vscode.commands.registerCommand('winExecMcp.startHttp', () => startHttp(context)),
        vscode.commands.registerCommand('winExecMcp.stopHttp', stopHttp),
        // 显式命令 = 明确同意：先打开开关（触发配置变化处理器完成写入）
        vscode.commands.registerCommand('winExecMcp.register', async () => {
            if (!cfg.get('stdio.enabled', false)) {
                await cfg.update('stdio.enabled', true, vscode.ConfigurationTarget.Global);
            }
            ensureUserMcp(context, vscode.workspace.getConfiguration('winExecMcp'));
        }),
        vscode.commands.registerCommand('winExecMcp.registerProject', registerProject),
        vscode.commands.registerCommand('winExecMcp.unregisterProject', unregisterProject),
        // 一键配置服务器端 agent 链路：等价于首次弹窗选“完整配置”
        vscode.commands.registerCommand('winExecMcp.setupExternal', () => enableExternalSetup(context)),
        // 一键修复防火墙（管理员）：清除 win-exec 阻止规则、放行固定路径、清理残留进程
        vscode.commands.registerCommand('winExecMcp.fixFirewall', () => runFirewallFix(context)),
        vscode.commands.registerCommand('winExecMcp.setup', async () => {
            if (await askConsent(context)) autoSetup(context);
        })
    );
    // 未同意前不写任何文件、不拉起任何进程：首次运行弹窗征询；同意过或手动开启开关才自动配置
    if (consented(context, cfg)) {
        autoSetup(context);
        // 巡检：远端窗口每次激活都检查——未授权→授权提示；已授权→修复缺失/过期，修好提示重载
        setTimeout(() => externalSetupSweep(context).catch(() => { }), 12000);
    } else if (context.globalState.get(CONSENT_KEY) === undefined) {
        askConsent(context).then((ok) => { if (ok) autoSetup(context); });
    }
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((e) => {
        if (e.affectsConfiguration('winExecMcp')) {
            const c2 = vscode.workspace.getConfiguration('winExecMcp');
            if (!consented(context, c2)) { stopHttp(); return; }
            autoSetup(context);
            if (!c2.get('http.enabled', false)) stopHttp();
        }
    }));
    // 打开/添加项目后补做项目注册（没打开项目时 autoSetup 自动跳过，不报错）
    context.subscriptions.push(vscode.workspace.onDidChangeWorkspaceFolders(() => autoSetup(context)));
}

function deactivate() {
    if (watchdog) { clearInterval(watchdog); watchdog = null; }
    stopHttp();
}

module.exports = { activate, deactivate };
