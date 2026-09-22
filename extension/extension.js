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

let child = null;
let autoToken = null;
let watchdog = null; // 定时检查：服务被其他窗口关闭/崩溃时自动恢复

// —— 更新提示：装/更新扩展后，当前窗口仍在跑旧代码，提示重载窗口 ——
let updateWatcher = null;      // fs.watch 句柄
let updateWatchTimer = null;   // 60s 轮询兜底
let updatePromptedFor = null;  // 已提示过的目标版本（防重复弹）

// 比对磁盘上扩展注册表记录的版本 vs 当前运行版本；不一致就提示重载
function checkPendingUpdate(context) {
    try {
        const extDir = path.dirname(context.extensionPath);
        const list = JSON.parse(fs.readFileSync(path.join(extDir, 'extensions.json'), 'utf8'));
        const running = context.extension.packageJSON.version;
        const me = list.find((e) => e.identifier && e.identifier.id === context.extension.id);
        if (!me || !me.version || me.version === running || updatePromptedFor === me.version) return;
        updatePromptedFor = me.version;
        vscode.window.showInformationMessage(
            'WinExec MCP: 已安装版本 ' + me.version + '（当前窗口运行 ' + running + '），重载窗口后生效',
            '重载窗口'
        ).then((pick) => { if (pick === '重载窗口') vscode.commands.executeCommand('workbench.action.reloadWindow'); });
    } catch (e) { }
}

// 监听扩展目录变化（安装/更新会重写 extensions.json），另加 60s 轮询兜底
function watchExtensionUpdates(context) {
    try {
        const extDir = path.dirname(context.extensionPath);
        let debounce = null;
        updateWatcher = fs.watch(extDir, { persistent: false }, (ev, name) => {
            if (name && name !== 'extensions.json' && name !== '.obsolete') return;
            if (debounce) clearTimeout(debounce);
            debounce = setTimeout(() => checkPendingUpdate(context), 1000);
        });
        updateWatchTimer = setInterval(() => checkPendingUpdate(context), 60000);
        setTimeout(() => checkPendingUpdate(context), 5000); // 启动后也查一次
    } catch (e) { }
}

