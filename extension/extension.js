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
let gContext = null; // activate 存下的 context（命令/事件回调里需要 globalState 时用）
let statusItem = null; // 状态栏项（健康 + 活动：单一项，用底色区分，不换图标不加项）
let busyTool = ''; // 最近一次 [call] 的工具名（仅供 tooltip；状态栏文本里不出现文字/数字）
let busyCount = 0; // 在飞的 tools/call 数（[call]/[done] 配对计数，并行调用不会被先回来的 [done] 提前清掉）
let hot = false; // 每批输出翻转一次（zap ⇄ sparkle）；静默 400ms 后回到安静的闪电
let hotTimer = null; // 回落定时器（每次 [act] 重置 ⇒ 密集输出时是常亮，不会被节流成顿闪）
let sigStats = { call: 0, act: 0, done: 0 }; // 收到的信号计数（只进诊断输出，不写日志）
let httpState = 'down'; // down | starting | live
let staleExeWarned = false;

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
        const list = Array.from(repairedItems).join(vscode.l10n.t(', '));
        repairedItems.clear();
        const reloadLabel = vscode.l10n.t('Reload Window');
        vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('Repaired configuration files ({0}); reload the window to apply', list), reloadLabel)
            .then((pick) => { if (pick === reloadLabel) vscode.commands.executeCommand('workbench.action.reloadWindow'); });
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
        try {
            if (fs.existsSync(dst)) {
                logMsg('stable exe in use / update failed, keep existing: ' + (e && e.message));
                if (!staleExeWarned) {
                    staleExeWarned = true;
                    vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('the bundled exe was updated but the running copy is locked; reload the window to switch to it'), vscode.l10n.t('Reload Window'))
                        .then((pick) => { if (pick) vscode.commands.executeCommand('workbench.action.reloadWindow'); });
                }
                return dst;
            }
        } catch (e2) { }
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

// 用户级 mcp.json：只按 stdio 开关维护 stdio 条目，并清理 http 历史残留
// （http 条目一旦进入用户级，VS Code 的 MCP 网关会绑定其回环端口，遮蔽 exe 并与
//   SSH 隧道/外部客户端冲突——http 只属于项目级注册与外部客户端）
// stdio 通道自 0.6.2 起**默认关闭**：不开启就不会写入任何东西；曾经写入的（老版本同意弹窗
//   自动开的）会被主动清除——否则“默认不带 stdio”的语义会因为残留而失效
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
            vscode.window.showErrorMessage('WinExec MCP: ' + vscode.l10n.t('failed to read the user-level mcp.json: {0}', p));
            return;
        }
    }
    let doc;
    try {
        doc = raw.trim() ? JSON.parse(raw) : {};
    } catch (e) {
        vscode.window.showErrorMessage('WinExec MCP: ' + vscode.l10n.t('failed to parse the user-level mcp.json (it may contain comments), please configure it manually: {0}', p));
        return;
    }
    if (!doc.servers && !doc.mcpServers) {
        // 新建/无容器：VS Code 用 servers；Trae 等 Claude Code 风格 fork 用 mcpServers
        if (/trae/i.test(vscode.env.appName || '')) doc.mcpServers = {};
        else doc.servers = {};
    }
    const containers = ['servers', 'mcpServers'].filter((k) => doc[k]);
    let changed = false;
    // 判定**只看开关**，与窗口类型无关：① 开关为真才写（含 stdio.enabled 显式打开）；
    // ② 开关为假就删掉已有条目。与窗口类型解耦是刻意的：用户级 mcp.json 是所有窗口共用的一份，
    // 若按窗口增删，本地窗口写、SSH 窗口删，同一文件来回拉锯 → 每次激活都 changed → 每次都弹“需重载窗口”。
    // 权衡：开关开着时 SSH 窗口也会注册 stdio（与项目 .mcp.json 的 http 条目重复工具）——见设置说明。
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
            if (doc[k][STDIO_ID]) {
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
    vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('user-level mcp.json updated, takes effect after reloading the window'), vscode.l10n.t('Reload Window'))
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

