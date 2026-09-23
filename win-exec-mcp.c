/*
 * win-exec-mcp.c — MCP server: 在 Windows 客户端执行命令，供 Remote-SSH 的 agent 调用。
 *
 * 架构：配置为 VS Code 的 local MCP（location: "local"），在 Windows 客户端启动。
 * 工具：windows_exec(command, timeout_ms) → 返回 stdout/stderr/exit code（UTF-8）。
 *
 * 构建（Linux 交叉编译）：
 *   x86_64-w64-mingw32-gcc -O2 -static -o win-exec-mcp.exe win-exec-mcp.c
 *
 * 传输：MCP stdio —— newline-delimited JSON，同时兼容 Content-Length 帧。
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ============ 极简 JSON 实现（解析 + 序列化） ============ */
typedef struct Json {
    int type;            /* 0=obj 1=arr 2=str 3=num 4=bool 5=null */
    char *str;           /* 字符串值 / 对象 key */
    double num;
    int boolean;
    struct Json **items; /* obj: [k,v,k,v...]; arr: [v,v,...] */
    int count;
} Json;

static Json *j_new(int type) {
    Json *j = (Json *)calloc(1, sizeof(Json));
    j->type = type;
    return j;
}
static void j_free(Json *j) {
    if (!j) return;
    if (j->str) free(j->str);
    for (int i = 0; i < j->count; i++) j_free(j->items[i]);
    if (j->items) free(j->items);
    free(j);
}
static void j_add(Json *parent, Json *child) {
    parent->items = (Json **)realloc(parent->items, (parent->count + 1) * sizeof(Json *));
    parent->items[parent->count++] = child;
}
static Json *j_obj(void) { return j_new(0); }
static Json *j_arr(void) { return j_new(1); }
static Json *j_str(const char *s) { Json *j = j_new(2); j->str = _strdup(s); return j; }
static Json *j_num(double n) { Json *j = j_new(3); j->num = n; return j; }
static Json *j_bool(int b) { Json *j = j_new(4); j->boolean = b; return j; }
static Json *j_null(void) { return j_new(5); }
static void j_set(Json *obj, const char *key, Json *val) {
    j_add(obj, j_str(key));
    j_add(obj, val);
}
/* MCP 规范 content 项：{"type":"text","text":"..."}，裸字符串会被 VS Code 客户端丢弃 */
static Json *j_text(const char *s) {
    Json *o = j_obj();
    j_set(o, "type", j_str("text"));
    j_set(o, "text", j_str(s));
    return o;
}

/* 解析器 */
typedef struct { const char *s; int pos; int len; } P;
static void p_ws(P *p) { while (p->pos < p->len && (p->s[p->pos] == ' ' || p->s[p->pos] == '\t' || p->s[p->pos] == '\r' || p->s[p->pos] == '\n')) p->pos++; }
static Json *p_value(P *p);

static Json *p_string(P *p) {
    p->pos++; /* 跳过 " */
    char *buf = (char *)malloc(p->len + 1);
    int n = 0;
    while (p->pos < p->len && p->s[p->pos] != '"') {
        if (p->s[p->pos] == '\\' && p->pos + 1 < p->len) {
            p->pos++;
            switch (p->s[p->pos]) {
                case 'n': buf[n++] = '\n'; break;
                case 't': buf[n++] = '\t'; break;
                case 'r': buf[n++] = '\r'; break;
                case 'b': buf[n++] = '\b'; break;
                case 'f': buf[n++] = '\f'; break;
                case '\\': buf[n++] = '\\'; break;
                case '"': buf[n++] = '"'; break;
                case '/': buf[n++] = '/'; break;
                case 'u': {
                    /* \uXXXX → UTF-8（BMP） */
                    unsigned cp = 0; int ok = 1;
                    for (int k = 0; k < 4; k++) {
                        char h = (p->pos + 1 + k < p->len) ? p->s[p->pos + 1 + k] : 0;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else { ok = 0; break; }
                    }
                    if (!ok || cp < 0x20 || cp == '"' || cp == '\\') {
                        buf[n++] = '?';
                        if (ok) p->pos += 4;
                    } else if (cp < 0x80) {
                        buf[n++] = (char)cp;
                        p->pos += 4;
                    } else if (cp < 0x800) {
                        buf[n++] = (char)(0xC0 | (cp >> 6));
                        buf[n++] = (char)(0x80 | (cp & 0x3F));
                        p->pos += 4;
                    } else {
                        buf[n++] = (char)(0xE0 | (cp >> 12));
                        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[n++] = (char)(0x80 | (cp & 0x3F));
                        p->pos += 4;
                    }
                    break;
                }
                default: buf[n++] = p->s[p->pos]; break;
            }
        } else {
            buf[n++] = p->s[p->pos];
        }
        p->pos++;
    }
    buf[n] = 0;
    if (p->pos < p->len) p->pos++; /* 跳过 " */
    Json *j = j_new(2);
    j->str = buf;
    return j;
}
static Json *p_object(P *p) {
    Json *o = j_obj();
    p->pos++; /* { */
    p_ws(p);
    while (p->pos < p->len && p->s[p->pos] != '}') {
        p_ws(p);
        Json *k = p_string(p);
        p_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ':') p->pos++;
        p_ws(p);
        Json *v = p_value(p);
        j_add(o, k);
        j_add(o, v);
        p_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') p->pos++;
        p_ws(p);
    }
    if (p->pos < p->len) p->pos++; /* } */
    return o;
}
static Json *p_array(P *p) {
    Json *a = j_arr();
    p->pos++; /* [ */
    p_ws(p);
    while (p->pos < p->len && p->s[p->pos] != ']') {
        Json *v = p_value(p);
        j_add(a, v);
        p_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') p->pos++;
        p_ws(p);
    }
    if (p->pos < p->len) p->pos++; /* ] */
    return a;
}
static Json *p_value(P *p) {
    p_ws(p);
    if (p->pos >= p->len) return j_null();
    char c = p->s[p->pos];
    if (c == '"') return p_string(p);
    if (c == '{') return p_object(p);
    if (c == '[') return p_array(p);
    if (strncmp(p->s + p->pos, "true", 4) == 0) { p->pos += 4; return j_bool(1); }
    if (strncmp(p->s + p->pos, "false", 5) == 0) { p->pos += 5; return j_bool(0); }
    if (strncmp(p->s + p->pos, "null", 4) == 0) { p->pos += 4; return j_null(); }
    /* 数字 */
    char *end = NULL;
    double d = strtod(p->s + p->pos, &end);
    if (end && end != p->s + p->pos) { p->pos = (int)(end - p->s); return j_num(d); }
    return j_null();
}
static Json *json_parse(const char *s) {
    P p = { s, 0, (int)strlen(s) };
    Json *r = p_value(&p);
    return r;
}
static Json *j_get(Json *obj, const char *key) {
    if (!obj || obj->type != 0) return NULL;
    for (int i = 0; i + 1 < obj->count; i += 2) {
        Json *k = obj->items[i];
        if (k->type == 2 && strcmp(k->str, key) == 0) return obj->items[i + 1];
    }
    return NULL;
}