function exePath(context) {
    return path.join(context.extensionPath, 'bin', 'win-exec-mcp.exe');
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
                ? { command: exePath(context), args: [] }
                : { type: 'stdio', command: exePath(context), args: [], location: 'local' };
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

// 远端窗口：自动在 ~/.ssh/config 追加 RemoteForward，让服务器本机工具经回环够到 Windows
// 只追加独立 Host 块（不改现有内容），先备份；下次 SSH 连接生效
function ensureSshForward() {
    if (!vscode.env.remoteName) return;
    if (vscode.env.remoteName.indexOf('ssh-remote+') !== 0) return;
    const host = vscode.env.remoteName.slice('ssh-remote+'.length);
    if (!host) return;
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    if (!cfg.get('http.enabled', false) || !cfg.get('ssh.autoForward', false)) return;
    const port = cfg.get('http.port', 38848);
    const localPort = cfg.get('ssh.localPort', 28848);
    const sshDir = path.join(os.homedir(), '.ssh');
    const sshConfig = path.join(sshDir, 'config');
    try {
        let content = '';
        try { content = fs.readFileSync(sshConfig, 'utf8'); } catch (e) { content = ''; }
        const re = new RegExp('RemoteForward[^\\r\\n]*:' + localPort + '\\b');
        if (re.test(content)) return; // 已配置过
        const block = '\nHost ' + host + '\n  RemoteForward 127.0.0.1:' + localPort + ' 127.0.0.1:' + port + '\n';
        if (!fs.existsSync(sshDir)) fs.mkdirSync(sshDir, { recursive: true });
        if (fs.existsSync(sshConfig)) {
            try { fs.copyFileSync(sshConfig, sshConfig + '.bak-winexec'); } catch (e) { }
        }
        fs.appendFileSync(sshConfig, block);
        vscode.window.showInformationMessage('WinExec MCP: 已在 ~/.ssh/config 追加 RemoteForward 127.0.0.1:' + localPort + ' → 127.0.0.1:' + port + '（下次 SSH 连接生效）');
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
        vscode.window.showInformationMessage('WinExec MCP: 已注册到本项目（.vscode/mcp.json + .mcp.json）');
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

function startHttp(context) {
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    const port = cfg.get('http.port', 38848);
    return isPortListening(port).then((listening) => {
        if (listening) return;
        const exe = exePath(context);
        if (!fs.existsSync(exe)) {
            vscode.window.showErrorMessage('WinExec MCP: 内置 exe 缺失 ' + exe);
            return;
        }
        child = spawn(exe, ['--http', String(port), '--token', currentToken(cfg), '--parent-pid', String(process.pid)], {
            windowsHide: true,
            stdio: 'ignore'
        });
        child.on('error', (err) => vscode.window.showErrorMessage('WinExec MCP 启动失败: ' + err.message));
        child.on('exit', () => { child = null; });
    });
}

function stopHttp() {
    if (child) { child.kill(); child = null; }
}

// 用户是否已同意：显式点击过同意，或手动开启了 stdio/http 开关（开关本身就是明确同意）
function consented(context, cfg) {
    if (context.globalState.get(CONSENT_KEY) === true) return true;
    return !!cfg.get('stdio.enabled', false) || !!cfg.get('http.enabled', false);
}

// 首次运行征得同意：说明将要修改的内容，用户明确同意后才启用任何自动行为
async function askConsent(context) {
    const pick = await vscode.window.showWarningMessage(
        'WinExec MCP 需要你的同意才会修改配置：\n' +
        '1) 将内置 win-exec-mcp.exe 注册到用户级 mcp.json（stdio MCP 服务器）\n' +
        '2) Remote-SSH 窗口打开时在本机 127.0.0.1 启动 HTTP MCP 服务并自动连接\n' +
        '默认不修改 ~/.ssh/config 与项目文件（相关设置默认关闭，可后续手动开启）',
        { modal: true }, '同意并启用', '暂不');
    if (pick === '同意并启用') {
        await context.globalState.update(CONSENT_KEY, true);
        const cfg = vscode.workspace.getConfiguration('winExecMcp');
        await cfg.update('stdio.enabled', true, vscode.ConfigurationTarget.Global);
        await cfg.update('http.enabled', true, vscode.ConfigurationTarget.Global);
        return true;
    }
    await context.globalState.update(CONSENT_KEY, false);
    return false;
}

// 同意后的自动配置（幂等，配置变化时可重复执行）
function autoSetup(context) {
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
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
    ensureSshForward();
    // 远端被 autoForward 误转发的端口是 RemoteForward 的入口端（ssh.localPort），不是 exe 的 http.port；
    // http.port 一并忽略是兜底：localPort 若配成与 exe 相同端口，转发监听会遮蔽 exe
    ensurePortsIgnore(cfg.get('ssh.localPort', 28848), cfg.get('http.port', 38848));
    // 可选：远端窗口激活时自动注册到项目级 .vscode/mcp.json（默认关闭，避免擅改项目文件）
    if (cfg.get('project.autoRegister', false) && vscode.env.remoteName) {
        setTimeout(() => registerProject().catch(() => { }), 4000);
    }
}

function activate(context) {
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
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
        vscode.commands.registerCommand('winExecMcp.setup', async () => {
            if (await askConsent(context)) autoSetup(context);
        })
    );
    // 未同意前不写任何文件、不拉起任何进程：首次运行弹窗征询；同意过或手动开启开关才自动配置
    if (consented(context, cfg)) {
        autoSetup(context);
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
    // 安装/更新后提示重载（当前窗口仍在跑旧代码）
    watchExtensionUpdates(context);
}

function deactivate() {
    if (watchdog) { clearInterval(watchdog); watchdog = null; }
    if (updateWatchTimer) { clearInterval(updateWatchTimer); updateWatchTimer = null; }
    if (updateWatcher) { try { updateWatcher.close(); } catch (e) { } updateWatcher = null; }
    stopHttp();
}

module.exports = { activate, deactivate };