// SSH 远端窗口：自动启动 http MCP 连接（等价替用户按 Start；定义异步加载，多轮重试）。
// 仅限 SSH：http 入口端口（ssh.localPort）靠 SSH 隧道才存在；WSL 等远程（remoteName='wsl'）没有隧道，
// 写了也连不上（WSL2 与 Windows 网络命名空间隔离）——那种场景只用 location:local 的 stdio 条目
function autoStartHttpConnection() {
    if (!isSshRemote()) return;
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
    if (!paths) { vscode.window.showErrorMessage('WinExec MCP: ' + vscode.l10n.t('no workspace is open')); return; }
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const entry = projectHttpEntry(cfg);
    const mEntry = { type: 'http', url: entry.url, headers: entry.headers };
    let changed = false;

    // 1) 项目根 .mcp.json（Claude Code mcpServers 格式，VS Code 1.102+ 也兼容）——唯一写入点。
    // 不要再写 .vscode/mcp.json：VS Code 两个文件都读且不去重，会得到两份重复的工具
    const rootFile = vscode.Uri.joinPath(paths.dir, '..', '.mcp.json');
    let mdoc = {};
    try {
        const text = new TextDecoder().decode(await vscode.workspace.fs.readFile(rootFile)).trim();
        if (text) mdoc = JSON.parse(text);
    } catch (e) {
        if (e && e.code !== 'FileNotFound') {
            vscode.window.showErrorMessage('WinExec MCP: ' + vscode.l10n.t('failed to parse {0} (it may contain comments), please add manually', rootFile.fsPath));
            return;
        }
    }
    if (!mdoc.mcpServers) mdoc.mcpServers = {};
    if (!sameHttpEntry(mdoc.mcpServers['win-exec-mcp'], mEntry)) {
        mdoc.mcpServers['win-exec-mcp'] = mEntry;
        await vscode.workspace.fs.writeFile(rootFile, new TextEncoder().encode(JSON.stringify(mdoc, null, '\t')));
        changed = true;
    }
    // token 风险提示：项目根 .mcp.json 含 Bearer token，而它通常会被 commit —— 一次性提醒
    try {
        await vscode.workspace.fs.stat(vscode.Uri.joinPath(paths.dir, '..', '.git'));
        if (gContext && !gContext.globalState.get('tokenGitHintShown')) {
            const notAgain = vscode.l10n.t("Don't show again");
            vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('.mcp.json contains a Bearer token and this project looks like a git repository - consider adding .mcp.json to .gitignore or git update-index --skip-worktree'), notAgain)
                .then((p2) => { if (p2 === notAgain) gContext.globalState.update('tokenGitHintShown', true); });
        }
    } catch (e) { } // 无 .git 或 stat 失败：非 git 项目，不提示

    // 2) 清理历史遗留的 .vscode/mcp.json 条目（旧版本写入过 → 与根 .mcp.json 重复）
    try {
        const text = new TextDecoder().decode(await vscode.workspace.fs.readFile(paths.file)).trim();
        if (text) {
            const doc = JSON.parse(text);
            if (doc.servers && doc.servers[HTTP_ID]) {
                delete doc.servers[HTTP_ID];
                await vscode.workspace.fs.writeFile(paths.file, new TextEncoder().encode(JSON.stringify(doc, null, '\t')));
                changed = true;
            }
        }
    } catch (e) { }

    if (changed) {
        noteRepaired(vscode.l10n.t('project registration (.mcp.json)'));
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
    vscode.window.showInformationMessage('WinExec MCP: ' + (removed ? vscode.l10n.t('removed from this project') : vscode.l10n.t('no registration found in this project')));
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

// 端口上跑的到底是不是一个"活的 HTTP 服务"：TCP 连上但迟迟没有响应，就是孤儿/僵尸监听
// （句柄被子进程继承后父进程被杀，端口仍 LISTENING 但无人 accept 应答——探测必须做到 HTTP 层）
// live=有 HTTP 响应；dead=连上但无响应（孤儿）；refused=无人监听
function probeOurHttp(port, timeoutMs) {
    return new Promise((resolve) => {
        const s = net.connect(port, '127.0.0.1');
        let buf = '';
        let done = false;
        const finish = (r) => { if (!done) { done = true; try { s.destroy(); } catch (e) { } resolve(r); } };
        s.setTimeout(timeoutMs || 2000, () => finish(buf ? 'live' : 'dead'));
        s.on('error', () => finish('refused'));
        s.on('connect', () => {
            try { s.write('GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n'); } catch (e) { finish('dead'); }
        });
        s.on('data', (d) => { buf += d; if (/^HTTP\/1\.[01] /.test(buf)) finish('live'); });
        s.on('close', () => finish(buf ? 'live' : 'dead'));
    });
}

let httpSpawnFails = 0;   // 连续启动失败次数（退避用，避免 watchdog 反复堆进程）
let httpNextTryAt = 0;    // 退避期内不允许再次启动的时间戳
let blockedWarned = false; // “回环被拦截”的告警只提示一次（连接成功后复位）
let healingPort = false;  // 端口自愈进行中（防止 watchdog 与 probe 回调反复叠加进程）

// 把指向旧端口的 RemoteForward 行改写到新端口：只动我们写过的形态（入口端口不变、目标端口为旧值），
// 不新加 Host 块，避免每换一次端口就堆积一段
function rewriteSshForwardPort(oldPort, newPort) {
    if (!isSshRemote()) return false;
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const localPort = cfg.get('ssh.localPort', 28848);
    const sshConfig = path.join(os.homedir(), '.ssh', 'config');
    let content;
    try { content = fs.readFileSync(sshConfig, 'utf8'); } catch (e) { return false; }
    const re = new RegExp('^(\\s*RemoteForward\\s+(?:127\\.0\\.0\\.1:)?' + localPort + '\\s+(?:127\\.0\\.0\\.1|localhost):)' + oldPort + '(\\s*(?:#.*)?)$', 'm');
    if (!re.test(content)) return false;
    try {
        try { fs.copyFileSync(sshConfig, sshConfig + '.bak-winexec'); } catch (e) { }
        fs.writeFileSync(sshConfig, content.replace(re, '$1' + newPort + '$2'));
        noteRepaired('SSH RemoteForward 127.0.0.1:' + localPort + ' \u2192 127.0.0.1:' + newPort);
        return true;
    } catch (e) {
        logMsg('rewriteSshForwardPort failed: ' + e.message);
        return false;
    }
}

// 端口连续起不来（被别人占用，或旧实例的残留监听占着）时自愈：换一个空闲端口重启服务，
// 同步改设置与 ~/.ssh/config，再由 sweep 幂等同步项目文件。
// 不做这一步的话，端口一旦被占就永远 bind 不上（SO_EXCLUSIVEADDRUSE），只剩无限退避。
async function trySelfHealPort(context, fromPort) {
    if (healingPort) return;
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    // 显式指定了绑定地址（局域网直连）时端口是用户选定的，不擅自换
    if (cfg.get('http.host', '')) return;
    healingPort = true;
    try {
        const exe = serviceExe(context);
        if (!fs.existsSync(exe)) return;
        logMsg('self-heal: port ' + fromPort + ' unusable, looking for a free port');
        if (child) { try { child.kill(); } catch (e) { } child = null; }
        const token = currentToken(cfg);
        for (let p = fromPort + 1; p <= fromPort + 10; p++) {
            if (await isPortListening(p)) continue;
            const c = spawn(exe, ['--http', String(p), '--parent-pid', String(process.pid)],
                { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, WINEXEC_TOKEN: token } });
            if (c.stdout) c.stdout.on('data', (d) => logMsg('http[out] ' + String(d).replace(/[\r\n]+$/, '')));
            if (c.stderr) c.stderr.on('data', (d) => logMsg('http[err] ' + String(d).replace(/[\r\n]+$/, '')));
            await new Promise((r) => setTimeout(r, 1200));
            if (await probePort(p, 1500) === 'ok') {
                logMsg('self-heal: switched to port ' + p);
                await cfg.update('http.port', p, vscode.ConfigurationTarget.Global);
                rewriteSshForwardPort(fromPort, p);
                // 其他项目的 .mcp.json 不在这里改：各自窗口打开时 sweep 会按新设置重写
                child = c;
                httpSpawnFails = 0; httpNextTryAt = 0; blockedWarned = false;
                try { await externalSetupSweep(context); } catch (e) { logMsg('self-heal sweep failed: ' + e.message); }
                const reload = vscode.l10n.t('Reload Window');
                vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('port {0} is unusable (occupied by another program); switched to {1} and updated the configuration. Reload the window to apply.', String(fromPort), String(p)), reload)
                    .then((pick) => { if (pick === reload) vscode.commands.executeCommand('workbench.action.reloadWindow'); });
                return;
            }
            try { c.kill(); } catch (e) { }
        }
        logMsg('self-heal: no usable port in ' + (fromPort + 1) + '..' + (fromPort + 10));
        vscode.window.showWarningMessage('WinExec MCP: ' + vscode.l10n.t('port {0} is unusable and no alternative port worked - see Output \u2192 WinExec MCP', String(fromPort)));
    } finally {
        healingPort = false;
    }
}