/* 序列化到动态 buffer */
typedef struct { char *buf; int len; int cap; } SB;
static void sb_put(SB *sb, const char *s) {
    int n = (int)strlen(s);
    if (sb->len + n + 1 > sb->cap) {
        sb->cap = (sb->cap + n + 16) * 2;
        sb->buf = (char *)realloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = 0;
}
static void j_ser(SB *sb, Json *j) {
    if (!j) { sb_put(sb, "null"); return; }
    switch (j->type) {
        case 2: {
            sb_put(sb, "\"");
            for (char *c = j->str; *c; c++) {
                if (*c == '"' || *c == '\\') { char t[3] = { '\\', *c, 0 }; sb_put(sb, t); }
                else if (*c == '\n') sb_put(sb, "\\n");
                else if (*c == '\r') sb_put(sb, "\\r");
                else if (*c == '\t') sb_put(sb, "\\t");
                else if ((unsigned char)*c < 0x20) { /* 其余控制字符（如 ANSI \x1b）必须 \uXXXX 转义，否则非法 JSON */
                    char t[8];
                    snprintf(t, sizeof(t), "\\u%04x", (unsigned char)*c);
                    sb_put(sb, t);
                }
                else { char t[2] = { *c, 0 }; sb_put(sb, t); }
            }
            sb_put(sb, "\"");
            break;
        }
        case 3: {
            char t[64];
            snprintf(t, sizeof(t), "%g", j->num);
            sb_put(sb, t);
            break;
        }
        case 4: sb_put(sb, j->boolean ? "true" : "false"); break;
        case 5: sb_put(sb, "null"); break;
        case 1: {
            sb_put(sb, "[");
            for (int i = 0; i < j->count; i++) {
                if (i) sb_put(sb, ",");
                j_ser(sb, j->items[i]);
            }
            sb_put(sb, "]");
            break;
        }
        case 0: {
            sb_put(sb, "{");
            for (int i = 0; i + 1 < j->count; i += 2) {
                if (i) sb_put(sb, ",");
                j_ser(sb, j->items[i]);
                sb_put(sb, ":");
                j_ser(sb, j->items[i + 1]);
            }
            sb_put(sb, "}");
            break;
        }
    }
}
static char *json_serialize(Json *j) {
    SB sb = { NULL, 0, 0 };
    j_ser(&sb, j);
    return sb.buf ? sb.buf : _strdup("null");
}

/* ============ UTF-8 校验 + GBK→UTF-8 转码 ============ */
static int utf8_valid(const char *s, int len) {
    int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i++; continue; }
        int need;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return 0;
        if (i + need >= len) return 0;
        for (int k = 1; k <= need; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
        i += need + 1;
    }
    return 1;
}
static char *gbk_to_utf8(const char *gbk, int len) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, gbk, len, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, gbk, len, w, wlen);
    w[wlen] = 0;
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
    char *u = (char *)malloc(ulen + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, u, ulen, NULL, NULL);
    u[ulen] = 0;
    free(w);
    return u;
}

/* ============ git-bash 探测（仅显式候选路径；绝不回退 System32\bash.exe=WSL） ============ */
static char g_bash_path[MAX_PATH * 2]; /* 空 = 未找到 */

