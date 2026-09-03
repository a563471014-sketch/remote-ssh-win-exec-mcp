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

let child = null;
let autoToken = null;
let watchdog = null; // 定时检查：服务被其他窗口关闭/崩溃时自动恢复

function exePath(context) {
    return path.join(context.extensionPath, 'bin', 'win-exec-mcp.exe');
}

function userMcpPath() {
    return path.join(process.env.APPDATA || '', 'Code', 'User', 'mcp.json');
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
function ensureUserMcp(context, cfg) {
    const p = userMcpPath();
    let doc;
    try {
        doc = JSON.parse(fs.readFileSync(p, 'utf8'));
    } catch (e) {
        vscode.window.showErrorMessage('WinExec MCP: 用户级 mcp.json 解析失败（可能含注释），请手动配置：' + p);
        return;
    }
    if (!doc.servers) doc.servers = {};
    let changed = false;
    if (cfg.get('stdio.enabled', true)) {
        const want = { type: 'stdio', command: exePath(context), args: [], location: 'local' };
        if (JSON.stringify(doc.servers[STDIO_ID]) !== JSON.stringify(want)) {
            doc.servers[STDIO_ID] = want;
            changed = true;
        }
    } else if (doc.servers[STDIO_ID] && isOurEntry(STDIO_ID, doc.servers[STDIO_ID])) {
        delete doc.servers[STDIO_ID];
        changed = true;
    }
    if (doc.servers[HTTP_ID] && isOurEntry(HTTP_ID, doc.servers[HTTP_ID])) {
        delete doc.servers[HTTP_ID];
        changed = true;
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
    if (!cfg.get('http.enabled', true) || !cfg.get('ssh.autoForward', true)) return;
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

// 关闭 VS Code 对本端口的自动转发：RemoteForward 让远端出现监听后，autoForwardPorts 会把它
// 转发回客户端并绑定同名回环口，遮蔽 exe（Windows 特定回环绑定优先于通配绑定）且形成转发环路
function ensurePortsIgnore(port) {
    try {
        const rcfg = vscode.workspace.getConfiguration('remote');
        const attrs = rcfg.get('portsAttributes', {});
        const key = String(port);
        if (attrs[key] && attrs[key].onAutoForward === 'ignore') return;
        attrs[key] = { onAutoForward: 'ignore' };
        rcfg.update('portsAttributes', attrs, vscode.ConfigurationTarget.Global);
    } catch (e) { }
}

// 远端窗口：自动启动 http MCP 连接（等价替用户按 Start；定义异步加载，多轮重试）
function autoStartHttpConnection() {
    if (!vscode.env.remoteName) return;
    const cfg = vscode.workspace.getConfiguration('winExecMcp');
    if (!cfg.get('http.enabled', true)) return;
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
    let doc = {};
    try {
        const bytes = await vscode.workspace.fs.readFile(paths.file);
        doc = JSON.parse(new TextDecoder().decode(bytes));
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
    const rootFile = vscode.Uri.joinPath(paths.dir, '..', '.mcp.json');
    let mdoc = {};
    try {
        const bytes = await vscode.workspace.fs.readFile(rootFile);
        mdoc = JSON.parse(new TextDecoder().decode(bytes));
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
    // 清理 .vscode/mcp.json
    let doc;
    try {
        const bytes = await vscode.workspace.fs.readFile(paths.file);
        doc = JSON.parse(new TextDecoder().decode(bytes));
    } catch (e) { return; }
    if (doc.servers && doc.servers[HTTP_ID]) {
        delete doc.servers[HTTP_ID];
        await vscode.workspace.fs.writeFile(paths.file, new TextEncoder().encode(JSON.stringify(doc, null, '\t')));
    }
    // 同步清理项目根 .mcp.json
    const rootFile = vscode.Uri.joinPath(paths.dir, '..', '.mcp.json');
    let mdoc;
    try {
        const bytes = await vscode.workspace.fs.readFile(rootFile);
        mdoc = JSON.parse(new TextDecoder().decode(bytes));
    } catch (e) { return; }
    if (mdoc.mcpServers && mdoc.mcpServers['win-exec-mcp']) {
        delete mdoc.mcpServers['win-exec-mcp'];
        await vscode.workspace.fs.writeFile(rootFile, new TextEncoder().encode(JSON.stringify(mdoc, null, '\t')));
    }
    vscode.window.showInformationMessage('WinExec MCP: 已从本项目移除注册');
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
        vscode.commands.registerCommand('winExecMcp.register', () => ensureUserMcp(context, cfg)),
        vscode.commands.registerCommand('winExecMcp.registerProject', registerProject),
        vscode.commands.registerCommand('winExecMcp.unregisterProject', unregisterProject)
    );
    // 安装/更新即自动配置用户级 mcp.json（stdio + http）
    ensureUserMcp(context, cfg);
    // 远端窗口：自动拉起 http 进程 + 自动启动 http MCP 连接 + 自动配置 SSH 转发
    if (vscode.env.remoteName && cfg.get('http.enabled', true)) {
        setTimeout(() => startHttp(context), 3000);
        // watchdog：服务被其他窗口关闭/崩溃时自动恢复（多窗口场景保证可用）
        watchdog = setInterval(() => {
            const c2 = vscode.workspace.getConfiguration('winExecMcp');
            if (!c2.get('http.enabled', true)) return;
            const port = c2.get('http.port', 38848);
            isPortListening(port).then((listening) => {
                if (!listening) startHttp(context);
            });
        }, 15000);
    }
    autoStartHttpConnection();
    ensureSshForward();
    ensurePortsIgnore(cfg.get('http.port', 38848));
    // 可选：远端窗口激活时自动注册到项目级 .vscode/mcp.json（默认关闭，避免擅改项目文件）
    if (cfg.get('project.autoRegister', false) && vscode.env.remoteName) {
        setTimeout(() => registerProject().catch(() => { }), 4000);
    }
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((e) => {
        if (e.affectsConfiguration('winExecMcp')) {
            const c2 = vscode.workspace.getConfiguration('winExecMcp');
            ensureUserMcp(context, c2);
            if (c2.get('http.enabled', true)) {
                if (vscode.env.remoteName) startHttp(context);
                autoStartHttpConnection();
            } else {
                stopHttp();
            }
        }
    }));
}

function deactivate() {
    if (watchdog) { clearInterval(watchdog); watchdog = null; }
    stopHttp();
}

module.exports = { activate, deactivate };