function startHttp(context) {
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const port = cfg.get('http.port', 38848);
    return isPortListening(port).then((listening) => {
        if (listening) {
            // 端口有监听不等于我们的服务在跑（可能是别的程序占了这个端口）：探一次再判定
            if (Date.now() < httpNextTryAt || healingPort) return;
            probeOurHttp(port, 2000).then((r) => {
                if (r === 'live') { setStatus('live'); httpSpawnFails = 0; httpNextTryAt = 0; blockedWarned = false; return; }
                if (r === 'refused') return;
                setStatus('down');
                httpSpawnFails++;
                logMsg('startHttp: port ' + port + ' is listening but not answering as a live HTTP service (probe=' + r + ', fail #' + httpSpawnFails + ')');
                if (httpSpawnFails >= 3) trySelfHealPort(context, port).catch(() => { });
            });
            return;
        }
        // 已有实例还活着（可能只是正忙/暂不可连）：先等它，绝不再叠加新进程
        if (child && child.exitCode === null) return;
        if (Date.now() < httpNextTryAt) return;
        const exe = serviceExe(context);
        if (!fs.existsSync(exe)) {
            vscode.window.showErrorMessage('WinExec MCP: ' + vscode.l10n.t('bundled exe is missing {0}', exe));
            return;
        }
        const args = ['--http', String(port), '--parent-pid', String(process.pid)];
        const bindHost = cfg.get('http.host', '');
        if (bindHost) args.push('--bind', bindHost); // 默认 exe 只绑 127.0.0.1；显式配置局域网 IP 时才绑它
        // 捕获 exe 的输出：关键诊断（bind 失败原因 / 孤儿 socket 提示等）都在 stderr，
        // 原先 stdio:'ignore' 把它们全丢了，故障时只剩 probe=refused 无从定位。
        // token 走环境变量而不是命令行：进程列表/任务管理器里看不到它（--token 仍兼容）
        child = spawn(exe, args, { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, WINEXEC_TOKEN: currentToken(cfg) } });
        setStatus('starting');
        if (child.stderr) child.stderr.on('data', (d) => {
            // 活动信号（[call]/[act]/[done]）只喂状态栏，刻意不写输出通道：忙起来每秒十几条会把日志刷爆。
            // 同批里混着的其它 stderr（绑定失败/孤儿端口等诊断）仍照常记录。
            const s = String(d);
            handleActivitySignals(s);
            const other = s.split('\n').filter((l) => l.trim() && !/^\[(call|act|done)\]/.test(l.trim()));
            if (other.length) logMsg('http[err] ' + other.join(' | '));
        });
        child.on('error', (err) => vscode.window.showErrorMessage(vscode.l10n.t('WinExec MCP failed to start: {0}', err.message)));
        child.on('exit', (code) => { logMsg('http: service exited (code ' + code + ')'); child = null; resetBusy(); refreshStatusBar(); });
        // 3 秒后复查：ok=正常；refused=进程没起来（退避重试）；timeout=回环被拦截（提示一键修复）
        setTimeout(() => {
            probePort(port, 1500).then((r) => {
                if (r === 'ok') { setStatus('live'); httpSpawnFails = 0; httpNextTryAt = 0; blockedWarned = false; return; }
                setStatus('down');
                if (r === 'timeout') noteFirewallBlocked(context, port);
                httpSpawnFails++;
                httpNextTryAt = Date.now() + Math.min(15000 * Math.pow(2, Math.min(httpSpawnFails - 1, 3)), 120000);
                logMsg('startHttp: port ' + port + ' probe=' + r + ' after spawn (fail #' + httpSpawnFails + '), backoff');
                // refused=端口起不来（被占用），换端口能解决；timeout=防火墙拦截回环，换端口无用
                if (r === 'refused' && httpSpawnFails >= 3) { trySelfHealPort(context, port).catch(() => { }); return; }
                if (httpSpawnFails === 5 && r !== 'timeout') {
                    vscode.window.showWarningMessage('WinExec MCP: ' + vscode.l10n.t('the HTTP service still cannot be reached after several attempts (port {0}); retries have been slowed down - see Output → WinExec MCP', String(port)));
                }
            });
        }, 3000);
    });
}