static int file_exists_a(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void detect_git_bash(void) {
    g_bash_path[0] = 0;
    char cand[MAX_PATH * 2];
    const char *envs[3];
    int n = 0;
    if (getenv("ProgramFiles")) envs[n++] = "ProgramFiles";
    if (getenv("ProgramFiles(x86)")) envs[n++] = "ProgramFiles(x86)";
    if (getenv("LocalAppData")) envs[n++] = "LocalAppData";
    for (int i = 0; i < n; i++) {
        const char *dir = getenv(envs[i]);
        if (strcmp(envs[i], "LocalAppData") == 0)
            snprintf(cand, sizeof(cand), "%s\\Programs\\Git\\bin\\bash.exe", dir);
        else
            snprintf(cand, sizeof(cand), "%s\\Git\\bin\\bash.exe", dir);
        if (file_exists_a(cand)) {
            strncpy(g_bash_path, cand, sizeof(g_bash_path) - 1);
            g_bash_path[sizeof(g_bash_path) - 1] = 0;
            return;
        }
    }
}

/* ============ 进度通知（notifications/progress）——自动流式输出 ============ */
/* 当客户端在 tools/call 的 params._meta.progressToken 里带令牌时：
   - stdio：命令执行期间把新输出按行作为 progress 通知实时写到 stdout；
   - HTTP ：响应提前以 SSE 流打开，进度事件边执行边推送，最后再发最终 result。
   客户端（VS Code / Claude Code 等）在工具调用 UI 上实时显示，调用方无需做任何事。
   无 token 时行为与旧版完全一致（协议要求：没有 token 不应主动发 progress）。 */
static int g_http_mode = 0;               /* 1 = HTTP 服务模式 */
static __thread char   g_ptok[256];       /* 本次调用的 progressToken（字符串型） */
static __thread int    g_ptok_num = 0;    /* token 是数字型 */
static __thread double g_ptok_numval = 0; /* 数字型 token 的值 */
static __thread int    g_pseq = 0;        /* progress 递增序号 */
static __thread DWORD  g_p_last_ms = 0;   /* 上次发送时间（限流 ~10/s） */
static __thread int    g_streaming = 0;   /* HTTP：本次响应已开始 SSE 流式 */
static __thread SOCKET g_stream_sock = 0; /* HTTP：流式响应所用连接 */

static int progress_begin(Json *params) {
    g_ptok[0] = 0; g_ptok_num = 0; g_ptok_numval = 0; g_pseq = 0; g_p_last_ms = 0;
    Json *meta = params ? j_get(params, "_meta") : NULL;
    Json *t = meta ? j_get(meta, "progressToken") : NULL;
    if (!t) return 0;
    if (t->type == 2 && t->str && t->str[0]) {
        strncpy(g_ptok, t->str, sizeof(g_ptok) - 1);
        g_ptok[sizeof(g_ptok) - 1] = 0;
        return 1;
    }
    if (t->type == 3) { g_ptok_num = 1; g_ptok_numval = t->num; return 1; }
    return 0;
}
static int progress_on(void) { return g_ptok[0] || g_ptok_num; }

/* 发送一条 notifications/progress（text 必须是合法 UTF-8） */
static void progress_emit(const char *text) {
    if (!progress_on()) return;
    Json *n = j_obj();
    j_set(n, "jsonrpc", j_str("2.0"));
    j_set(n, "method", j_str("notifications/progress"));
    Json *p = j_obj();
    if (g_ptok_num) j_set(p, "progressToken", j_num(g_ptok_numval));
    else j_set(p, "progressToken", j_str(g_ptok));
    j_set(p, "progress", j_num(++g_pseq));
    j_set(p, "message", j_str(text));
    j_set(n, "params", p);
    char *s = json_serialize(n);
    if (g_http_mode) {
        if (g_streaming) {
            char *sse = (char *)malloc(strlen(s) + 64);
            int sn = sprintf(sse, "event: message\ndata: %s\n\n", s);
            send(g_stream_sock, sse, sn, 0);
            free(sse);
        }
    } else {
        printf("%s\n", s);
        fflush(stdout);
    }
    free(s);
    j_free(n);
}

/* 原始输出 → UTF-8 副本（合法 UTF-8 直接用；否则按 GBK 转） */
static char *to_utf8_dup(const char *s, int len) {
    char *r;
    if (len <= 0) return NULL;
    if (utf8_valid(s, len)) {
        r = (char *)malloc(len + 1);
        memcpy(r, s, len); r[len] = 0;
        return r;
    }
    r = gbk_to_utf8(s, len);
    if (r) return r;
    r = (char *)malloc(len + 1);
    memcpy(r, s, len); r[len] = 0;
    return r;
}

/* run_cmd 每读到一段输出后调用：把 [*emitted, olen) 内"最后一个换行前"的新内容发出去。
   force=1（命令结束）时无换行也发、无视限流。消息太长时只发尾部 1200 字节（UI 是实时尾部视图）。 */
static void progress_scan(const char *out, int olen, int *emitted, int force) {
    if (!progress_on() || olen <= *emitted) return;
    int end;
    if (force) {
        end = olen;
    } else {
        int i = olen - 1;
        while (i >= *emitted && out[i] != '\n') i--;
        if (i < *emitted) return; /* 暂无完整的新行 */
        end = i + 1;
    }
    DWORD now = GetTickCount();
    if (!force && now - g_p_last_ms < 100) return; /* 限流：~10 条/秒 */
    g_p_last_ms = now;
    const char *seg = out + *emitted;
    int seg_len = end - *emitted;
    if (seg_len > 1200) {
        seg += seg_len - 1200;
        seg_len = 1200;
        while (seg_len > 0 && ((unsigned char)*seg & 0xC0) == 0x80) { seg++; seg_len--; }
    }
    char *u = to_utf8_dup(seg, seg_len);
    if (u) {
        int ul = (int)strlen(u);
        while (ul > 0 && (u[ul - 1] == '\n' || u[ul - 1] == '\r')) u[--ul] = 0;
        if (ul > 0) progress_emit(u);
        free(u);
    }
    *emitted = end;
}

/* ============ 执行 Windows 命令（CreateProcess + 管道 + 超时） ============ */
typedef struct {
    char *output;   /* UTF-8 文本 */
    int exit_code;
    int timed_out;
    int spawn_err;
} RunResult;

#define MAX_OUTPUT (16 * 1024 * 1024) /* 输出上限，超出截断（仍需继续排空管道，防止子进程写满管道阻塞） */

/* 追加数据到输出缓冲，超出 MAX_OUTPUT 的部分丢弃 */
static void out_append(char **out, int *olen, int *ocap, const char *buf, int rd, int *truncated) {
    if (*olen >= MAX_OUTPUT) { *truncated = 1; return; }
    int take = rd;
    if (*olen + take > MAX_OUTPUT) { take = MAX_OUTPUT - *olen; *truncated = 1; }
    if (*olen + take + 1 > *ocap) {
        *ocap = (*ocap + take + 16) * 2;
        *out = (char *)realloc(*out, *ocap);
    }
    memcpy(*out + *olen, buf, take);
    *olen += take;
    (*out)[*olen] = 0;
}

static RunResult run_cmdline(const char *cmd_line_in, int timeout_ms) {
    RunResult rr = { NULL, -1, 0, 0 };
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hOutR = NULL, hOutW = NULL;
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) { rr.spawn_err = 1; return rr; }
    SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOutW;
    si.hStdError = hOutW;
    PROCESS_INFORMATION pi = { 0 };

    /* 合并 stderr 到 stdout；完整命令行由调用方构造（cmd /c 或 git-bash 两条路径）。
       CreateProcess 可能修改命令行缓冲，这里持有可变副本。 */
    char *cmdline = _strdup(cmd_line_in);
    /* UNC cwd 自我修复：win-exec 当前目录若是 UNC（\\\\ 开头），cmd 继承会报
       "UNC 路径不受支持"；此时用系统盘符作子进程 cwd（不依赖任何盘映射）。 */
    char safe_cwd[MAX_PATH] = "";
    if (GetCurrentDirectoryA(MAX_PATH, safe_cwd) && safe_cwd[0] == '\\' && safe_cwd[1] == '\\') {
        GetWindowsDirectoryA(safe_cwd, MAX_PATH);
        for (char *p = safe_cwd; *p; p++) if (*p == '\\') { *p = 0; break; }
    }
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL,
                             safe_cwd[0] ? safe_cwd : NULL, &si, &pi);
    free(cmdline);
    CloseHandle(hOutW);

    if (!ok) { rr.spawn_err = 1; CloseHandle(hOutR); return rr; }

    /* 读输出（管道阻塞读，进程退出后读到 EOF） */
    char *out = (char *)malloc(1);
    out[0] = 0;
    int olen = 0, ocap = 1;
    int truncated = 0;
    int emitted = 0; /* 已经通过进度通知发出的字节数 */
    char buf[4096];
    DWORD rd;
    /* 主循环：等待进程 + 读管道（非阻塞方式） */
    DWORD start_ms = GetTickCount(); /* 每次调用独立计时（不能 static，否则第二次调用起全部误判超时） */
    int alive = 1;
    while (alive) {
        /* 用 PeekNamedPipe 避免阻塞 */
        DWORD avail = 0;
        if (PeekNamedPipe(hOutR, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (ReadFile(hOutR, buf, sizeof(buf), &rd, NULL) && rd > 0) {
                out_append(&out, &olen, &ocap, buf, (int)rd, &truncated);
                progress_scan(out, olen, &emitted, 0);
            }
        }
        DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        if (wait != WAIT_TIMEOUT) {
            /* 读完剩余数据 */
            while (PeekNamedPipe(hOutR, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                if (ReadFile(hOutR, buf, sizeof(buf), &rd, NULL) && rd > 0) {
                    out_append(&out, &olen, &ocap, buf, (int)rd, &truncated);
                    progress_scan(out, olen, &emitted, 0);
                }
            }
            alive = 0;
        }
        /* 超时检查 */
        if (alive && timeout_ms > 0 && (GetTickCount() - start_ms) > (DWORD)timeout_ms) {
            TerminateProcess(pi.hProcess, 1);
            rr.timed_out = 1;
            alive = 0;
        }
    }
    /* 退出码必须在 CloseHandle 之前获取，否则永远拿不到真实值 */
    if (!rr.timed_out) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        rr.exit_code = (int)code;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hOutR);

    /* 截断时回退到 ASCII 边界，避免截断多字节字符导致整段被误判为 GBK 转码 */
    if (truncated) {
        while (olen > 0 && (unsigned char)out[olen - 1] >= 0x80) {
            olen--;
            out[olen] = 0;
        }
        out_append(&out, &olen, &ocap, "\n[output truncated]\n", 20, &truncated);
    }

    /* 收尾：把剩余未发的输出 flush 成最后一条进度 */
    progress_scan(out, olen, &emitted, 1);

    /* 编码处理：合法 UTF-8 原样，否则按 GBK 转 UTF-8 */
    if (olen > 0 && utf8_valid(out, olen)) {
        rr.output = out;
    } else {
        char *u = gbk_to_utf8(out, olen);
        rr.output = u ? u : out;
        if (u) free(out);
    }
    return rr;
}

/* ============ 两条执行路径的入口 ============ */
static void sbuf_append(char **buf, int *len, int *cap, const char *s) {
    int n = (int)strlen(s);
    if (*len + n + 1 > *cap) { *cap = (*len + n + 16) * 2; *buf = (char *)realloc(*buf, *cap); }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

/* 按 Windows/MSYS 参数规则加引号：\ 仅在 " 前成对双写，" 用 \" 转义 */
static void sbuf_append_quoted(char **buf, int *len, int *cap, const char *s) {
    int n = (int)strlen(s);
    if (*len + n * 2 + 4 > *cap) { *cap = (*len + n * 2 + 16) * 2; *buf = (char *)realloc(*buf, *cap); }
    char *b = *buf;
    b[(*len)++] = '"';
    int bs = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\\') { bs++; b[(*len)++] = '\\'; continue; }
        if (*p == '"') {
            for (int k = 0; k < bs; k++) b[(*len)++] = '\\'; /* 双写：2n+1 个 \ 后跟字面 " */
            b[(*len)++] = '\\';
            b[(*len)++] = '"';
            bs = 0;
            continue;
        }
        bs = 0;
        b[(*len)++] = *p;
    }
    for (int k = 0; k < bs; k++) b[(*len)++] = '\\'; /* 结尾反斜杠双写，防转义收尾引号 */
    b[(*len)++] = '"';
    b[*len] = 0;
}

/* 默认路径：cmd /c <command>。注意 cmd 的 /c 引号启发式：命令以引号开头且含特殊字符时
   会剥掉首尾引号（"C:\Program Files\..." 被截断成 'C:\Program'），此处主动再包一层。 */