function stopHttp() {
    if (child) { child.kill(); child = null; }
    resetBusy();
    setStatus('down');
}

// —— 状态栏：远端窗口显示服务健康（绿=live / 黄=starting / 红=down），点击出菜单 ——
function refreshStatusBar() {
    const relevant = isSshRemote() || (child && child.exitCode === null);
    if (!relevant) { if (statusItem) statusItem.hide(); return; }
    if (!statusItem) {
        statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 90);
        statusItem.name = 'WinExec MCP';
        statusItem.command = 'winExecMcp.menu';
        if (gContext) gContext.subscriptions.push(statusItem);
    }
    const port = vscode.workspace.getConfiguration('winExecMcp').get('http.port', 38848);
    // 实测（CDP 采样真实 DOM）：影响这一项宽度、进而推动相邻状态栏项的只有底色 ——
    // ① 有/无底色切换：+6px；② 连底色与底色之间切换也会变（warningBackground 那相比 prominentBackground 宽 6px、
    // 相邻项位置移 11px）。而等宽 codicon 之间来回换图标完全不动（连续 3 秒采样零位移）。
    // 所以：底色一律不设（故障态除外），活动只用图标表达 —— 空闲 plug / 忙静默 zap / 有输出 sparkle。
    statusItem.text = (httpState === 'starting'
        ? '$(sync~spin)'
        : (busyCount > 0 ? (hot ? '$(sparkle)' : '$(zap)') : '$(plug)')) + ' win-exec:' + port;
    if (httpState === 'live') {
        statusItem.backgroundColor = undefined;
        statusItem.tooltip = busyCount > 0
            ? vscode.l10n.t('WinExec MCP tool call: {0}', busyTool)
            : vscode.l10n.t('WinExec MCP service is running on port {0}', String(port));
    } else if (httpState === 'starting') {
        statusItem.backgroundColor = undefined;
        statusItem.tooltip = vscode.l10n.t('WinExec MCP service is starting on port {0}', String(port));
    } else {
        // 故障很罕见但重要：保留红底（进出故障各一次宽度变化，可以接受）
        statusItem.backgroundColor = new vscode.ThemeColor('statusBarItem.errorBackground');
        statusItem.tooltip = vscode.l10n.t('WinExec MCP service is down (port {0}); click the status bar item for actions', String(port));
    }
    statusItem.show();
}

function setStatus(state) {
    if (httpState !== state) logMsg('http state: ' + httpState + ' -> ' + state);
    httpState = state;
    refreshStatusBar(); // 不动 busy 状态：watchdog/probe 每 15s 也走到这里，清掉会让进行中的调用指示中途消失
}

// —— 调用活动：exe 在 stderr 上发 [call] 工具名 / [act]（有输出）/ [done]。
// 表现只用图标（不放数字、不用底色 —— 前者会被误读成 token 消耗，后者会推动相邻状态栏项）：
//   忙 = 闪电常驻；每批输出翻转一次（zap ⇄ sparkle）= 跟数据同频闪；静默 400ms 回到安静的闪电。
function handleActivitySignals(s) {
    let hit = false;
    let m;
    const callRe = /\[call\]\s*(\S+)/g;
    while ((m = callRe.exec(s))) { busyTool = m[1]; busyCount++; sigStats.call++; hit = true; }
    if (s.indexOf('[act]') !== -1) {
        // 每批输出翻一次：跟数据同频闪（exe 侧 100ms 节流 ⇒ 上限 10Hz），而不是“有输出就一直亮”
        const n = (s.match(/\[act\]/g) || []).length;
        for (let i = 0; i < n; i++) hot = !hot;
        sigStats.act += n;
        if (hotTimer) clearTimeout(hotTimer);
        hotTimer = setTimeout(() => { hotTimer = null; hot = false; refreshStatusBar(); }, 400);
        hit = true;
    }
    let done = 0;
    const doneRe = /\[done\]/g;
    while (doneRe.exec(s)) done++;
    if (done) {
        busyCount = Math.max(0, busyCount - done);
        sigStats.done += done;
        if (busyCount === 0) resetBusy();
        hit = true;
    }
    if (hit) refreshStatusBar();
}

function resetBusy() {
    busyCount = 0;
    busyTool = '';
    hot = false;
    if (hotTimer) { clearTimeout(hotTimer); hotTimer = null; }
}


async function showWinExecMenu() {
    const t = (k) => vscode.l10n.t(k);
    const running = !!(child && child.exitCode === null);
    const items = [
        { label: '$(server-process) ' + t(running ? 'Stop HTTP service' : 'Start HTTP service'), act: () => (running ? stopHttp() : startHttp(gContext)) },
        { label: '$(info) ' + t('Diagnostics'), act: () => runDiagnostics().catch((e) => logMsg('diagnostics failed: ' + e.message)) },
        { label: '$(copy) ' + t('Copy HTTP token'), act: () => copyToken() },
        { label: '$(shield) ' + t('Run repair (administrator)'), act: () => runFirewallFix(gContext) },
        { label: '$(output) ' + t('Open Output'), act: () => { if (outChannel) outChannel.show(); } },
        { label: '$(link) ' + t('Configure server-side agent (SSH forwarding + project registration)'), act: () => enableExternalSetup(gContext) },
        { label: '$(add) ' + t('Register in this project'), act: () => registerProject().catch(() => { }) },
        { label: '$(remove) ' + t('Remove from this project'), act: () => unregisterProject() },
        { label: '$(key) ' + t('Regenerate token'), act: () => regenerateToken().catch((e) => logMsg('token regen failed: ' + e.message)) }
    ];
    const pick = await vscode.window.showQuickPick(items, { placeHolder: 'WinExec MCP' });
    if (pick && pick.act) pick.act();
}

// —— token 轮换：换随机 token 并重启服务；各项目根 .mcp.json 不在这里改 ——
// （token/端口都是设置驱动，各项目打开时的 externalSetupSweep 会按当前设置重写条目）
async function regenerateToken() {
    if (!gContext) return;
    const token = crypto.randomBytes(8).toString('hex');
    await gContext.globalState.update('httpToken', token);
    logMsg('token regenerated');
    stopHttp();
    startHttp(gContext);
    vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('token regenerated; reopen each registered project to resync its .mcp.json'));
}

async function copyToken() {
    const token = currentToken(vscode.workspace.getConfiguration('winExecMcp'));
    if (!token) { vscode.window.showWarningMessage('WinExec MCP: ' + vscode.l10n.t('no token configured (leave http.token empty to auto-generate one)')); return; }
    await vscode.env.clipboard.writeText(token);
    vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('HTTP token copied to the clipboard'));
}