static RunResult run_cmd(const char *cmd, int timeout_ms) {
    int s0 = 0;
    while (cmd[s0] == ' ' || cmd[s0] == '\t') s0++;
    char *line = (char *)malloc(strlen(cmd) + 16);
    if (cmd[s0] == '"') sprintf(line, "cmd /c \"%s\"", cmd);
    else sprintf(line, "cmd /c %s", cmd);
    RunResult rr = run_cmdline(line, timeout_ms);
    free(line);
    return rr;
}

/* git-bash 路径：直接 CreateProcess bash.exe（不经 cmd，避免 % 展开与引号启发式），
   脚本作为 -c 参数；-c 已带完整 MSYS PATH 且保持当前工作目录。 */
static RunResult run_bash_cmd(const char *script, int timeout_ms) {
    char *line = NULL;
    int len = 0, cap = 0;
    sbuf_append_quoted(&line, &len, &cap, g_bash_path);
    sbuf_append(&line, &len, &cap, " -c ");
    sbuf_append_quoted(&line, &len, &cap, script);
    RunResult rr = run_cmdline(line, timeout_ms);
    free(line);
    return rr;
}

/* ============ MCP 协议 ============ */
/* 注：g_http_mode 已提前到 run_cmd 之前定义（进度通知共用） */
static __thread SB g_http_resp; /* HTTP 模式：收集响应（每线程独立） */
static void send_json(Json *j) {
    char *s = json_serialize(j);
    if (g_http_mode) {
        sb_put(&g_http_resp, s);
    } else {
        printf("%s\n", s);
        fflush(stdout);
    }
    free(s);
}
static Json *resp_result(Json *id, Json *result) {
    Json *r = j_obj();
    j_set(r, "jsonrpc", j_str("2.0"));
    if (id) j_set(r, "id", id); else j_set(r, "id", j_null());
    j_set(r, "result", result);
    return r;
}
static Json *resp_error(Json *id, int code, const char *msg) {
    Json *r = j_obj();
    j_set(r, "jsonrpc", j_str("2.0"));
    if (id) j_set(r, "id", id); else j_set(r, "id", j_null());
    Json *e = j_obj();
    j_set(e, "code", j_num(code));
    j_set(e, "message", j_str(msg));
    j_set(r, "error", e);
    return r;
}

static void handle_tools_call(Json *id, Json *params) {
    Json *name = j_get(params, "name");
    Json *args = j_get(params, "arguments");
    const char *tool = (name && name->type == 2) ? name->str : "";

    progress_begin(params); /* 客户端带 progressToken 时启用自动流式输出 */

    if (strcmp(tool, "windows_exec") == 0) {
        const char *command = "";
        int timeout_ms = 30000;
        const char *shell = NULL;
        if (args) {
            Json *c = j_get(args, "command");
            if (c && c->type == 2) command = c->str;
            Json *t = j_get(args, "timeout_ms");
            if (t && t->type == 3) timeout_ms = (int)t->num;
            else if (t && t->type == 2) timeout_ms = atoi(t->str);
            Json *sh = j_get(args, "shell");
            if (sh && sh->type == 2 && sh->str && sh->str[0]) shell = sh->str;
        }
        if (!command[0]) {
            Json *r = j_obj();
            j_set(r, "isError", j_bool(1));
            Json *c = j_arr();
            j_add(c, j_text("参数 command 不能为空"));
            j_set(r, "content", c);
            send_json(resp_result(id, r));
            return;
        }
        int use_bash = 0;
        if (shell) {
            if (_stricmp(shell, "gitbash") == 0 || _stricmp(shell, "bash") == 0 || _stricmp(shell, "git-bash") == 0) use_bash = 1;
            else if (_stricmp(shell, "cmd") != 0) {
                Json *r = j_obj();
                j_set(r, "isError", j_bool(1));
                Json *c = j_arr();
                char emsg[160];
                snprintf(emsg, sizeof(emsg), "shell 参数仅支持 \"cmd\" 或 \"gitbash\"（收到: %.60s）", shell);
                j_add(c, j_text(emsg));
                j_set(r, "content", c);
                send_json(resp_result(id, r));
                return;
            }
        }
        if (use_bash && !g_bash_path[0]) {
            Json *r = j_obj();
            j_set(r, "isError", j_bool(1));
            Json *c = j_arr();
            j_add(c, j_text("未找到 git-bash（已检查 Program Files / Program Files (x86) / LocalAppData 下的 Git 安装）；请改用 cmd 语法（省略 shell 或传 shell:\"cmd\"）。"));
            j_set(r, "content", c);
            send_json(resp_result(id, r));
            return;
        }
        /* 先推一条"开始执行"：长命令的静默期也有即时反馈 */
        if (progress_on()) {
            char head[200];
            snprintf(head, sizeof(head), "▶ 开始执行: %.140s", command);
            int hl = (int)strlen(head);
            while (hl > 0 && ((unsigned char)head[hl - 1] & 0xC0) == 0x80) hl--;
            if (hl > 0 && (unsigned char)head[hl - 1] >= 0xC0) hl--;
            head[hl] = 0;
            progress_emit(head);
            g_p_last_ms = GetTickCount();
        }
        RunResult rr = use_bash ? run_bash_cmd(command, timeout_ms) : run_cmd(command, timeout_ms);
        char prefix[128];
        const char *shmark = use_bash ? " [shell: gitbash]" : "";
        if (rr.spawn_err) sprintf(prefix, "[spawn error]%s", shmark);
        else if (rr.timed_out) sprintf(prefix, "[exit: timeout(%dms) 命令超时被终止]%s", timeout_ms, shmark);
        else sprintf(prefix, "[exit: %d]%s", rr.exit_code, shmark);
        char *text = (char *)malloc(strlen(prefix) + (rr.output ? strlen(rr.output) : 0) + 16);
        sprintf(text, "%s\n%s", prefix, rr.output ? rr.output : "");
        Json *r = j_obj();
        Json *c = j_arr();
        j_add(c, j_text(text));
        j_set(r, "content", c);
        send_json(resp_result(id, r));
        free(text);
        free(rr.output);
        g_ptok[0] = 0; g_ptok_num = 0; /* 清理本次进度上下文 */
        return;
    }

    Json *r = j_obj();
    j_set(r, "isError", j_bool(1));
    Json *c = j_arr();
    j_add(c, j_text("未知工具"));
    j_set(r, "content", c);
    send_json(resp_result(id, r));
}