// —— 一键诊断：把排障需要的全部事实落到输出通道，报障贴一段就够 ——
let diagLines = [];
async function runDiagnostics() {
    diagLines = [];
    const L = (s) => { diagLines.push(s); logMsg('[diag] ' + s); };
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const port = cfg.get('http.port', 38848);
    const exe = serviceExe(gContext);
    L('--- diagnostics begin ---');
    L('extension: ' + (gContext && gContext.extension ? gContext.extension.packageJSON.version : '?') + ', app: ' + (vscode.env.appName || '?'));
    L('remoteName: ' + (vscode.env.remoteName || '(local)') + ', sshHost: ' + (sshHost() || '(n/a)'));
    L('exe: ' + exe + ' exists=' + fs.existsSync(exe));
    L('http: enabled=' + cfg.get('http.enabled', false) + ' port=' + port + ' host=' + (cfg.get('http.host', '') || '(127.0.0.1)'));
    L('port probe: ' + (await probePort(port, 1500)) + ', http probe: ' + (await probeOurHttp(port, 2000)));
    L('spawn fails: ' + httpSpawnFails + ', state: ' + httpState);
    L('child: ' + (child ? ('pid=' + child.pid + ' alive=' + (child.exitCode === null)) : 'none') + ', statusItem=' + (statusItem ? 'created' : 'null'));
    L('activity (ui v3: icon+always-on background): in-flight=' + busyCount + ', tool=' + (busyTool || '-') + ', hot=' + hot + ', signals: call=' + sigStats.call + ' act=' + sigStats.act + ' done=' + sigStats.done);
    try {
        const doc = JSON.parse(fs.readFileSync(userMcpPath(gContext), 'utf8'));
        const has = ['servers', 'mcpServers'].some((k) => doc[k] && doc[k][STDIO_ID]);
        L('user mcp.json: stdio entry=' + (has ? 'yes' : 'no'));
    } catch (e) { L('user mcp.json: unreadable (' + (e && e.message) + ')'); }
    L('project registration: ' + (await projectFilesStatus(cfg)));
    L('ssh forward: ' + sshForwardStatus(cfg));
    L('--- diagnostics end ---');
    if (outChannel) outChannel.show();
    const copyLabel = vscode.l10n.t('Copy to clipboard');
    vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('diagnostics written to the output channel'), copyLabel)
        .then((pick) => { if (pick === copyLabel) vscode.env.clipboard.writeText(diagLines.join('\n')); });
}

// 回环连接超时 = 本机防火墙/安全软件把该程序的入站连接静默丢弃的典型特征（连 127.0.0.1 都连不上）。
// 提示一次，并提供一键修复（清除 win-exec 阻止规则 + 放行固定路径 + 清理残留进程；需管理员/UAC）
function noteFirewallBlocked(context, port) {
    if (blockedWarned) return;
    blockedWarned = true;
    logMsg('probe timeout on 127.0.0.1:' + port + ' - likely blocked by firewall / security suite');
    const fixLabel = vscode.l10n.t('Run repair (administrator)');
    const outputLabel = vscode.l10n.t('Open Output');
    vscode.window.showWarningMessage(
        'WinExec MCP: ' + vscode.l10n.t('connection to port {0} times out - the local firewall / security software is blocking win-exec-mcp (even loopback packets are dropped). A one-click repair is available.', String(port)),
        fixLabel, outputLabel
    ).then((pick) => {
        if (pick === fixLabel) runFirewallFix(context);
        else if (pick === outputLabel && outChannel) outChannel.show();
    });
}

// 以管理员身份运行随扩展分发的 fix-firewall.ps1（弹 UAC；脚本会清除阻止规则、放行固定路径、清理残留进程）
function runFirewallFix(context) {
    const script = path.join(context.extensionPath, 'fix-firewall.ps1');
    if (!fs.existsSync(script)) {
        vscode.window.showErrorMessage('WinExec MCP: ' + vscode.l10n.t('repair script is missing {0}', script));
        return;
    }
    const psArg = '-NoProfile -ExecutionPolicy Bypass -File "' + script.replace(/"/g, '""') + '"';
    const psCmd = "Start-Process -FilePath powershell.exe -Verb RunAs -ArgumentList '" + psArg.replace(/'/g, "''") + "'";
    try {
        const p = spawn('powershell.exe', ['-NoProfile', '-Command', psCmd], { windowsHide: true, stdio: 'ignore' });
        p.on('error', (err) => vscode.window.showErrorMessage('WinExec MCP: ' + vscode.l10n.t('cannot start the repair script: {0}', err.message)));
        const reloadLabel = vscode.l10n.t('Reload Window');
        vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('requesting administrator privileges to run the repair (click Yes in the UAC dialog); reload the window afterwards'), reloadLabel)
            .then((pick) => { if (pick === reloadLabel) vscode.commands.executeCommand('workbench.action.reloadWindow'); });
    } catch (e) {
        vscode.window.showErrorMessage('WinExec MCP: ' + vscode.l10n.t('cannot start the repair script: {0}', e.message));
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
    if (isSshRemote() && folder) {
        registerProject().catch(() => { });
    }
    // 开关变化触发 onDidChangeConfiguration → autoSetup：远端窗口随即追加 RemoteForward 并注册项目文件
    if (!vscode.env.remoteName) {
        vscode.window.showInformationMessage('WinExec MCP: ' + vscode.l10n.t('server-side agent configuration enabled (HTTP service + SSH loopback forwarding + project registration); it takes effect automatically when a Remote-SSH window opens'));
    }
}

// 首次运行征得同意：明确列出将修改的文件；“完整配置”额外覆盖服务器端 agent 链路
// 标签在加载时按当前显示语言取值，且同一语言内比较（用户选择即返回同一字符串）
const CONSENT_FULL = vscode.l10n.t('Full setup (incl. server-side agent)');
const CONSENT_BASIC = vscode.l10n.t('VS Code only');

async function askConsent(context) {
    const pick = await vscode.window.showWarningMessage(
        vscode.l10n.t('WinExec MCP needs your consent before changing any configuration. Choose the scope:\n· {0}: Remote-SSH windows start and connect to the local HTTP service automatically; append to ~/.ssh/config (a separate Host block, the original file is backed up first); write the project-root .mcp.json into the current project (with the Bearer token) - server-side Claude Code / Cursor etc. connect back through the SSH loopback\n· {1}: start and connect the local HTTP service only (enough for the built-in VS Code agent); SSH forwarding and project registration stay untouched and can be added later with the command \'WinExec MCP: Configure server-side agent\'\nNeither choice touches the user-level mcp.json - the stdio channel is OFF by default; enable it explicitly with the command \'WinExec MCP: Register user mcp.json\' (needed for WSL / local windows)', CONSENT_FULL, CONSENT_BASIC),
        { modal: true }, CONSENT_FULL, CONSENT_BASIC, vscode.l10n.t('Not now'));
    if (pick !== CONSENT_FULL && pick !== CONSENT_BASIC) {
        await context.globalState.update(CONSENT_KEY, false);
        return false;
    }
    await context.globalState.update(CONSENT_KEY, true);
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    // 同意 ≠ 注册 stdio：stdio 通道自 0.6.2 起默认关闭，只由显式设置/命令打开（见 ensureUserMcp）
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
        const mdoc = await readJson(vscode.Uri.joinPath(paths.dir, '..', '.mcp.json'));
        const cur = mdoc.mcpServers && mdoc.mcpServers['win-exec-mcp'];
        if (!cur) bump('missing');
        else if (!sameHttpEntry(cur, mEntry)) bump('wrong');
    } catch (e) {
        bump(e && e.code === 'FileNotFound' ? 'missing' : 'broken');
    }
    // 历史遗留：旧版本写进 .vscode/mcp.json 的条目 → 视为过期，触发 registerProject 清理
    try {
        const doc = await readJson(paths.file);
        if (doc.servers && doc.servers[HTTP_ID]) bump('wrong');
    } catch (e) { }
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
        const enableLabel = vscode.l10n.t('Enable');
        const notNowLabel = vscode.l10n.t('Not now');
        const pick = await vscode.window.showWarningMessage(
            'WinExec MCP: ' + vscode.l10n.t('server-side agent (Claude Code / Cursor, etc.) configuration is not enabled (SSH loopback forwarding / project registration) and needs your consent{0}. Once authorized it is checked and repaired automatically on every start, and a reload prompt is shown afterwards',
                hasFolder ? '' : vscode.l10n.t('. No project is open yet; project registration completes automatically once a project is opened')),
            enableLabel, notNowLabel);
        if (pick === enableLabel) {
            await enableExternalSetup(context);
        } else if (pick === notNowLabel) {
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
            if (s === 'conflict') problems.push(vscode.l10n.t('~/.ssh/config already has another RemoteForward on the same entry port; adjust it manually'));
            else if (s !== 'ok' && sshHost()) problems.push(vscode.l10n.t('cannot write ~/.ssh/config (check file permissions)'));
        }
    }
    if (wantProject) {
        let st = await projectFilesStatus(cfg);
        if (st === 'missing' || st === 'wrong') {
            try { await registerProject(); } catch (e) { }
            st = await projectFilesStatus(cfg);
        }
        const stName = { missing: vscode.l10n.t('item missing'), wrong: vscode.l10n.t('item outdated') };
        if (st === 'broken') problems.push(vscode.l10n.t('project mcp file failed to parse (it may contain comments); manual fix required'));
        else if (st !== 'ok' && st !== 'na') problems.push(vscode.l10n.t('cannot write the project mcp file ({0})', stName[st] || st));
    }
    if (problems.length) {
        logMsg('sweep problems: ' + problems.join(vscode.l10n.t('; ')));
        vscode.window.showWarningMessage('WinExec MCP: ' + problems.join(vscode.l10n.t('; ')));
    } else {
        logMsg('sweep: ok');
    }
}