static void handle_message(const char *line) {
    Json *msg = json_parse(line);
    if (!msg || msg->type != 0) { if (msg) j_free(msg); return; }
    Json *id = j_get(msg, "id");
    Json *method = j_get(msg, "method");
    const char *m = (method && method->type == 2) ? method->str : "";
    Json *params = j_get(msg, "params");

    if (strcmp(m, "initialize") == 0) {
        Json *r = j_obj();
        /* protocolVersion 回显客户端请求的版本，避免 VS Code 因版本不匹配丢弃响应 */
        Json *pv = j_get(params, "protocolVersion");
        const char *proto = (pv && pv->type == 2 && pv->str && *pv->str) ? pv->str : "2024-11-05";
        j_set(r, "protocolVersion", j_str(proto));
        Json *caps = j_obj();
        j_set(caps, "tools", j_obj());
        j_set(r, "capabilities", caps);
        Json *info = j_obj();
        j_set(info, "name", j_str("win-exec-mcp"));
        j_set(info, "version", j_str("0.3.6"));
        j_set(r, "serverInfo", info);
        char instr[600];
        snprintf(instr, sizeof(instr),
            "windows_exec 在 Windows 主机上执行命令：默认 cmd 语法。%s"
            "长命令的输出会以 progress 通知实时推送，最终结果包含完整输出与退出码。",
            g_bash_path[0]
                ? "本机已安装 git-bash：需要 Linux 风格命令或 .sh 脚本时请传 shell:\"gitbash\"（服务端自动定位 bash.exe，不会误用 WSL）。"
                : "本机未检测到 git-bash。");
        j_set(r, "instructions", j_str(instr));
        send_json(resp_result(id, r));
    } else if (strcmp(m, "notifications/initialized") == 0 || strcmp(m, "initialized") == 0) {
        /* 无响应 */
    } else if (strcmp(m, "ping") == 0) {
        send_json(resp_result(id, j_obj()));
    } else if (strcmp(m, "tools/list") == 0) {
        Json *r = j_obj();
        Json *tools = j_arr();
        Json *t = j_obj();
        j_set(t, "name", j_str("windows_exec"));
        char desc[1200];
        snprintf(desc, sizeof(desc),
            "在 Windows 客户端执行一条命令（供 Remote-SSH/Linux 端的 agent 调用，跑在用户电脑的 Windows 上）。"
            "任何 Windows 命令/CLI/脚本皆可（如 dir、ipconfig、PowerShell、adb、esptool 等）；"
            "结果返回 stdout/stderr 和退出码。"
            "参数: command(必填, Windows 命令字符串, 支持 && 和管道), timeout_ms(可选, 超时毫秒, 默认 30000), "
            "shell(可选, \"cmd\"(默认) 或 \"gitbash\")。%s",
            g_bash_path[0]
                ? "【环境】本机已安装 git-bash：需要 Linux 风格命令（grep/sed/awk/ls/单引号/$变量 等）或 .sh 脚本时，传 shell:\"gitbash\" 即可（服务端自动定位 bash.exe，无需自行拼路径与引号，也不会误用 WSL）。"
                : "【环境】本机未检测到 git-bash：请使用 cmd/Windows 语法。");
        j_set(t, "description", j_str(desc));
        Json *schema = j_obj();
        j_set(schema, "type", j_str("object"));
        Json *props = j_obj();
        Json *p1 = j_obj();
        j_set(p1, "type", j_str("string"));
        j_set(p1, "description", j_str("要执行的 Windows 命令，如 adb devices"));
        j_set(props, "command", p1);
        Json *p2 = j_obj();
        j_set(p2, "type", j_str("number"));
        j_set(p2, "description", j_str("超时毫秒，默认 30000"));
        j_set(props, "timeout_ms", p2);
        Json *p3 = j_obj();
        j_set(p3, "type", j_str("string"));
        j_set(p3, "description", j_str("执行 shell：\"cmd\"(默认) 或 \"gitbash\"（Linux 风格命令 / .sh 脚本用）"));
        j_set(props, "shell", p3);
        j_set(schema, "properties", props);
        Json *req = j_arr();
        j_add(req, j_str("command"));
        j_set(schema, "required", req);
        j_set(t, "inputSchema", schema);
        j_add(tools, t);
        j_set(r, "tools", tools);
        send_json(resp_result(id, r));
    } else if (strcmp(m, "tools/call") == 0) {
        handle_tools_call(id, params);
    } else {
        send_json(resp_error(id, -32601, "method not found"));
    }
    j_free(msg);
}