// 同意后的自动配置（幂等，配置变化时可重复执行）
function autoSetup(context) {
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const optOut = context.globalState.get(EXTERNAL_OPTOUT_KEY, {}) || {};
    ensureUserMcp(context, cfg);
    // HTTP 服务只在 SSH 隧道场景有意义（WSL 连不到 Windows 回环，见 autoStartHttpConnection 说明）
    if (isSshRemote() && cfg.get('http.enabled', false)) {
        setTimeout(() => startHttp(context), 3000);
        // watchdog：服务被其他窗口关闭/崩溃时自动恢复（多窗口场景保证可用）
        if (watchdog) clearInterval(watchdog);
        watchdog = setInterval(() => {
            const c2 = vscode.workspace.getConfiguration('winExecMcp');
            if (!c2.get('http.enabled', false)) return;
            const port = c2.get('http.port', 38848);
            probeOurHttp(port, 2000).then((r) => {
                if (r === 'live') { setStatus('live'); return; }
                setStatus('down');
                startHttp(context); // refused=没监听；dead=孤儿监听，startHttp 里的探测+自愈能处理
            });
        }, 15000);
    }
    autoStartHttpConnection();
    if (!optOut.ssh) ensureSshForward(context);
    // 远端被 autoForward 误转发的端口是 RemoteForward 的入口端（ssh.localPort），不是 exe 的 http.port；
    // http.port 一并忽略是兜底：localPort 若配成与 exe 相同端口，转发监听会遮蔽 exe
    ensurePortsIgnore(cfg.get('ssh.localPort', 28848), cfg.get('http.port', 38848));
    // SSH 远端窗口自动注册到项目级 .vscode/mcp.json（“暂不”过则不动）
    // 未打开项目（无工作区）时跳过——不报错；打开/添加项目后由 workspaceFolders 变化事件补注册
    // 无条件写条目（声明式配置：服务暂时不可用时 VS Code 会自行重试，服务恢复即可用）
    const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (!optOut.project && isSshRemote() && folder) {
        setTimeout(() => registerProject().catch(() => { }), 4000);
    }
}

function activate(context) {
    gContext = context;
    outChannel = vscode.window.createOutputChannel('WinExec MCP');
    context.subscriptions.push(outChannel);
    logMsg('activate v' + context.extension.packageJSON.version + (vscode.env.remoteName ? ' remote=' + vscode.env.remoteName : ' local') + ' pid=' + process.pid + ' ui=v5 (icons only, data-driven blink)');
    // 0.6.2 一次性迁移：stdio 通道改为“默认关闭、显式注册”。旧版同意弹窗会把 stdio.enabled 写成
    // true（global），那不是用户的明确选择 —— 清掉它（workspace 级不动），随后 ensureUserMcp
    // 会按新规则移除用户级 mcp.json 里的残留条目。
    if (!context.globalState.get('stdioDefaultOffMigrated')) {
        (async () => {
            const c0 = vscode.workspace.getConfiguration('winExecMcp');
            const insp = c0.inspect('stdio.enabled');
            if (insp && insp.globalValue === true) {
                await c0.update('stdio.enabled', undefined, vscode.ConfigurationTarget.Global);
                logMsg('migration: cleared stdio.enabled (stdio channel is off by default now)');
                // 立刻按新值收敛一次：这个 await 期间本窗口的 autoSetup 可能已按旧值写入了条目，
                // 不补这一下就要等下一次激活才删（用户会看到"说了默认关，条目却还在"）。
                ensureUserMcp(context, vscode.workspace.getConfiguration('winExecMcp'));
            }
            await context.globalState.update('stdioDefaultOffMigrated', true);
        })().catch(() => { });
    }
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
        vscode.commands.registerCommand('winExecMcp.menu', () => showWinExecMenu()),
        vscode.commands.registerCommand('winExecMcp.diagnostics', () => runDiagnostics().catch((e) => logMsg('diagnostics failed: ' + e.message))),
        vscode.commands.registerCommand('winExecMcp.regenerateToken', () => regenerateToken().catch((e) => logMsg('token regen failed: ' + e.message))),
        vscode.commands.registerCommand('winExecMcp.copyToken', () => copyToken()),
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
    refreshStatusBar();
}

function deactivate() {
    if (watchdog) { clearInterval(watchdog); watchdog = null; }
    stopHttp();
}

module.exports = { activate, deactivate };