/* 读取一条消息：支持 newline-delimited JSON 和 Content-Length 帧 */
static char *read_message(void) {
    static char *line = NULL;
    static int cap = 0;
    int len = 0;
    int c;
    /* 探测 Content-Length 头 */
    {
        /* 读第一行判断 */
        int first_len = 0;
        char first[32] = { 0 };
        while (first_len < 31) {
            c = getchar();
            if (c == EOF) return NULL;
            if (c == '\n') break;
            if (c != '\r') first[first_len++] = (char)c;
        }
        if (strncmp(first, "Content-Length:", 15) == 0) {
            int body_len = atoi(first + 15);
            /* 跳过剩余头（直到空行） */
            int done = 0;
            while (!done) {
                int c2 = getchar();
                if (c2 == EOF) return NULL;
                if (c2 == '\n') {
                    /* 上一行是否为空（\r\n\r\n）*/
                    /* 简单处理：读完两行 */
                    int c3 = getchar();
                    if (c3 == '\r') getchar();
                    else if (c3 == '\n') { /* 空行 */ }
                    else { /* 重新处理？ */ }
                    done = 1;
                }
            }
            line = (char *)malloc(body_len + 1);
            for (int i = 0; i < body_len; i++) {
                int c4 = getchar();
                if (c4 == EOF) { free(line); return NULL; }
                line[i] = (char)c4;
            }
            line[body_len] = 0;
            return line;
        } else {
            /* 普通 newline-delimited 行：first 只是行首，继续读完剩余部分，
             * 否则超过 31 字节的消息会被截断导致握手失败 */
            if (first_len + 1 > cap) { cap = first_len + 16; line = (char *)realloc(line, cap); }
            memcpy(line, first, first_len);
            len = first_len;
            while (c != '\n') {
                c = getchar();
                if (c == EOF) break;
                if (c != '\r') {
                    if (len + 1 >= cap) { cap = len + 16; line = (char *)realloc(line, cap); }
                    line[len++] = (char)c;
                }
            }
            line[len] = 0;
            return line;
        }
    }
    (void)len;
}

/* ============ Streamable HTTP transport（--http <port> 模式） ============ */
/* 极简实现：每连接一线程、Connection: close、无状态（不返回 Mcp-Session-Id） */
typedef struct {
    char method[16];
    char path[256];
    char headers[8192];
} HttpReq;

/* 从原始 header 文本中查找 header 值，写入调用方 buffer（线程安全） */
static char *http_header(const char *headers, const char *name, char *out, int outlen) {
    const char *p = headers;
    size_t nlen = strlen(name);
    while (*p) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end) line_end = p + strlen(p);
        size_t llen = (size_t)(line_end - p);
        if (llen > nlen && _strnicmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = (size_t)(line_end - v);
            if ((int)vl >= outlen) vl = outlen - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            if (vl && out[vl - 1] == '\r') out[vl - 1] = 0;
            return out;
        }
        p = line_end + 2;
    }
    return NULL;
}

static void http_send(SOCKET s, int status, const char *status_text,
                      const char *content_type, const char *body, int body_len,
                      const char *origin, const char *allow) {
    char head[2048];
    int n = 0;
    n += sprintf(head + n, "HTTP/1.1 %d %s\r\n", status, status_text);
    if (content_type) n += sprintf(head + n, "Content-Type: %s\r\n", content_type);
    if (allow && *allow) n += sprintf(head + n, "Allow: %s\r\n", allow);
    if (origin && *origin) {
        n += sprintf(head + n, "Access-Control-Allow-Origin: %s\r\n", origin);
    } else {
        n += sprintf(head + n, "Access-Control-Allow-Origin: *\r\n");
    }
    n += sprintf(head + n, "Access-Control-Allow-Headers: content-type, authorization, mcp-session-id, mcp-protocol-version, accept, origin\r\n");
    n += sprintf(head + n, "Access-Control-Allow-Methods: POST, OPTIONS\r\n");
    n += sprintf(head + n, "Access-Control-Max-Age: 86400\r\n");
    n += sprintf(head + n, "Connection: close\r\n");
    if (body && body_len > 0) n += sprintf(head + n, "Content-Length: %d\r\n", body_len);
    n += sprintf(head + n, "\r\n");
    send(s, head, n, 0);
    if (body && body_len > 0) send(s, body, body_len, 0);
}

static const char *g_token = NULL;

static DWORD WINAPI http_client_thread(LPVOID arg) {
    SOCKET s = (SOCKET)(intptr_t)arg;
    char buf[16384];
    int total = 0;
    int header_end = -1;
    int content_length = 0;

    while (total < (int)sizeof(buf) - 1) {
        int rd = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (rd <= 0) break;
        total += rd;
        buf[total] = 0;
        char *he = strstr(buf, "\r\n\r\n");
        if (he) {
            header_end = (int)(he - buf);
            /* content-length 头名大小写不敏感：VS Code/undici 发全小写头 */
            for (char *p = buf; p < he; ) {
                char *le = strstr(p, "\r\n");
                if (!le || le > he) le = he;
                if (le - p > 15 && _strnicmp(p, "content-length", 14) == 0 && p[14] == ':') {
                    content_length = atoi(p + 15);
                    break;
                }
                p = (le == he) ? he + 1 : le + 2;
            }
            break;
        }
    }
    if (header_end < 0) { closesocket(s); return 0; }

    HttpReq req;
    memset(&req, 0, sizeof(req));
    char *sp1 = strchr(buf, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2) { closesocket(s); return 0; }
    int mlen = (int)(sp1 - buf);
    if (mlen >= (int)sizeof(req.method)) mlen = (int)sizeof(req.method) - 1;
    memcpy(req.method, buf, mlen); req.method[mlen] = 0;
    int plen = (int)(sp2 - sp1 - 1);
    if (plen >= (int)sizeof(req.path)) plen = (int)sizeof(req.path) - 1;
    memcpy(req.path, sp1 + 1, plen); req.path[plen] = 0;
    int hlen = header_end;
    if (hlen >= (int)sizeof(req.headers)) hlen = (int)sizeof(req.headers) - 1;
    memcpy(req.headers, buf, hlen); req.headers[hlen] = 0;

    char origin_buf[256] = "";
    http_header(req.headers, "origin", origin_buf, sizeof(origin_buf));

    if (strcmp(req.method, "OPTIONS") == 0) {
        http_send(s, 204, "No Content", NULL, NULL, 0, origin_buf, "POST, OPTIONS");
        closesocket(s);
        return 0;
    }
    if (strcmp(req.method, "POST") != 0) {
        /* GET 流不支持：405 + Allow 头（Streamable HTTP 规范要求，客户端据此回退 POST-only） */
        const char *e = "{\"error\":\"method not allowed\"}";
        http_send(s, 405, "Method Not Allowed", "application/json", e, (int)strlen(e), origin_buf, "POST, OPTIONS");
        closesocket(s);
        return 0;
    }

    if (g_token && *g_token) {
        char auth_buf[512] = "";
        http_header(req.headers, "authorization", auth_buf, sizeof(auth_buf));
        char expected[512];
        snprintf(expected, sizeof(expected), "Bearer %s", g_token);
        if (strcmp(auth_buf, expected) != 0) {
            const char *e = "{\"error\":\"unauthorized\"}";
            http_send(s, 401, "Unauthorized", "application/json", e, (int)strlen(e), origin_buf, NULL);
            closesocket(s);
            return 0;
        }
    }

    if (content_length > 0) {
        char *body = (char *)malloc(content_length + 1);
        int copied = 0;
        int have = total - header_end - 4;
        if (have > 0) {
            int take = have < content_length ? have : content_length;
            memcpy(body, buf + header_end + 4, take);
            copied = take;
        }
        while (copied < content_length) {
            int rd = recv(s, body + copied, content_length - copied, 0);
            if (rd <= 0) break;
            copied += rd;
        }
        body[copied] = 0;

        /* 带 progressToken 的 tools/call：先开 SSE 流（无 Content-Length，关闭即结束），
           命令执行期间 progress 事件直接写本连接，最后再发最终 result */
        int want_progress = 0;
        {
            Json *pm = json_parse(body);
            if (pm) {
                Json *mth = j_get(pm, "method");
                if (mth && mth->type == 2 && strcmp(mth->str, "tools/call") == 0) {
                    Json *pp = j_get(pm, "params");
                    Json *meta = pp ? j_get(pp, "_meta") : NULL;
                    if (meta && j_get(meta, "progressToken")) want_progress = 1;
                }
                j_free(pm);
            }
        }
        if (want_progress) {
            int nodelay = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
            http_send(s, 200, "OK", "text/event-stream", NULL, 0, origin_buf, NULL);
            g_streaming = 1;
            g_stream_sock = s;
        }

        g_http_resp.buf = NULL; g_http_resp.len = 0; g_http_resp.cap = 0;
        handle_message(body);
        if (g_http_resp.len > 0) {
            /* VS Code 客户端要求 SSE（text/event-stream）格式响应 */
            char *sse = (char *)malloc(g_http_resp.len + 64);
            int sn = sprintf(sse, "event: message\ndata: %s\n\n", g_http_resp.buf);
            if (want_progress) send(s, sse, sn, 0); /* 响应头已随流先行发出，这里只追加事件 */
            else http_send(s, 200, "OK", "text/event-stream", sse, sn, origin_buf, NULL);
            free(sse);
        } else if (!want_progress) {
            /* notification 等无响应消息：202 Accepted 无 body */
            http_send(s, 202, "Accepted", NULL, NULL, 0, origin_buf, NULL);
        }
        g_streaming = 0;
        free(g_http_resp.buf);
        free(body);
    } else {
        http_send(s, 200, "OK", "application/json", "", 0, origin_buf, NULL);
    }
    closesocket(s);
    return 0;
}

static DWORD g_parent_pid = 0;

/* 父进程看护：--parent-pid 指定的进程（扩展 host）退出后本服务自动退出，
   避免 VS Code 关闭/卸载/崩溃后留下占用端口的孤儿进程 */
static DWORD WINAPI parent_watchdog(LPVOID arg) {
    (void)arg;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, g_parent_pid);
    if (!h) ExitProcess(1); /* 父进程已不存在，无需看护 */
    for (;;) {
        DWORD w = WaitForSingleObject(h, 2000);
        if (w == WAIT_OBJECT_0) { CloseHandle(h); ExitProcess(0); } /* 父进程已退出 */
        if (w == WAIT_FAILED) { CloseHandle(h); ExitProcess(1); }
    }
    return 0;
}

static int http_main(int port, const char *token, DWORD parent_pid) {
    g_http_mode = 1;
    g_token = token;
    if (parent_pid) {
        g_parent_pid = parent_pid;
        CreateThread(NULL, 0, parent_watchdog, NULL, 0, NULL);
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) { fprintf(stderr, "socket failed\n"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind port %d failed\n", port);
        return 1;
    }
    if (listen(ls, 16) != 0) { fprintf(stderr, "listen failed\n"); return 1; }
    fprintf(stderr, "win-exec-mcp HTTP listening on port %d (token: %s)\n", port, g_token ? "set" : "none");
    for (;;) {
        SOCKET cl = accept(ls, NULL, NULL);
        if (cl == INVALID_SOCKET) continue;
        CreateThread(NULL, 0, http_client_thread, (LPVOID)(intptr_t)cl, 0, NULL);
    }
    WSACleanup();
    return 0;
}

int main(int argc, char **argv) {
    detect_git_bash();
    if (argc >= 3 && strcmp(argv[1], "--http") == 0) {
        const char *token = NULL;
        DWORD parent_pid = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) token = argv[i + 1];
            else if (strcmp(argv[i], "--parent-pid") == 0 && i + 1 < argc) parent_pid = (DWORD)atol(argv[i + 1]);
        }
        return http_main(atoi(argv[2]), token, parent_pid);
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    for (;;) {
        char *msg = read_message();
        if (!msg) break;
        if (msg[0]) handle_message(msg);
    }
    return 0;
}
