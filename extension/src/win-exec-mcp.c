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

/* 版本号由构建脚本注入（build.cmd 传 -DWINEXEC_VERSION=\"x.y.z\"）；手工编译时退化为 dev。
 * serverInfo.version 原样上报它——这是远端判断“实际在跑哪一版”的唯一途径。 */
#ifndef WINEXEC_VERSION
#define WINEXEC_VERSION "dev"
#endif

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601 /* Job Object / RegGetValue 等 API 可用性 */
#include <winsock2.h>
#include <windows.h>
#include <winreg.h>
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

/* ============ git-bash 探测（多级通用发现；绝不回退 System32\bash.exe=WSL） ============
   发现顺序（与具体机器无关，靠环境变量/注册表自适配）：
   0) WINEXEC_GITBASH 环境变量显式覆盖（便携版等特殊安装）
   1) 注册表 GitForWindows\InstallPath（安装器写入：HKLM/HKCU，含 WOW6432）
   2) 常见安装目录：ProgramFiles / ProgramFiles(x86) / LocalAppData\Programs / Scoop
   3) PATH 派生：git.exe 位于 ...\Git\cmd\ 时推 ...\Git\bin\bash.exe；
      bash.exe 直接在 PATH 时仅接受路径含 \Git\ 的（排除 System32/WindowsApps 的 WSL） */
static char g_bash_path[MAX_PATH * 2]; /* 空 = 未找到 */

static int file_exists_a(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int accept_bash(const char *p) {
    if (!file_exists_a(p)) return 0;
    strncpy(g_bash_path, p, sizeof(g_bash_path) - 1);
    g_bash_path[sizeof(g_bash_path) - 1] = 0;
    return 1;
}

/* 注册表 HKEY\SOFTWARE\GitForWindows\InstallPath → <path>\bin\bash.exe */
static int try_reg_hive(HKEY root, const char *subkey) {
    char val[MAX_PATH * 2];
    DWORD sz = sizeof(val), type = 0;
    if (RegGetValueA(root, subkey, "InstallPath", RRF_RT_REG_SZ, &type, val, &sz) != ERROR_SUCCESS)
        return 0;
    int l = (int)strlen(val);
    while (l > 0 && (val[l - 1] == '\\' || val[l - 1] == '/')) val[--l] = 0;
    if (!l) return 0;
    char cand[MAX_PATH * 2];
    snprintf(cand, sizeof(cand), "%s\\bin\\bash.exe", val);
    return accept_bash(cand);
}

static int contains_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (_strnicmp(p, needle, nl) == 0) return 1;
    return 0;
}

static int detect_try_all(void) {
    char cand[MAX_PATH * 2];
    char found[MAX_PATH * 2];

    /* 0) 显式覆盖 */
    const char *ov = getenv("WINEXEC_GITBASH");
    if (ov && ov[0] && accept_bash(ov)) return 1;

    /* 1) 注册表（安装器写入的最权威来源） */
    if (try_reg_hive(HKEY_LOCAL_MACHINE, "SOFTWARE\\GitForWindows")) return 1;
    if (try_reg_hive(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\GitForWindows")) return 1;
    if (try_reg_hive(HKEY_CURRENT_USER, "SOFTWARE\\GitForWindows")) return 1;
    if (try_reg_hive(HKEY_CURRENT_USER, "SOFTWARE\\WOW6432Node\\GitForWindows")) return 1;

    /* 2) 常见安装目录 */
    const char *envs[3] = { "ProgramFiles", "ProgramFiles(x86)", "LocalAppData" };
    for (int i = 0; i < 3; i++) {
        const char *dir = getenv(envs[i]);
        if (!dir || !dir[0]) continue;
        if (strcmp(envs[i], "LocalAppData") == 0)
            snprintf(cand, sizeof(cand), "%s\\Programs\\Git\\bin\\bash.exe", dir);
        else
            snprintf(cand, sizeof(cand), "%s\\Git\\bin\\bash.exe", dir);
        if (accept_bash(cand)) return 1;
    }
    const char *up = getenv("USERPROFILE");
    if (up && up[0]) {
        snprintf(cand, sizeof(cand), "%s\\scoop\\apps\\git\\current\\bin\\bash.exe", up);
        if (accept_bash(cand)) return 1;
    }

    /* 3) PATH 派生 */
    DWORD sp = SearchPathA(NULL, "git.exe", NULL, (DWORD)sizeof(found), found, NULL);
    if (sp > 0 && sp < (DWORD)sizeof(found)) {
        size_t l = strlen(found);
        if (l > 12 && _stricmp(found + l - 12, "\\cmd\\git.exe") == 0) {
            found[l - 12] = 0; /* 去掉 \cmd\git.exe */
            snprintf(cand, sizeof(cand), "%s\\bin\\bash.exe", found);
            if (accept_bash(cand)) return 1;
        }
    }
    sp = SearchPathA(NULL, "bash.exe", NULL, (DWORD)sizeof(found), found, NULL);
    if (sp > 0 && sp < (DWORD)sizeof(found) && contains_ci(found, "\\git\\")) {
        if (accept_bash(found)) return 1; /* 仅接受 Git 目录下的 bash（排除 WSL） */
    }

    return 0;
}

static void detect_git_bash(void) {
    g_bash_path[0] = 0;
    detect_try_all();
    if (getenv("WINEXEC_DEBUG")) /* 诊断开关：设了就能在日志里看到探到了什么 */
        fprintf(stderr, "[win-exec-mcp] git-bash: %s\n", g_bash_path[0] ? g_bash_path : "(not found)");
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
static __thread DWORD  g_act_last_ms = 0; /* 上次 [act] 输出活动信号时间（100ms 节流） */
static __thread DWORD  g_p_partial_ms = 0;/* 上次补发部分行的时间 */
static __thread int    g_streaming = 0;   /* HTTP：本次响应已开始 SSE 流式 */
static __thread SOCKET g_stream_sock = 0; /* HTTP：流式响应所用连接 */
static __thread SOCKET g_conn_sock = 0;   /* HTTP：本次请求的连接（未流式时也能探活） */
static __thread DWORD  g_probe_last_ms = 0; /* 上次探活时间（空闲心跳限流） */
static __thread char   g_cur_reqid[80];   /* 当前 tools/call 的 request id（取消匹配用） */
static __thread int    g_client_gone = 0; /* 发送失败/管道错误 → 客户端已走，应终止命令 */
static __thread char  *g_prog_last = NULL;/* 上一条进度消息（相邻去重） */
static __thread int    g_prog_last_cap = 0;
/* 可调阈值（环境变量覆盖，见 load_env_config）。
   进度参数默认放宽（0.3.9 起 30ms / 60000B）：常见输出速率下每窗口能覆盖全部新行，
   不再频繁出现 "…[跳过 N 行]…"；要更保守可用环境变量调小/调慢。 */
static int g_prog_ms = 30;              /* WINEXEC_PROGRESS_MS     进度最小间隔毫秒 */
static int g_probe_ms = 5000;           /* WINEXEC_PROBE_MS        客户端探活间隔毫秒 */
static int g_prog_bytes = 60000;        /* WINEXEC_PROGRESS_BYTES  单条进度上限字节 */
static int g_spill_bytes = 512 * 1024;  /* WINEXEC_MAX_RESULT_BYTES 结果超此值落盘（0=关） */
static int g_spill_keep = 20;           /* WINEXEC_SPILL_KEEP      落盘目录保留个数（0=不按个数清理） */
static int g_spill_keep_days = 7;       /* WINEXEC_SPILL_KEEP_DAYS 落盘文件最长保留天数（0=不按天数清理） */
static LONG g_spill_seq = 0;            /* 同毫秒并发序号：文件名唯一化（HTTP 每连接一线程，防同秒覆盖） */

/* ============ 可调阈值（环境变量，启动时读取） ============ */
static int env_int(const char *name, int def, int lo, int hi) {
    const char *s = getenv(name);
    if (!s || !s[0]) return def;
    int v = atoi(s);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}
static void load_env_config(void) {
    g_prog_ms = env_int("WINEXEC_PROGRESS_MS", 30, 30, 10000);
    g_probe_ms = env_int("WINEXEC_PROBE_MS", 5000, 500, 60000);
    g_prog_bytes = env_int("WINEXEC_PROGRESS_BYTES", 60000, 200, 60000);
    g_spill_bytes = env_int("WINEXEC_MAX_RESULT_BYTES", 512 * 1024, 0, 1 << 30);
    g_spill_keep = env_int("WINEXEC_SPILL_KEEP", 20, 0, 100000);
    g_spill_keep_days = env_int("WINEXEC_SPILL_KEEP_DAYS", 7, 0, 3650);
}

static int progress_begin(Json *params) {
    g_ptok[0] = 0; g_ptok_num = 0; g_ptok_numval = 0; g_pseq = 0; g_p_last_ms = 0;
    g_p_partial_ms = 0;
    g_client_gone = 0;
    if (g_prog_last) g_prog_last[0] = 0;
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

/* 完整发送（大缓冲区可能会分多次写；出错返回 0） */
static int send_all(SOCKET s, const char *buf, int len) {
    int off = 0;
    while (off < len) {
        int n = send(s, buf + off, len - off, 0);
        if (n <= 0) return 0;
        off += n;
    }
    return 1;
}

/* 客户端存活探测。
   长驻低输出命令没有新输出 → 没有 progress 可发 → 「写失败=客户端已走」这个唯一的断连
   信号永远不会被触发，客户端被 kill -9 / SSH 掉线后进程树会一直残留（典型后果：串口被占死）。
   这里在等待子进程的空转里定期主动探活：
   HTTP - 对端已关闭（FIN/RST）：套接字可读且 MSG_PEEK 读到 0；流式连接再写一条 SSE 注释行
          （规范要求客户端忽略，兼防代理/隧道空闲超时），写失败即断开；
   stdio - 客户端的两个管道都已断：stdin 读到 EOF（PeekNamedPipe 报管道已断）**且** stdout
          写不进去（0 字节 WriteFile 失败）。两个条件缺一不可——只断 stdin 是
          `echo … | win-exec-mcp.exe` 这种一次性调用（还能通过 stdout 拿到结果，不能杀）。 */
static int client_probe(void) {
    DWORD now = GetTickCount();
    if (g_probe_last_ms && now - g_probe_last_ms < (DWORD)g_probe_ms) return 0;
    g_probe_last_ms = now;
    if (!g_http_mode) {
        /* 必须先确认是管道：手工从控制台运行（或 stdin 重定向到文件）时 PeekNamedPipe 同样会失败，
           但那不是断连。文件/控制台句柄一律忽略。 */
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        if (!hin || hin == INVALID_HANDLE_VALUE || GetFileType(hin) != FILE_TYPE_PIPE) return 0;
        DWORD avail = 0;
        if (PeekNamedPipe(hin, NULL, 0, NULL, &avail, NULL)) return 0;
        DWORD err = GetLastError();
        if (err != ERROR_BROKEN_PIPE && err != ERROR_PIPE_NOT_CONNECTED && err != ERROR_NO_DATA) return 0;
        /* stdin 已关，但 stdout 还活着 → 结果仍送得出去（一次性调用），不算断连 */
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!hout || hout == INVALID_HANDLE_VALUE || GetFileType(hout) != FILE_TYPE_PIPE) return 0;
        DWORD written = 0;
        if (WriteFile(hout, "", 0, &written, NULL)) return 0;
        return 1;
    }
    if (g_conn_sock) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(g_conn_sock, &rf);
        struct timeval tv = { 0, 0 };
        if (select(0, &rf, NULL, NULL, &tv) > 0) {
            char tmp[256];
            int n = recv(g_conn_sock, tmp, sizeof(tmp), MSG_PEEK);
            if (n == 0) return 1;                                       /* 对端已关闭 */
            if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) return 1; /* 连接已失效 */
        }
    }
    if (g_streaming && g_stream_sock) {
        static const char sse_ka[] = ": keepalive\n\n";
        if (!send_all(g_stream_sock, sse_ka, (int)sizeof(sse_ka) - 1)) return 1;
    }
    return 0;
}

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
            if (!send_all(g_stream_sock, sse, sn)) g_client_gone = 1; /* 连接已断 → 终止命令 */
            free(sse);
        }
    } else {
        printf("%s\n", s);
        fflush(stdout);
        if (ferror(stdout)) g_client_gone = 1; /* 管道已断 → 终止命令 */
    }
    free(s);
    j_free(n);
}

/* 原始输出 → UTF-8，**逐行判定编码**：每行单独试 UTF-8，不合法才按 GBK 转。
   整段只判一次时，混用两种编码源的输出必然坏一半（cmd 内建命令走 GBK，而 git/node/python
   等工具走 UTF-8）。按行判定后两种都能正确解码；代价是启发式：极少数 GBK 行的字节恰好
   构成合法 UTF-8 时会被漏转、显示为乱码。
   strip_cr=1 时去掉行尾 \r（进度逐行展示不需要 CRLF）。
   注：\n (0x0A) 不可能出现在 GBK/UTF-8 的多字节序列内部，按行切分是安全的。 */
static char *to_utf8_lines(const char *s, int len, int strip_cr) {
    if (len <= 0) return NULL;
    int cap = len * 3 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    int olen = 0, i = 0;
    while (i < len) {
        int le = i;
        while (le < len && s[le] != '\n') le++;
        int seg = le - i;
        if (strip_cr && seg > 0 && s[i + seg - 1] == '\r') seg--;
        if (seg > 0) {
            char *u = NULL;
            int ulen = seg;
            if (!utf8_valid(s + i, seg)) {
                u = gbk_to_utf8(s + i, seg);
                if (u) ulen = (int)strlen(u);
            }
            const char *src = u ? u : s + i;
            if (olen + ulen + 2 > cap) {
                cap = (olen + ulen + 32) * 2;
                out = (char *)realloc(out, cap);
            }
            memcpy(out + olen, src, ulen);
            olen += ulen;
            if (u) free(u);
        }
        if (le < len) out[olen++] = '\n';
        i = le + 1;
    }
    out[olen] = 0;
    return out;
}

/* 组装一条进度消息：按"完整行"取窗口（最近若干行，装进 budget），
   跳过的内容前置标注 "…[跳过 N 行]…"；单行超长时折叠为 "行首 …[省略 N B]… 行尾"。
   返回 malloc 的 UTF-8 文本；无可发内容返回 NULL。 */
static char *build_progress_msg(const char *raw, int rstart, int rend, int budget) {
    while (rend > rstart && (raw[rend - 1] == '\n' || raw[rend - 1] == '\r')) rend--;
    if (rend <= rstart) return NULL;
    int reserve = 48; /* 标注预留 */

    /* 整段放得下：直接转换 */
    if (rend - rstart <= budget) return to_utf8_lines(raw + rstart, rend - rstart, 1);

    /* 从底部向上收"完整行"，直到再加一行就超过预算 */
    int keep_start = rend;
    int e = rend;
    while (e > rstart) {
        int ls = e;
        while (ls > rstart && raw[ls - 1] != '\n') ls--;
        if (rend - ls > budget - reserve) break; /* 本行放不进 → 停（本行及其上全部跳过） */
        keep_start = ls;
        if (ls == rstart) break;
        e = ls - 1; /* 跳过分隔换行，继续向上一行 */
    }

    if (keep_start == rend) {
        /* 底部整行超预算：折叠该行头尾 */
        int ls = rend;
        while (ls > rstart && raw[ls - 1] != '\n') ls--;
        char *u = to_utf8_lines(raw + ls, rend - ls, 1);
        if (!u) return NULL;
        int ul = (int)strlen(u);
        int inner = budget - reserve - 40;
        if (inner < 64) inner = 64;
        int head = inner / 2, tail = inner - head;
        int hl = head;
        while (hl > 0 && hl < ul && ((unsigned char)u[hl] & 0xC0) == 0x80) hl--;
        int ts = ul - tail;
        if (ts < hl) ts = hl;
        while (ts < ul && ((unsigned char)u[ts] & 0xC0) == 0x80) ts++;
        int dlines = 0;
        for (int i = rstart; i < ls; i++) if (raw[i] == '\n') dlines++;
        if (ls > rstart && dlines == 0) dlines = 1;
        char *msg = (char *)malloc(ul + 256);
        int n = 0;
        if (dlines > 0) n += sprintf(msg + n, "…[跳过 %d 行]…\n", dlines);
        n += sprintf(msg + n, "%.*s …[省略 %d B]… %s", hl, u, ts - hl, u + ts);
        free(u);
        return msg;
    }

    /* 常规窗口：转换 [keep_start, rend) 并前置跳过标注 */
    char *u = to_utf8_lines(raw + keep_start, rend - keep_start, 1);
    if (!u) return NULL;
    if (keep_start <= rstart) return u;
    int dlines = 0;
    for (int i = rstart; i < keep_start; i++) if (raw[i] == '\n') dlines++;
    if (dlines == 0) dlines = 1;
    char *msg = (char *)malloc(strlen(u) + 96);
    sprintf(msg, "…[跳过 %d 行]…\n%s", dlines, u);
    free(u);
    return msg;
}

/* run_cmd 每读到一段输出后调用：把 [*emitted, olen) 内"最后一个换行前"的新内容发出去。
   force=1（命令结束）时无换行也发、无视限流。窗口内只保留完整行，跳过/折叠均有标注。 */
static void progress_scan(const char *out, int olen, int *emitted, int force) {
    /* 输出活动信号（与 progressToken 无关）：扩展据此把状态栏图标“点亮”（闪电 + 蓝底），
       250ms 无新输出自动回落成空心。100ms 节流 ⇒ 密集输出时是常亮、间歇输出时跟着数据闪；
       静默期不发 ⇒ 保持空心（一眼看出没有输出） */
    if (g_http_mode && olen > *emitted) {
        DWORD anow = GetTickCount();
        if (anow - g_act_last_ms >= 100) {
            g_act_last_ms = anow;
            fprintf(stderr, "[act]\n");
            fflush(stderr);
        }
    }
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
    if (!force && now - g_p_last_ms < (DWORD)g_prog_ms) return; /* 限流 */
    g_p_last_ms = now;
    char *msg = build_progress_msg(out, *emitted, end, g_prog_bytes);
    *emitted = end;
    if (!msg) return;
    if (msg[0]) {
        /* 相邻重复去重（高同构输出常见） */
        if (!g_prog_last || strcmp(msg, g_prog_last) != 0) {
            progress_emit(msg);
            int ml = (int)strlen(msg);
            if (ml + 1 > g_prog_last_cap) {
                g_prog_last_cap = ml + 64;
                g_prog_last = (char *)realloc(g_prog_last, g_prog_last_cap);
            }
            memcpy(g_prog_last, msg, ml + 1);
        }
    }
    free(msg);
}

/* 静默期的"部分行"补发（tqdm 之类用 \r 刷新的进度条、长时间不换行的输出）：
   没有完整新行时 progress_scan 一直不发，UI 全程无反馈。这里在"距上次补发 ≥2s
   且还有未发出的字节"时把尾部当作一条进度发出；不推进 emitted ⇒ 该行真正写完时
   仍会按正常进度再发一次完整内容。 */
static void progress_scan_partial(const char *out, int olen, int emitted) {
    if (!progress_on() || olen <= emitted) return;
    DWORD now = GetTickCount();
    if (now - g_p_partial_ms < 2000) return;
    g_p_partial_ms = now;
    char *msg = build_progress_msg(out, emitted, olen, g_prog_bytes);
    if (!msg) return;
    if (msg[0] && (!g_prog_last || strcmp(msg, g_prog_last) != 0)) {
        progress_emit(msg);
        int ml = (int)strlen(msg);
        if (ml + 1 > g_prog_last_cap) {
            g_prog_last_cap = ml + 64;
            g_prog_last = (char *)realloc(g_prog_last, g_prog_last_cap);
        }
        memcpy(g_prog_last, msg, ml + 1);
    }
    free(msg);
}

/* ---- 落盘目录卫生（0.3.11）：只清理本程序命名的 out-*.log / bg-*.log，best-effort、失败静默 ----
   时机：每次写入后 lazy 执行（服务是常驻/多窗口进程，没有可靠的退出时机；
   且文件是要留给模型/人后续 grep 的，不能早删）。策略：
   - keep > 0：最多保留最近 keep 个（含刚写入的 cur，它永不删除）；
   - days > 0：删除修改时间早于 days 天的。
   文件名以 <prefix>-YYYYMMDD-HHMMSS-mmm-pid-seq.log 开头 → 字典序即时间序，排序即可。 */
typedef struct { char name[64]; FILETIME ft; } SpillEnt;
static int spill_ent_cmp(const void *a, const void *b) {
    return strcmp(((const SpillEnt *)a)->name, ((const SpillEnt *)b)->name);
}
static void spill_sweep(const char *dir, const char *cur, int keep, int days) {
    if (keep <= 0 && days <= 0) return;
    SpillEnt *ents = NULL;
    int n = 0, cap = 0;
    /* 两轮：out-*（工具结果落盘）与 bg-*（后台作业输出）同池回收 */
    static const char *pats[2] = { "out-*.log", "bg-*.log" };
    for (int pi = 0; pi < 2; pi++) {
        char pat[1200];
        snprintf(pat, sizeof(pat), "%s\\%s", dir, pats[pi]);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                SpillEnt *t = (SpillEnt *)realloc(ents, sizeof(SpillEnt) * cap);
                if (!t) break;
                ents = t;
            }
            strncpy(ents[n].name, fd.cFileName, sizeof(ents[n].name) - 1);
            ents[n].name[sizeof(ents[n].name) - 1] = 0;
            ents[n].ft = fd.ftLastWriteTime;
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!ents) return;
    ULONGLONG cut = 0;
    if (days > 0) {
        FILETIME nowft;
        GetSystemTimeAsFileTime(&nowft);
        ULONGLONG now100 = ((ULONGLONG)nowft.dwHighDateTime << 32) | nowft.dwLowDateTime;
        ULONGLONG span = (ULONGLONG)days * 864000000000ULL; /* 100ns 单位/天 */
        cut = now100 > span ? now100 - span : 0;
    }
    qsort(ents, n, sizeof(SpillEnt), spill_ent_cmp);
    int drop_oldest = (keep > 0 && n > keep) ? n - keep : 0;
    for (int i = 0; i < n; i++) {
        int drop = (i < drop_oldest);
        if (!drop && cut) {
            ULONGLONG t = ((ULONGLONG)ents[i].ft.dwHighDateTime << 32) | ents[i].ft.dwLowDateTime;
            drop = (t < cut);
        }
        if (!drop) continue;
        char full[2048];
        snprintf(full, sizeof(full), "%s\\%s", dir, ents[i].name);
        if (cur && _stricmp(full, cur) == 0) continue; /* 绝不删刚写入的文件 */
        DeleteFileA(full);
    }
    free(ents);
}

/* 分配一个 %TEMP%\win-exec-mcp\<prefix>-*.log 路径（目录不存在则创建；失败返回 0） */
static int spill_make_path_ex(char *path, int path_sz, const char *prefix) {
    char dir[1024];
    const char *tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = getenv("TMP");
    if (!tmp || !tmp[0]) return 0;
    snprintf(dir, sizeof(dir), "%s\\win-exec-mcp", tmp);
    CreateDirectoryA(dir, NULL);
    SYSTEMTIME st;
    GetLocalTime(&st);
    /* 毫秒 + PID + 原子序号：同秒并发（HTTP 每连接一线程）也不会互相覆盖 */
    long seq = (long)InterlockedIncrement(&g_spill_seq);
    snprintf(path, path_sz, "%s\\%s-%04d%02d%02d-%02d%02d%02d-%03d-%lu-%ld.log", dir, prefix,
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             (unsigned long)GetCurrentProcessId(), seq);
    return 1;
}
static int spill_make_path(char *path, int path_sz) { return spill_make_path_ex(path, path_sz, "out"); }

/* 按路径所在目录做落盘清理（供"运行中溢出"复用；文件名模式与结果落盘一致） */
static void spill_sweep_for(const char *path) {
    char dir[1024];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *bs = strrchr(dir, '\\');
    if (!bs) return;
    *bs = 0;
    spill_sweep(dir, path, g_spill_keep, g_spill_keep_days);
}

/* 读落盘文件的末尾，生成"标注 + 末尾 ~200 行"文本（失败返回 NULL）。
   从首个换行后开始取：既是行边界，也避开截断处的半个多字节字符
   （0x0A 不可能出现在 GBK/UTF-8 多字节序列内部）。 */
static char *spill_tail_text(const char *path, const char *note) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return NULL; }
    long long total = sz.QuadPart;
    int want = total > 65536 ? 65536 : (int)total;
    LARGE_INTEGER pos;
    pos.QuadPart = total - want;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    char *buf = (char *)malloc(want + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD got = 0;
    ReadFile(h, buf, (DWORD)want, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    char *start = buf;
    if (want < total) {
        char *nl = (char *)memchr(buf, '\n', got);
        /* 按行边界切（避免从半个多字节字符开始）；但整行超长时切完可能什么都不剩，
           这时退回原始窗口，只按 UTF-8 连续字节修剪。 */
        if (nl && (nl + 1 - buf) <= got - 4096) start = nl + 1;
        else while (start < buf + got && ((unsigned char)*start & 0xC0) == 0x80) start++;
    }
    int sl = (int)strlen(start), cut = 0, seen = 0;
    for (int i = sl - 1; i >= 0; i--) {
        if (start[i] == '\n' && ++seen > 200) { cut = i + 1; break; }
    }
    char *u = to_utf8_lines(start + cut, sl - cut, 0);
    const char *tail = u ? u : start + cut;
    char *out = (char *)malloc(strlen(tail) + strlen(path) + strlen(note) + 256);
    if (out)
        sprintf(out, "[%s：%lld 字节；完整日志已存盘：%s（可用 windows_exec 读取/检索）]\n[以下为末尾 %d 字节]\n%s",
                note, total, path, (int)strlen(tail), tail);
    free(u);
    free(buf);
    return out;
}

/* 结果超过阈值：全文写 %TEMP%\win-exec-mcp\out-*.log，返回"标注 + 尾部"（失败返回 NULL 走原样） */
static char *spill_result(const char *text, const char *prefix, char **path_out) {
    char path[2048];
    if (!spill_make_path(path, sizeof(path))) return NULL;
    FILE *fp = fopen(path, "wb");
    if (!fp) return NULL;
    fwrite(text, 1, strlen(text), fp);
    fclose(fp);
    spill_sweep_for(path); /* 顺手回收旧文件（保留最近 N 个 / 过期天数） */
    if (path_out) *path_out = _strdup(path);
    /* 尾部：最后 ~200 行、且不超过 64KB */
    int total = (int)strlen(text);
    int cut = total > 65536 ? total - 65536 : 0;
    int j = total, seen = 0;
    while (j > cut) {
        j--;
        if (text[j] == '\n' && ++seen > 200) { j++; break; }
    }
    int tail_start = j;
    while (tail_start < total && ((unsigned char)text[tail_start] & 0xC0) == 0x80) tail_start++;
    char *out = (char *)malloc(total - tail_start + strlen(path) + 320);
    sprintf(out, "%s\n[输出过大：%d 字节；完整日志已存盘：%s（可用 windows_exec 读取/检索）]\n[以下为末尾 %d 字节]\n%s",
            prefix, total, path, total - tail_start, text + tail_start);
    return out;
}

/* ---- 回传体收缩（tail_lines）----
   只作用于“回传”的那一份；流式显示（progress）是另一条路，始终全量。
   与 Linux 侧 server.py 的 _tail_keep_spill_hint 同语义。 */

/* 复制 [p, p+len) 为独立字符串（失败返回 NULL） */
static char *dup_range(const char *p, size_t len) {
    char *s = (char *)malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, p, len);
    s[len] = '\0';
    return s;
}

/* 摘出“完整日志已存盘”那一行（无则 NULL） */
static char *spill_hint_line(const char *spill_text) {
    const char *h = strstr(spill_text, "完整日志已存盘");
    const char *s, *e;
    if (!h) return NULL;
    s = h; e = h;
    while (s > spill_text && s[-1] != '\n') s--;
    while (*e && *e != '\n') e++;
    return dup_range(s, (size_t)(e - s));
}

/* 只保留末尾 n 行（行数没超 n 则返回 NULL，调用方保持原文）。
   首行 [exit: ...] 前缀与 hint_line（落盘提示）始终保留 —— 截尾不能把线索截掉。 */
static char *tail_last_lines(const char *text, int n, const char *hint_line) {
    const char *nl, *body, *p, *ts;
    unsigned total = 1, i;
    int seen = 0, w = 0;
    size_t hl, tl, hlen;
    char *out;
    if (n <= 0) return NULL;
    nl = strchr(text, '\n');
    body = nl ? nl + 1 : text + strlen(text);
    for (i = 0; body[i]; i++) if (body[i] == '\n') total++;
    if ((int)total <= n) return NULL;
    p = body + strlen(body);
    ts = body;
    while (p > body) {
        p--;
        if (*p == '\n' && ++seen > n) { ts = p + 1; break; }
    }
    hl = (size_t)(body - text);                    /* 前缀行（含换行） */
    tl = strlen(ts);
    hlen = hint_line ? strlen(hint_line) + 1 : 0;
    out = (char *)malloc(hl + hlen + tl + 128);
    if (!out) return NULL;
    memcpy(out, text, hl);
    w = (int)hl;
    if (hlen) {
        int l = (int)(hlen - 1);
        memcpy(out + w, hint_line, l);
        w += l;
        out[w++] = '\n';
    }
    w += sprintf(out + w, "... (truncated, last %d of %u lines) ...\n", n, total);
    memcpy(out + w, ts, tl);
    out[w + (int)tl] = '\0';
    return out;
}

/* ============ 执行 Windows 命令（CreateProcess + 管道 + 超时） ============ */
typedef struct {
    char *output;   /* UTF-8 文本 */
    int exit_code;
    int timed_out;
    int spawn_err;
    int aborted;    /* 1=客户端取消 2=客户端断开（进程树已终止） */
    int truncated;  /* 输出超出内存上限（完整内容已落盘 spill_path） */
    char *spill_path; /* 落盘文件路径（无则 NULL） */
} RunResult;

#define MAX_OUTPUT (16 * 1024 * 1024) /* 内存缓冲上限；超出后改为边跑边写落盘文件，不再丢弃（仍需继续排空管道，防止子进程写满管道阻塞） */

/* 首行机器可读哨兵：所有结束路径（正常/超时/取消/断连/spawn 失败/落盘）共用同一 schema，
   下游不必再用正则猜；旧的文本前缀按原样跟在同一行后面，过渡期兼容既有解析。 */
static void json_escape_path(const char *src, char *dst, size_t cap) {
    size_t w = 0;
    if (!src) { if (cap >= 5) strcpy(dst, "null"); else if (cap) dst[0] = 0; return; }
    if (cap < 3) { if (cap) dst[0] = 0; return; }
    dst[w++] = '"';
    for (const char *p = src; *p && w + 3 < cap; p++) {
        if (*p == '\\' || *p == '"') dst[w++] = '\\';
        dst[w++] = *p;
    }
    dst[w++] = '"';
    dst[w] = 0;
}

static char *exit_json_line(const RunResult *rr, unsigned long dur, const char *shell_id, const char *prefix) {
    char code[24], log[2100];
    if (rr->spawn_err || rr->timed_out || rr->aborted) strcpy(code, "null");
    else sprintf(code, "%d", rr->exit_code);
    json_escape_path(rr->spill_path, log, sizeof(log));
    size_t cap = strlen(prefix) + strlen(log) + 320;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    snprintf(out, cap,
        "[exit-json: {\"code\":%s,\"ms\":%lu,\"shell\":\"%s\",\"timedOut\":%s,\"cancelled\":%s,"
        "\"clientGone\":%s,\"spawnError\":%s,\"spilled\":%s,\"log\":%s}] %s",
        code, dur, shell_id,
        rr->timed_out ? "true" : "false",
        rr->aborted == 1 ? "true" : "false",
        rr->aborted == 2 ? "true" : "false",
        rr->spawn_err ? "true" : "false",
        rr->spill_path ? "true" : "false",
        log, prefix);
    return out;
}

/* 把 text 的第一行换成 line1（其余字节原样保留，**含原首行末尾的换行**）；失败返回 NULL，调用方保持原文 */
static char *replace_first_line(char *text, const char *line1) {
    const char *nl = strchr(text, '\n');
    const char *rest = nl ? nl : "";
    size_t l1 = strlen(line1), rl = strlen(rest);
    char *out = (char *)malloc(l1 + rl + 1);
    if (!out) return NULL;
    memcpy(out, line1, l1);
    memcpy(out + l1, rest, rl + 1);
    return out;
}

/* 追加数据到输出缓冲，超出 MAX_OUTPUT 的部分丢弃（仅在落盘失败的回退路径上会发生） */
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

/* ---- 运行中溢出落盘：输出超过 MAX_OUTPUT 时把**完整内容**写文件，不再截断 ----
   之前超限部分直接丢（大日志/构建洪流会被截断且无处可寻）。现在首次越限就把内存里已有
   部分回灌进 %TEMP%\win-exec-mcp\out-*.log（与结果落盘同命名、同清理策略），此后全部走文件；
   结果改为"路径 + 末尾"。建文件失败时退回旧的"丢弃"行为。 */
typedef struct { HANDLE h; char path[2048]; int failed; long long bytes; } SpillSink;

static void collect_out(SpillSink *sink, char **out, int *olen, int *ocap,
                        const char *buf, int rd, int *truncated) {
    if (sink->h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(sink->h, buf, (DWORD)rd, &w, NULL);
        sink->bytes += w;
        return;
    }
    if (sink->failed || *olen + rd <= MAX_OUTPUT) {
        out_append(out, olen, ocap, buf, rd, truncated);
        return;
    }
    if (!spill_make_path(sink->path, sizeof(sink->path))) {
        sink->failed = 1;
        out_append(out, olen, ocap, buf, rd, truncated);
        return;
    }
    sink->h = CreateFileA(sink->path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (sink->h == INVALID_HANDLE_VALUE) {
        sink->failed = 1;
        sink->path[0] = 0;
        out_append(out, olen, ocap, buf, rd, truncated);
        return;
    }
    DWORD w = 0;
    if (*olen > 0) WriteFile(sink->h, *out, (DWORD)*olen, &w, NULL);
    sink->bytes = *olen;
    WriteFile(sink->h, buf, (DWORD)rd, &w, NULL);
    sink->bytes += rd;
    *truncated = 1;
    /* 此后进度不再随输出更新，提示一次（否则 UI 看起来卡住） */
    if (progress_on()) {
        char note[2300];
        snprintf(note, sizeof(note), "…[输出已超过 %d MB，完整内容改写入：%s（进度不再逐行更新）]…",
                 (int)(MAX_OUTPUT / (1024 * 1024)), sink->path);
        progress_emit(note);
    }
}

/* ============ 进程创建支持（env 环境块 / stdin 管道 / 共用 spawn） ============ */

typedef struct { HANDLE h; const char *data; int len; } StdinCtx;

/* stdin 喂数据线程：写完自动关写端；子进程提前退出/不读 stdin 时写失败即退（不阻塞调用方） */
static DWORD WINAPI stdin_writer_thread(LPVOID arg) {
    StdinCtx *c = (StdinCtx *)arg;
    DWORD done = 0;
    while (done < (DWORD)c->len) {
        DWORD wr = 0;
        if (!WriteFile(c->h, c->data + done, (DWORD)c->len - done, &wr, NULL) || wr == 0) break;
        done += wr;
    }
    CloseHandle(c->h);
    free(c);
    return 0;
}

static void blk_append(char **blk, int *len, int *cap, const char *p, int n) {
    if (*len + n + 1 > *cap) { *cap = (*len + n + 64) * 2; *blk = (char *)realloc(*blk, *cap); }
    memcpy(*blk + *len, p, n);
    *len += n;
}

/* 构造子进程环境块：继承当前环境 + env 对象覆盖/追加（null 值 = 删除该变量）。
   无 env 参数返回 NULL（=继承，不是失败）。条目在 UTF-8 域拼好再整体转 UTF-16；
   环境变量名大小写不敏感（Windows 语义），比较用 _strnicmp。 */
static wchar_t *build_env_block(Json *env_obj, int *failed) {
    *failed = 0;
    if (!env_obj || env_obj->type != 0 || env_obj->count <= 0) return NULL;
    for (int i = 0; i + 1 < env_obj->count; i += 2) {
        Json *k = env_obj->items[i];
        Json *v = env_obj->items[i + 1];
        if (!k || k->type != 2 || !k->str || !k->str[0] || strchr(k->str, '=')) { *failed = 1; return NULL; }
        if (v && (v->type == 2 || v->type == 5)) continue;
        *failed = 1;
        return NULL;
    }
    LPWCH parent = GetEnvironmentStringsW();
    if (!parent) { *failed = 1; return NULL; }
    char *blk = NULL;
    int len = 0, cap = 0;
    LPWCH p = parent;
    while (*p) {
        int wl = (int)wcslen(p);
        int ul = WideCharToMultiByte(CP_UTF8, 0, p, wl, NULL, 0, NULL, NULL);
        if (ul > 0) {
            char *u = (char *)malloc(ul + 1);
            if (!u) { FreeEnvironmentStringsW(parent); free(blk); *failed = 1; return NULL; }
            WideCharToMultiByte(CP_UTF8, 0, p, wl, u, ul, NULL, NULL);
            u[ul] = 0;
            char *eq = strchr(u, '=');
            int skip = 0;
            if (eq) {
                int klen = (int)(eq - u);
                for (int i = 0; i + 1 < env_obj->count; i += 2) {
                    const char *k = env_obj->items[i]->str;
                    if ((int)strlen(k) == klen && _strnicmp(k, u, klen) == 0) { skip = 1; break; }
                }
            }
            if (!skip) {
                blk_append(&blk, &len, &cap, u, ul);
                blk_append(&blk, &len, &cap, "\0", 1);
            }
            free(u);
        }
        p += wl + 1;
    }
    FreeEnvironmentStringsW(parent);
    for (int i = 0; i + 1 < env_obj->count; i += 2) {
        Json *k = env_obj->items[i];
        Json *v = env_obj->items[i + 1];
        if (v->type == 5) continue; /* null：仅删除，不追加 */
        blk_append(&blk, &len, &cap, k->str, (int)strlen(k->str));
        blk_append(&blk, &len, &cap, "=", 1);
        blk_append(&blk, &len, &cap, v->str, (int)strlen(v->str));
        blk_append(&blk, &len, &cap, "\0", 1);
    }
    blk_append(&blk, &len, &cap, "\0", 1); /* 环境块结尾双 NUL */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, blk, len, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(((size_t)(wlen > 0 ? wlen : 1) + 1) * sizeof(wchar_t));
    if (!w) { free(blk); *failed = 1; return NULL; }
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, blk, len, w, wlen);
    else w[0] = 0;
    w[wlen > 0 ? wlen : 0] = 0;
    free(blk);
    return w;
}

typedef struct { int ok; PROCESS_INFORMATION pi; HANDLE hOutR; HANDLE hInW; } SpawnResult;

/* 创建子进程（前台 run 与后台 bg 共用）：stdout/stderr 合到一个管道；with_stdin 时再加 stdin 管道。
   成功：hOutR = 父端读句柄（不可继承），hInW = stdin 写句柄（调用方负责写完关闭）。
   命令行是 UTF-8，必须走 W 版 API：CreateProcessA 会把字节串按 CP_ACP 解码，中文等
   非 ASCII 参数被错位重编码（git-bash 侧直接乱码，cmd 侧仅因 GBK 往返恰好还原）。
   句柄白名单只放行本函数创建的管道端，防止将来新增可继承句柄（如监听 socket）泄入子进程。 */
static SpawnResult spawn_child(const char *cmd_line_utf8, const char *cwd_utf8, wchar_t *env_block, int with_stdin) {
    SpawnResult sp;
    memset(&sp, 0, sizeof(sp));
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hOutR = NULL, hOutW = NULL, hInR = NULL, hInW = NULL;
    if (!CreatePipe(&hOutR, &hOutW, &sa, 0)) return sp;
    SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);
    if (with_stdin) {
        if (!CreatePipe(&hInR, &hInW, &sa, 0)) { CloseHandle(hOutR); CloseHandle(hOutW); return sp; }
        SetHandleInformation(hInW, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE inherit_list[2];
    int inherit_n = 0;
    if (with_stdin) inherit_list[inherit_n++] = hInR;
    inherit_list[inherit_n++] = hOutW;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    BOOL have_attrs = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    if (have_attrs) {
        have_attrs = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               inherit_list, inherit_n * sizeof(HANDLE), NULL, NULL);
    }
    STARTUPINFOEXW six = { 0 };
    six.StartupInfo.cb = sizeof(six);
    six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    six.StartupInfo.hStdInput = with_stdin ? hInR : 0;
    six.StartupInfo.hStdOutput = hOutW;
    six.StartupInfo.hStdError = hOutW;
    six.lpAttributeList = have_attrs ? attrs : NULL;
    STARTUPINFOW si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = with_stdin ? hInR : 0;
    si.hStdOutput = hOutW;
    si.hStdError = hOutW;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd_line_utf8, -1, NULL, 0);
    wchar_t *wcmdline = (wchar_t *)malloc((wlen > 0 ? wlen : 1) * sizeof(wchar_t));
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, cmd_line_utf8, -1, wcmdline, wlen);
    else wcmdline[0] = 0;
    /* cwd：显式目录（UTF-8）优先；否则服务自身目录，但 UNC 开头时退回系统盘符
       （cmd 继承 UNC cwd 会报"UNC 路径不受支持"）。 */
    wchar_t safe_cwd[MAX_PATH] = L"";
    if (cwd_utf8 && *cwd_utf8) {
        MultiByteToWideChar(CP_UTF8, 0, cwd_utf8, -1, safe_cwd, MAX_PATH);
        safe_cwd[MAX_PATH - 1] = 0;
    } else {
        wchar_t cwd_now[MAX_PATH] = L"";
        if (GetCurrentDirectoryW(MAX_PATH, cwd_now) && cwd_now[0] == L'\\' && cwd_now[1] == L'\\') {
            GetWindowsDirectoryW(safe_cwd, MAX_PATH);
            for (wchar_t *q = safe_cwd; *q; q++) if (*q == L'\\') { *q = 0; break; }
        }
    }
    /* CREATE_UNICODE_ENVIRONMENT：lpEnvironment 是 UTF-16 块时必须带此标志，
       否则系统按 ANSI 解析 UTF-16 块 → CreateProcessW 直接 err=87 */
    BOOL ok = CreateProcessW(NULL, wcmdline, NULL, NULL, TRUE, /* 白名单模式下仍须 TRUE */
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (have_attrs ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             env_block, safe_cwd[0] ? safe_cwd : NULL,
                             have_attrs ? &six.StartupInfo : &si, &sp.pi);
    if (attrs) { if (have_attrs) DeleteProcThreadAttributeList(attrs); free(attrs); }
    free(wcmdline);
    CloseHandle(hOutW);
    if (with_stdin) CloseHandle(hInR);
    if (!ok) {
        DWORD se = GetLastError();
        fprintf(stderr, "spawn failed: CreateProcessW err=%lu\n", (unsigned long)se);
        CloseHandle(hOutR);
        if (hInW) CloseHandle(hInW);
        return sp;
    }
    sp.ok = 1;
    sp.hOutR = hOutR;
    sp.hInW = hInW;
    return sp;
}

/* ============ 杀进程树 / 取消 / 断连处理 ============ */

/* 备用杀树：taskkill /T /F（Job Object 未能承载时用） */
static void kill_tree(DWORD pid) {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "taskkill /T /F /PID %lu", (unsigned long)pid);
    char *cl = _strdup(cmd);
    STARTUPINFOA si2 = { sizeof(si2) };
    PROCESS_INFORMATION pi2 = { 0 };
    if (CreateProcessA(NULL, cl, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si2, &pi2)) {
        WaitForSingleObject(pi2.hProcess, 5000);
        CloseHandle(pi2.hThread);
        CloseHandle(pi2.hProcess);
    }
    free(cl);
}

/* 终止整棵进程树（超时/取消/断连统一入口） */
static void terminate_tree(HANDLE job, PROCESS_INFORMATION *pi) {
    if (job) TerminateJobObject(job, 1);
    else {
        TerminateProcess(pi->hProcess, 1);
        kill_tree(pi->dwProcessId);
    }
}

/* ---- 根 Job：本进程退出时连带终止其全部后代 ----
   per-call Job 刻意不设 kill-on-close（调用正常结束时 start /b 起的后台进程必须存活），
   于是 exe 退出后这些后代无人认领、会一直跑下去（典型：占着串口的监控脚本）。
   这里把本进程放进一个 KILL_ON_JOB_CLOSE 的根 Job：子进程创建时自动继承成员身份，
   per-call Job 再赋给同一进程即形成嵌套（Windows 8+），两者互不干扰——
   exe 存活期间后台进程照常存活；exe 退出（正常退出，或被 TerminateProcess 硬杀）时
   句柄关闭，内核一并终止整棵后代树。
   句柄不可继承，且 run_cmdline 用了句柄白名单，子进程不会持有它（否则 kill-on-close 失效）。 */
static HANDLE g_root_job = NULL; /* 仅用于持有句柄（进程存活期间不得关闭） */
static void init_root_job(void) {
    HANDLE j = CreateJobObjectA(NULL, NULL);
    if (!j) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof(li));
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof(li)) ||
        !AssignProcessToJobObject(j, GetCurrentProcess())) {
        /* 本进程已属于某个不允许嵌套的 Job → 放弃，退回旧行为（后代可能在 exe 退出后残留） */
        fprintf(stderr, "root job unavailable (err %lu); descendants may outlive this process\n",
                (unsigned long)GetLastError());
        CloseHandle(j);
        return;
    }
    g_root_job = j;
}

/* ---- 在飞请求表（HTTP：notifications/cancelled → 杀对应进程树） ---- */
static CRITICAL_SECTION g_inflight_cs;
static int g_inflight_ready = 0;
typedef struct { char id[80]; HANDLE job; int cancelled; } Inflight;
static Inflight g_inflight[16];

static void inflight_begin(const char *id) {
    if (!g_inflight_ready) return;
    EnterCriticalSection(&g_inflight_cs);
    for (int i = 0; i < 16; i++) {
        if (!g_inflight[i].id[0]) {
            strncpy(g_inflight[i].id, id, sizeof(g_inflight[i].id) - 1);
            g_inflight[i].id[sizeof(g_inflight[i].id) - 1] = 0;
            g_inflight[i].job = NULL;
            g_inflight[i].cancelled = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_inflight_cs);
}
/* 进程创建后挂上 job；若期间已被取消则立即终止并返回 1（进程还没起来就取消也不再丢失） */
static int inflight_attach(const char *id, HANDLE job) {
    if (!g_inflight_ready) return 0;
    int cancelled = 0;
    EnterCriticalSection(&g_inflight_cs);
    for (int i = 0; i < 16; i++) {
        if (g_inflight[i].id[0] && strcmp(g_inflight[i].id, id) == 0) {
            if (g_inflight[i].cancelled) { cancelled = 1; TerminateJobObject(job, 1); }
            else g_inflight[i].job = job;
            break;
        }
    }
    LeaveCriticalSection(&g_inflight_cs);
    return cancelled;
}
/* 在锁内直接终止（避免取消线程持有将失效的句柄） */
static void inflight_terminate(const char *id) {
    if (!g_inflight_ready) return;
    EnterCriticalSection(&g_inflight_cs);
    for (int i = 0; i < 16; i++) {
        if (g_inflight[i].id[0] && strcmp(g_inflight[i].id, id) == 0) {
            g_inflight[i].cancelled = 1;
            if (g_inflight[i].job) TerminateJobObject(g_inflight[i].job, 1);
            break;
        }
    }
    LeaveCriticalSection(&g_inflight_cs);
}
/* 摘除条目并返回"期间是否被取消"（owner 在 CloseHandle 前调） */
static int inflight_finish(const char *id) {
    int cancelled = 0;
    if (!g_inflight_ready) return 0;
    EnterCriticalSection(&g_inflight_cs);
    for (int i = 0; i < 16; i++) {
        if (g_inflight[i].id[0] && strcmp(g_inflight[i].id, id) == 0) {
            cancelled = g_inflight[i].cancelled;
            g_inflight[i].id[0] = 0;
            g_inflight[i].job = NULL;
            break;
        }
    }
    LeaveCriticalSection(&g_inflight_cs);
    return cancelled;
}

/* ---- stdio：执行期间非阻塞轮询 stdin，捕获 notifications/cancelled ---- */
static char g_carry[8192];       /* 执行期间读到的半行，待后续补齐 */
static int g_carry_len = 0;
static char *g_pending[64];      /* 执行期间读到的完整行（保持顺序） */
static int g_pending_n = 0;

static int stdio_cancel_pending(const char *reqid) {
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    while (PeekNamedPipe(hin, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        char tmp[4096];
        DWORD rd = 0;
        DWORD want = avail < (DWORD)(sizeof(tmp) - 1) ? avail : (DWORD)(sizeof(tmp) - 1);
        if (!ReadFile(hin, tmp, want, &rd, NULL) || rd == 0) break;
        if (g_carry_len + (int)rd >= (int)sizeof(g_carry)) g_carry_len = 0; /* 极端长行：丢弃重来 */
        memcpy(g_carry + g_carry_len, tmp, rd);
        g_carry_len += rd;
        g_carry[g_carry_len] = 0;
        char *start = g_carry;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            int llen = (int)(nl - start);
            while (llen > 0 && start[llen - 1] == '\r') llen--;
            char *linebuf = (char *)malloc(llen + 1);
            memcpy(linebuf, start, llen);
            linebuf[llen] = 0;
            int cancel_this = 0;
            if (llen > 0 && strstr(linebuf, "cancelled") && (!reqid || reqid[0])) {
                Json *jm = json_parse(linebuf);
                if (jm) {
                    Json *mth = j_get(jm, "method");
                    if (mth && mth->type == 2 && strcmp(mth->str, "notifications/cancelled") == 0) {
                        Json *pr = j_get(jm, "params");
                        Json *rq = pr ? j_get(pr, "requestId") : NULL;
                        if (rq) {
                            char *rs = json_serialize(rq);
                            if (!reqid || !reqid[0] || strcmp(rs, reqid) == 0) cancel_this = 1;
                            free(rs);
                        }
                    }
                    j_free(jm);
                }
            }
            if (cancel_this) {
                free(linebuf);
                int rest = g_carry_len - (int)(nl + 1 - g_carry);
                memmove(g_carry, nl + 1, rest);
                g_carry_len = rest;
                g_carry[rest] = 0;
                return 1;
            }
            if (llen > 0 && g_pending_n < 64) g_pending[g_pending_n++] = linebuf;
            else free(linebuf);
            start = nl + 1;
        }
        int consumed = (int)(start - g_carry);
        int rest = g_carry_len - consumed;
        memmove(g_carry, start, rest);
        g_carry_len = rest;
        g_carry[rest] = 0;
    }
    return 0;
}

static RunResult run_cmdline(const char *cmd_line_in, int timeout_ms, const char *cwd,
                             wchar_t *env_block, const char *stdin_data, int stdin_len) {
    RunResult rr = { NULL, -1, 0, 0, 0 };
    int with_stdin = (stdin_data != NULL);
    SpawnResult sp = spawn_child(cmd_line_in, cwd, env_block, with_stdin);
    if (!sp.ok) { rr.spawn_err = 1; return rr; }
    PROCESS_INFORMATION pi = sp.pi;
    HANDLE hOutR = sp.hOutR;
    /* stdin 喂数据线程：写完自动关写端；子进程不读 stdin 或提前退出时写失败即退 */
    HANDLE hStdinThread = NULL;
    if (with_stdin) {
        StdinCtx *sc = (StdinCtx *)malloc(sizeof(StdinCtx));
        sc->h = sp.hInW;
        sc->data = stdin_data;
        sc->len = stdin_len;
        hStdinThread = CreateThread(NULL, 0, stdin_writer_thread, sc, 0, NULL);
        if (!hStdinThread) CloseHandle(sp.hInW);
    }

    /* Job Object：超时/取消/断连时用 TerminateJobObject 杀整棵进程树。
       故意不设 KILL_ON_JOB_CLOSE——正常结束时 start /b 起的后台子进程必须存活
       （"后台+轮询"模式依赖它）；未能加入 job 时退回 TerminateProcess+taskkill /T。 */
    /* 进程此时已是根 Job 成员（继承），此处赋给 per-call Job 形成嵌套（Windows 8+）；
       失败则退回 TerminateProcess + taskkill /T —— 注意该回退对 start /b 脱离的后台进程无效。 */
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        fprintf(stderr, "per-call job assign failed (err %lu); timeout/cancel falls back to taskkill\n",
                (unsigned long)GetLastError());
        CloseHandle(job); job = NULL;
    }
    if (job && g_http_mode && g_inflight_ready && g_cur_reqid[0]) inflight_attach(g_cur_reqid, job);

    /* 读输出（管道阻塞读，进程退出后读到 EOF） */
    char *out = (char *)malloc(1);
    out[0] = 0;
    int olen = 0, ocap = 1;
    int truncated = 0;
    int emitted = 0; /* 已经通过进度通知发出的字节数 */
    SpillSink sink = { INVALID_HANDLE_VALUE, "", 0, 0 };
    char buf[4096];
    DWORD rd;
    /* 主循环：等待进程 + 读管道（非阻塞方式） */
    DWORD start_ms = GetTickCount(); /* 每次调用独立计时（不能 static，否则第二次调用起全部误判超时） */
    int alive = 1;
    while (alive) {
        /* 抽干管道：原来每轮只读一次 4096B，再等 50ms 进程句柄，所有命令的输出吞吐被钉死在 ~80KB/s，大输出命令（编译）6.7MB 要 100s+）被拖慢数十倍。 */
        int got = 0;
        DWORD avail = 0;
        while (PeekNamedPipe(hOutR, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (!ReadFile(hOutR, buf, sizeof(buf), &rd, NULL) || rd == 0) break;
            collect_out(&sink, &out, &olen, &ocap, buf, (int)rd, &truncated);
            if (sink.h == INVALID_HANDLE_VALUE) { /* 溢出落盘后缓冲不再增长，无须再发进度 */
                progress_scan(out, olen, &emitted, 0);
                progress_scan_partial(out, olen, emitted);
            }
            got = 1;
        }
        /* 空闲探活：静默期没有 progress 可发，只能靠主动探测发现客户端已走 */
        if (!g_client_gone && client_probe()) g_client_gone = 1;
        /* 客户端断开（发送失败或探活失败）→ 终止整树 */
        if (g_client_gone) {
            terminate_tree(job, &pi);
            rr.aborted = 2;
            alive = 0;
        }
        /* stdio：轮询 stdin 里的 notifications/cancelled（HTTP 由独立线程查在飞表） */
        if (alive && !g_http_mode && stdio_cancel_pending(g_cur_reqid)) {
            terminate_tree(job, &pi);
            rr.aborted = 1;
            alive = 0;
        }
        if (!alive) break;
        DWORD wait = WaitForSingleObject(pi.hProcess, got ? 0 : 50);
        if (wait != WAIT_TIMEOUT) {
            /* 读完剩余数据 */
            while (PeekNamedPipe(hOutR, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                if (ReadFile(hOutR, buf, sizeof(buf), &rd, NULL) && rd > 0) {
                    collect_out(&sink, &out, &olen, &ocap, buf, (int)rd, &truncated);
                    if (sink.h == INVALID_HANDLE_VALUE) {
                        progress_scan(out, olen, &emitted, 0);
                        progress_scan_partial(out, olen, emitted);
                    }
                }
            }
            alive = 0;
        }
        /* 超时检查 */
        if (alive && timeout_ms > 0 && (GetTickCount() - start_ms) > (DWORD)timeout_ms) {
            terminate_tree(job, &pi);
            rr.timed_out = 1;
            alive = 0;
        }
    }
    /* 退出码必须在 CloseHandle 之前获取，否则永远拿不到真实值（被终止的情况无意义） */
    if (!rr.timed_out && !rr.aborted) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        rr.exit_code = (int)code;
    }
    if (job) {
        if (g_http_mode && g_inflight_ready && g_cur_reqid[0]) {
            if (inflight_finish(g_cur_reqid) && !rr.aborted && !rr.timed_out) rr.aborted = 1; /* 被取消 */
        }
        CloseHandle(job); /* 无 kill-on-close：正常结束时后台子进程存活 */
    }
    /* stdin 写线程：正常毫秒级完成；子进程不读 stdin 时最多等 5 秒后放弃（线程自行结束） */
    if (hStdinThread) {
        WaitForSingleObject(hStdinThread, 5000);
        CloseHandle(hStdinThread);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hOutR);

    /* 运行中已溢出落盘：完整输出在文件里，结果改为"路径 + 末尾"（内存里的前 16MB 不再需要） */
    if (sink.h != INVALID_HANDLE_VALUE) {
        CloseHandle(sink.h);
        spill_sweep_for(sink.path);
        free(out);
        rr.truncated = 1;
        rr.spill_path = _strdup(sink.path);
        rr.output = spill_tail_text(sink.path, "输出过大");
        if (!rr.output) {
            char *m = (char *)malloc(strlen(sink.path) + 160);
            sprintf(m, "[输出过大：完整日志已存盘：%s]\n", sink.path);
            rr.output = m;
        }
        return rr;
    }

    /* 截断时回退到 ASCII 边界，避免截断多字节字符导致整段被误判为 GBK 转码 */
    rr.truncated = truncated;
    if (truncated) {
        while (olen > 0 && (unsigned char)out[olen - 1] >= 0x80) {
            olen--;
            out[olen] = 0;
        }
        out_append(&out, &olen, &ocap, "\n[output truncated]\n", 20, &truncated);
    }

    /* 收尾：把剩余未发的输出 flush 成最后一条进度（被取消/断连时不必再发） */
    if (!rr.aborted) progress_scan(out, olen, &emitted, 1);

    /* 编码处理：整体合法 UTF-8 直接复用（常见路径，零拷贝）；否则逐行判定后转换 */
    if (olen <= 0 || utf8_valid(out, olen)) {
        rr.output = out;
    } else {
        char *u = to_utf8_lines(out, olen, 0);
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
static RunResult run_cmd(const char *cmd, int timeout_ms, const char *cwd,
                         wchar_t *env_block, const char *stdin_data, int stdin_len) {
    int s0 = 0;
    while (cmd[s0] == ' ' || cmd[s0] == '\t') s0++;
    char *line = (char *)malloc(strlen(cmd) + 16);
    if (cmd[s0] == '"') sprintf(line, "cmd /c \"%s\"", cmd);
    else sprintf(line, "cmd /c %s", cmd);
    RunResult rr = run_cmdline(line, timeout_ms, cwd, env_block, stdin_data, stdin_len);
    free(line);
    return rr;
}

/* git-bash 路径：直接 CreateProcess bash.exe（不经 cmd，避免 % 展开与引号启发式），
   脚本作为 -c 参数；-c 已带完整 MSYS PATH 且保持当前工作目录。 */
static RunResult run_bash_cmd(const char *script, int timeout_ms, const char *cwd,
                              wchar_t *env_block, const char *stdin_data, int stdin_len) {
    char *line = NULL;
    int len = 0, cap = 0;
    sbuf_append_quoted(&line, &len, &cap, g_bash_path);
    sbuf_append(&line, &len, &cap, " -c ");
    sbuf_append_quoted(&line, &len, &cap, script);
    RunResult rr = run_cmdline(line, timeout_ms, cwd, env_block, stdin_data, stdin_len);
    free(line);
    return rr;
}

/* ---- shell 参数归一：0=cmd 1=gitbash 2=powershell；非法返回 -1 ---- */
static int parse_shell(const char *shell) {
    if (!shell || !shell[0]) return 0;
    if (_stricmp(shell, "cmd") == 0) return 0;
    if (_stricmp(shell, "gitbash") == 0 || _stricmp(shell, "bash") == 0 || _stricmp(shell, "git-bash") == 0) return 1;
    if (_stricmp(shell, "powershell") == 0 || _stricmp(shell, "pwsh") == 0 || _stricmp(shell, "ps") == 0) return 2;
    return -1;
}

/* ---- PowerShell 探测：pwsh 7（WINEXEC_PWSH 覆盖 → ProgramFiles\PowerShell\7 → PATH）
   与系统自带的 Windows PowerShell 5.1（System32，几乎必然存在） ---- */
static char g_pwsh_path[520];
static char g_ps5_path[520];
static void detect_powershell(void) {
    g_pwsh_path[0] = 0;
    g_ps5_path[0] = 0;
    const char *over = getenv("WINEXEC_PWSH");
    if (over && over[0] && file_exists_a(over)) snprintf(g_pwsh_path, sizeof(g_pwsh_path), "%s", over);
    if (!g_pwsh_path[0]) {
        const char *pf = getenv("ProgramFiles");
        char cand[MAX_PATH];
        if (pf && pf[0]) {
            snprintf(cand, sizeof(cand), "%s\\PowerShell\\7\\pwsh.exe", pf);
            if (file_exists_a(cand)) snprintf(g_pwsh_path, sizeof(g_pwsh_path), "%s", cand);
        }
    }
    if (!g_pwsh_path[0]) {
        char found[MAX_PATH];
        if (SearchPathA(NULL, "pwsh.exe", NULL, MAX_PATH, found, NULL) && file_exists_a(found))
            snprintf(g_pwsh_path, sizeof(g_pwsh_path), "%s", found);
    }
    char sysdir[MAX_PATH];
    if (GetSystemDirectoryA(sysdir, MAX_PATH)) {
        snprintf(g_ps5_path, sizeof(g_ps5_path), "%s\\WindowsPowerShell\\v1.0\\powershell.exe", sysdir);
        if (!file_exists_a(g_ps5_path)) g_ps5_path[0] = 0;
    }
}

/* PowerShell 路径：pwsh 7 优先，回退 Windows PowerShell 5.1。脚本作为 -Command 的单个
   参数传入——不经过脚本文件读取，绕开 PS 5.1 按 ANSI 读 .ps1 的中文坑；输出编码交给
   按行判定（5.1 输出偏 GBK、7 默认 UTF-8，两种都能正确解码）。 */
static RunResult run_ps_cmd(const char *script, int timeout_ms, const char *cwd,
                            wchar_t *env_block, const char *stdin_data, int stdin_len) {
    const char *exe = g_pwsh_path[0] ? g_pwsh_path : g_ps5_path;
    char *line = NULL;
    int len = 0, cap = 0;
    sbuf_append_quoted(&line, &len, &cap, exe);
    sbuf_append(&line, &len, &cap, " -NoProfile -NonInteractive -Command ");
    sbuf_append_quoted(&line, &len, &cap, script);
    RunResult rr = run_cmdline(line, timeout_ms, cwd, env_block, stdin_data, stdin_len);
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

/* ============ 文件传输（windows_fs_put / windows_fs_get） ============
   把"物料"补进这条 Remote-SSH 通道：脚本/配置/固件/日志不再塞进 32K 上限的
   命令行字符串，而是作为文件直传。读写都用 offset 分块——刻意不用 append 标志：
   重试同一个块时 offset 写入是幂等的，append 会把重试块重复追加。 */

#define FS_CHUNK_MAX (4 * 1024 * 1024) /* 单次读写上限；大文件由调用方按 offset 续传 */

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 宽松解码：跳过空白（有的客户端按 76 列换行包裹），遇 '=' 结束；非法字符返回 NULL。 */
static char *b64_decode_alloc(const char *s, int slen, int *out_len) {
    char *out = (char *)malloc((size_t)(slen / 4 + 1) * 3 + 4);
    if (!out) return NULL;
    unsigned acc = 0;
    int nbits = 0, olen = 0;
    for (int i = 0; i < slen; i++) {
        int c = (unsigned char)s[i];
        if (c == '=') break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int v = b64_val(c);
        if (v < 0) { free(out); return NULL; }
        acc = ((acc << 6) | (unsigned)v) & 0xFFFFFFu;
        nbits += 6;
        if (nbits >= 8) { nbits -= 8; out[olen++] = (char)((acc >> nbits) & 0xFF); }
    }
    out[olen] = 0;
    *out_len = olen;
    return out;
}

static char *b64_encode_alloc(const char *data, int len, int *out_len) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int olen = (len + 2) / 3 * 4;
    char *out = (char *)malloc((size_t)olen + 1);
    if (!out) return NULL;
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        unsigned v = (unsigned char)data[i] << 16;
        if (i + 1 < len) v |= (unsigned)(unsigned char)data[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)data[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tab[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tab[v & 63] : '=';
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

/* UTF-8 → UTF-16（含结尾 NUL），调用方 free；非法 UTF-8 返回 NULL。 */
static wchar_t *utf8_to_wide_alloc(const char *s) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wlen);
    return w;
}

/* 写文件前尽力递归创建父目录；失败不在此报错，留给 CreateFileW 给出具体原因。
   支持 "D:\a\b\c.txt" 与 "D:/a/b.txt"；UNC 前缀段（\\server\share）的创建失败会被忽略。 */
static void fs_make_parent_dirs(wchar_t *wp) {
    for (wchar_t *p = wp; *p; p++) if (*p == L'/') *p = L'\\';
    wchar_t *end = wcsrchr(wp, L'\\');
    if (!end || end == wp) return; /* 无父目录 / 位于卷根 */
    *end = 0;
    if (wp[0] && wp[1] == L':' && wp[2] == 0) { *end = L'\\'; return; } /* "D:" 本身 */
    wchar_t *seg = wp;
    if (seg[0] && seg[1] == L':') seg += 2;
    while (*seg == L'\\') seg++;
    for (wchar_t *q = seg; *q; q++) {
        if (*q == L'\\') {
            wchar_t save = *q;
            *q = 0;
            CreateDirectoryW(wp, NULL);
            *q = save;
        }
    }
    CreateDirectoryW(wp, NULL);
    *end = L'\\';
}

static void fs_reply_error(Json *id, const char *msg) {
    Json *r = j_obj();
    j_set(r, "isError", j_bool(1));
    Json *c = j_arr();
    j_add(c, j_text(msg));
    j_set(r, "content", c);
    send_json(resp_result(id, r));
}

static void handle_fs_put(Json *id, Json *args) {
    const char *path = "";
    const char *content = NULL, *content_b64 = NULL;
    double offset_d = 0;
    if (args) {
        Json *p = j_get(args, "path");
        if (p && p->type == 2) path = p->str;
        Json *c1 = j_get(args, "content");
        if (c1 && c1->type == 2) content = c1->str;
        Json *c2 = j_get(args, "content_base64");
        if (c2 && c2->type == 2) content_b64 = c2->str;
        Json *o = j_get(args, "offset");
        if (o && o->type == 3) offset_d = o->num;
    }
    char err[600];
    if (!path[0]) { fs_reply_error(id, "参数 path 不能为空"); return; }
    if (!content && !content_b64) { fs_reply_error(id, "必须提供 content（文本）或 content_base64（二进制）之一"); return; }
    if (content && content_b64) { fs_reply_error(id, "content 与 content_base64 只能提供其一"); return; }
    if (offset_d < 0 || offset_d >= 9.0e15) { fs_reply_error(id, "offset 非法（需为 >= 0 的数字）"); return; }
    long long offset = (long long)offset_d;

    const char *data = content;
    int dlen = content ? (int)strlen(content) : 0;
    char *decoded = NULL;
    if (content_b64) {
        decoded = b64_decode_alloc(content_b64, (int)strlen(content_b64), &dlen);
        if (!decoded) { fs_reply_error(id, "content_base64 不是合法的 base64"); return; }
        data = decoded;
    }
    if (dlen > FS_CHUNK_MAX) {
        snprintf(err, sizeof(err), "单次写入最多 %d MB（本次 %d 字节）；大文件请用 offset 分块续传（首块 offset=0）",
                 FS_CHUNK_MAX / (1024 * 1024), dlen);
        fs_reply_error(id, err);
        free(decoded);
        return;
    }

    wchar_t *wp = utf8_to_wide_alloc(path);
    if (!wp) { fs_reply_error(id, "path 非法（不是合法 UTF-8）"); free(decoded); return; }
    fs_make_parent_dirs(wp);
    /* offset=0 → CREATE_ALWAYS（整文件覆盖/重建，重试首块幂等）；offset>0 → 必须已存在。 */
    HANDLE h = CreateFileW(wp, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           offset == 0 ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (offset > 0 && e == ERROR_FILE_NOT_FOUND)
            snprintf(err, sizeof(err), "文件不存在（offset>0 时需文件已存在；首块请用 offset=0）: %.400s", path);
        else
            snprintf(err, sizeof(err), "无法打开文件（Win32 错误 %lu）: %.400s", (unsigned long)e, path);
        fs_reply_error(id, err);
        free(wp); free(decoded);
        return;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    if (offset > sz.QuadPart) {
        snprintf(err, sizeof(err), "offset %lld 超出当前文件大小 %lld（不允许留空洞）", offset, (long long)sz.QuadPart);
        fs_reply_error(id, err);
        CloseHandle(h); free(wp); free(decoded);
        return;
    }
    LARGE_INTEGER pos;
    pos.QuadPart = offset;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    DWORD wr = 0;
    if (!WriteFile(h, data, (DWORD)dlen, &wr, NULL)) {
        snprintf(err, sizeof(err), "写入失败（Win32 错误 %lu）: %.400s", (unsigned long)GetLastError(), path);
        fs_reply_error(id, err);
        CloseHandle(h); free(wp); free(decoded);
        return;
    }
    long long newsize = (offset + (long long)wr > sz.QuadPart) ? offset + (long long)wr : sz.QuadPart;
    CloseHandle(h);

    char *text = (char *)malloc(strlen(path) + 160);
    sprintf(text, "[fs_put] %.400s\n写入 %lu 字节（offset %lld），文件当前大小 %lld 字节",
            path, (unsigned long)wr, offset, newsize);
    Json *res = j_obj();
    Json *c = j_arr();
    j_add(c, j_text(text));
    j_set(res, "content", c);
    Json *sc = j_obj();
    j_set(sc, "path", j_str(path));
    j_set(sc, "bytesWritten", j_num((double)wr));
    j_set(sc, "offset", j_num((double)offset));
    j_set(sc, "size", j_num((double)newsize));
    j_set(res, "structuredContent", sc);
    send_json(resp_result(id, res));
    free(text);
    free(wp);
    free(decoded);
}

static void handle_fs_get(Json *id, Json *args) {
    const char *path = "";
    const char *as = "base64";
    double offset_d = 0, length_d = 0;
    if (args) {
        Json *p = j_get(args, "path");
        if (p && p->type == 2) path = p->str;
        Json *a = j_get(args, "as");
        if (a && a->type == 2 && a->str && a->str[0]) as = a->str;
        Json *o = j_get(args, "offset");
        if (o && o->type == 3) offset_d = o->num;
        Json *l = j_get(args, "length");
        if (l && l->type == 3) length_d = l->num;
    }
    char err[600];
    int as_text;
    if (_stricmp(as, "base64") == 0) as_text = 0;
    else if (_stricmp(as, "text") == 0) as_text = 1;
    else { snprintf(err, sizeof(err), "as 仅支持 \"base64\" 或 \"text\"（收到: %.60s）", as); fs_reply_error(id, err); return; }
    if (!path[0]) { fs_reply_error(id, "参数 path 不能为空"); return; }
    if (offset_d < 0 || offset_d >= 9.0e15 || length_d < 0) { fs_reply_error(id, "offset/length 非法（需为 >= 0 的数字）"); return; }
    long long offset = (long long)offset_d;

    wchar_t *wp = utf8_to_wide_alloc(path);
    if (!wp) { fs_reply_error(id, "path 非法（不是合法 UTF-8）"); return; }
    DWORD attr = GetFileAttributesW(wp);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        snprintf(err, sizeof(err), "文件不存在或无法访问: %.400s", path);
        fs_reply_error(id, err); free(wp); return;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        snprintf(err, sizeof(err), "路径是目录（本工具只读文件）: %.400s", path);
        fs_reply_error(id, err); free(wp); return;
    }
    /* FILE_SHARE_WRITE：允许读取正在被写入的文件（如运行中的日志） */
    HANDLE h = CreateFileW(wp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, sizeof(err), "无法打开文件（Win32 错误 %lu）: %.400s", (unsigned long)GetLastError(), path);
        fs_reply_error(id, err); free(wp); return;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    if (offset > sz.QuadPart) {
        snprintf(err, sizeof(err), "offset %lld 超出文件大小 %lld", offset, (long long)sz.QuadPart);
        fs_reply_error(id, err); CloseHandle(h); free(wp); return;
    }
    long long want = sz.QuadPart - offset;
    if (length_d > 0 && (long long)length_d < want) want = (long long)length_d;
    if (want > FS_CHUNK_MAX) want = FS_CHUNK_MAX;
    char *buf = (char *)malloc((size_t)want + 1);
    if (!buf) { fs_reply_error(id, "内存不足"); CloseHandle(h); free(wp); return; }
    DWORD got = 0;
    if (want > 0) {
        LARGE_INTEGER pos;
        pos.QuadPart = offset;
        SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
        if (!ReadFile(h, buf, (DWORD)want, &got, NULL)) {
            snprintf(err, sizeof(err), "读取失败（Win32 错误 %lu）: %.400s", (unsigned long)GetLastError(), path);
            fs_reply_error(id, err); CloseHandle(h); free(wp); free(buf); return;
        }
    }
    CloseHandle(h);
    int eof = (offset + (long long)got >= sz.QuadPart);

    char *content = NULL;
    if (as_text) {
        /* 复用按行编码探测：文本文件里 GBK/UTF-8 混排也能正确解码 */
        content = (got > 0) ? to_utf8_lines(buf, (int)got, 0) : (char *)calloc(1, 1);
    } else {
        int clen = 0;
        content = b64_encode_alloc(buf, (int)got, &clen);
    }
    free(buf);
    if (!content) { fs_reply_error(id, "转码/编码失败（内存不足）"); free(wp); return; }

    Json *res = j_obj();
    Json *c = j_arr();
    j_add(c, j_text(content));
    /* 元数据用独立 content 块：不能用 structuredContent（VS Code 会丢掉数据块，见 windows_exec 处注释） */
    char meta[320];
    snprintf(meta, sizeof(meta), "[win-exec meta] path=%s size=%lld offset=%lld bytesRead=%lu eof=%s as=%s",
             path, sz.QuadPart, offset, (unsigned long)got, eof ? "true" : "false", as_text ? "text" : "base64");
    j_add(c, j_text(meta));
    j_set(res, "content", c);
    send_json(resp_result(id, res));
    free(content);
    free(wp);
}

/* ============ 环境快照（windows_env） ============
   一次调用把"这台机器有什么"说全：OS/主机/管理员、shell 路径（git-bash/pwsh/PS5）、
   常用工具链存在性（PATH 查找，不跑子进程）、串口列表（注册表直读）、磁盘余量、代理。
   目的：省掉 agent 开场"ver & where …"的多次探测往返。 */

static void reg_query_str(HKEY root, const char *sub, const char *name, char *out, int out_sz) {
    out[0] = 0;
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return;
    DWORD type = 0, sz = (DWORD)out_sz;
    if (RegQueryValueExA(k, name, NULL, &type, (LPBYTE)out, &sz) != ERROR_SUCCESS) out[0] = 0;
    RegCloseKey(k);
}

static int reg_query_dword(HKEY root, const char *sub, const char *name, DWORD *out) {
    HKEY k;
    if (RegOpenKeyExA(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, sz = sizeof(DWORD);
    int ok = RegQueryValueExA(k, name, NULL, &type, (LPBYTE)out, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

static int is_admin(void) {
    BOOL admin = FALSE;
    PSID grp = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &grp)) {
        CheckTokenMembership(NULL, grp, &admin);
        FreeSid(grp);
    }
    return admin ? 1 : 0;
}

static char *wide_to_utf8_alloc(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *u = (char *)malloc(n);
    if (!u) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL);
    return u;
}

static void handle_windows_env(Json *id) {
    char osname[128], disp[64], build[48], host[64], temp[512];
    reg_query_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", osname, sizeof(osname));
    reg_query_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion", disp, sizeof(disp));
    reg_query_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuildNumber", build, sizeof(build));
    DWORD hsz = sizeof(host);
    GetComputerNameA(host, &hsz);
    wchar_t wuser[64];
    DWORD usz = 64;
    char *user8 = GetUserNameW(wuser, &usz) ? wide_to_utf8_alloc(wuser) : NULL;
    SYSTEM_INFO sinfo;
    GetNativeSystemInfo(&sinfo);
    const char *arch = (sinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) ? "x64"
                     : (sinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) ? "arm64" : "x86";
    int admin = is_admin();

    Json *com_arr = j_arr();
    char coms[512];
    coms[0] = 0;
    {
        HKEY k;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            char vname[256], vdata[64];
            DWORD i = 0;
            for (;;) {
                DWORD vn = sizeof(vname), vd = sizeof(vdata), type = 0;
                if (RegEnumValueA(k, i++, vname, &vn, NULL, &type, (LPBYTE)vdata, &vd) != ERROR_SUCCESS) break;
                vdata[sizeof(vdata) - 1] = 0;
                if (coms[0]) strncat(coms, ", ", sizeof(coms) - strlen(coms) - 1);
                strncat(coms, vdata, sizeof(coms) - strlen(coms) - 1);
                j_add(com_arr, j_str(vdata));
            }
            RegCloseKey(k);
        }
    }

    Json *disk_arr = j_arr();
    char disks[1024];
    disks[0] = 0;
    {
        DWORD dmask = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (!(dmask & (1u << i)) || (disks[0] && strlen(disks) > sizeof(disks) - 96)) continue;
            char root[4] = "A:\\";
            root[0] = (char)('A' + i);
            if (GetDriveTypeA(root) != DRIVE_FIXED) continue;
            ULARGE_INTEGER freeb, totalb;
            if (!GetDiskFreeSpaceExA(root, &freeb, &totalb, NULL)) continue;
            double fg = (double)freeb.QuadPart / (1024.0 * 1024 * 1024);
            double tg = (double)totalb.QuadPart / (1024.0 * 1024 * 1024);
            char one[96];
            snprintf(one, sizeof(one), "%s %.1f/%.1fGB", root, fg, tg);
            if (disks[0]) strncat(disks, "; ", sizeof(disks) - strlen(disks) - 1);
            strncat(disks, one, sizeof(disks) - strlen(disks) - 1);
            Json *d = j_obj();
            j_set(d, "drive", j_str(root));
            j_set(d, "freeGb", j_num(fg));
            j_set(d, "totalGb", j_num(tg));
            j_add(disk_arr, d);
        }
    }

    char proxy[256];
    proxy[0] = 0;
    {
        DWORD en = 0;
        if (reg_query_dword(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", "ProxyEnable", &en) && en)
            reg_query_str(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", "ProxyServer", proxy, sizeof(proxy));
    }

    GetTempPathA(sizeof(temp), temp);
    wchar_t wcwd[512];
    char *cwd8 = GetCurrentDirectoryW(512, wcwd) ? wide_to_utf8_alloc(wcwd) : NULL;

    static const char *tool_names[] = { "python.exe", "python3.exe", "py.exe", "node.exe", "npm.cmd",
                                        "git.exe", "adb.exe", "esptool.exe", "ssh.exe", "curl.exe", "tar.exe", "gcc.exe" };
    char tools_line[2048];
    tools_line[0] = 0;
    Json *tools_obj = j_obj();
    for (int i = 0; i < (int)(sizeof(tool_names) / sizeof(tool_names[0])); i++) {
        char fp[MAX_PATH];
        if (!SearchPathA(NULL, tool_names[i], NULL, MAX_PATH, fp, NULL)) continue;
        snprintf(tools_line + strlen(tools_line), sizeof(tools_line) - strlen(tools_line),
                 "%s%s=%s", tools_line[0] ? "; " : "", tool_names[i], fp);
        j_set(tools_obj, tool_names[i], j_str(fp));
    }

    /* WSL 检测：agent 摸底后才知道跨文件系统代价（mnt 路径 I/O 慢两个数量级，重活该进 WSL 原生跑）。
       不解析 wsl.exe 输出（UTF-16LE，C 字符串遇 0x00 截断）——直接读注册表 Lxss，每个发行版一个子键 */
    char wsl_exe[256]; wsl_exe[0] = 0;
    char wsl_names[256]; wsl_names[0] = 0;
    {
        char fp[MAX_PATH];
        if (SearchPathA(NULL, "wsl.exe", NULL, MAX_PATH, fp, NULL)) {
            strcpy(wsl_exe, fp);
            HKEY k;
            if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &k) == ERROR_SUCCESS) {
                char sub[64];
                DWORD i = 0;
                for (;;) {
                    DWORD sn = sizeof(sub);
                    if (RegEnumKeyExA(k, i++, sub, &sn, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
                    char path[160];
                    snprintf(path, sizeof(path), "Software\\Microsoft\\Windows\\CurrentVersion\\Lxss\\%s", sub);
                    char dn[128];
                    reg_query_str(HKEY_CURRENT_USER, path, "DistributionName", dn, sizeof(dn));
                    if (dn[0] && strlen(wsl_names) < sizeof(wsl_names) - 3) {
                        if (wsl_names[0]) strncat(wsl_names, ", ", sizeof(wsl_names) - strlen(wsl_names) - 1);
                        strncat(wsl_names, dn, sizeof(wsl_names) - strlen(wsl_names) - 1);
                    }
                }
                RegCloseKey(k);
            }
        }
    }

    char text[8192];
    snprintf(text, sizeof(text),
        "[windows_env]\n"
        "OS: %s %s (build %s) %s\n"
        "host: %s | user: %s | admin: %s\n"
        "git-bash: %s\n"
        "pwsh(7): %s\n"
        "powershell(5.1): %s\n"
        "tools: %s\n"
        "COM ports: %s\n"
        "disks: %s\n"
        "WSL: %s\n"
        "proxy: %s\n"
        "TEMP: %s\n"
        "CWD: %s\n",
        osname[0] ? osname : "Windows", disp, build, arch,
        host, user8 ? user8 : "?", admin ? "yes" : "no",
        g_bash_path[0] ? g_bash_path : "(not found)",
        g_pwsh_path[0] ? g_pwsh_path : "(not found)",
        g_ps5_path[0] ? g_ps5_path : "(not found)",
        tools_line[0] ? tools_line : "(none)",
        coms[0] ? coms : "(none)",
        disks[0] ? disks : "(none)",
        wsl_exe[0] ? (wsl_names[0] ? wsl_names : "(installed, no distros)") : "(not installed)",
        proxy[0] ? proxy : "(none)",
        temp, cwd8 ? cwd8 : "?");

    Json *res = j_obj();
    Json *c = j_arr();
    j_add(c, j_text(text));
    j_set(res, "content", c);
    Json *sc = j_obj();
    j_set(sc, "os", j_str(osname));
    j_set(sc, "version", j_str(disp));
    j_set(sc, "build", j_str(build));
    j_set(sc, "arch", j_str(arch));
    j_set(sc, "host", j_str(host));
    j_set(sc, "user", j_str(user8 ? user8 : ""));
    j_set(sc, "admin", j_bool(admin));
    j_set(sc, "gitBash", j_str(g_bash_path));
    j_set(sc, "pwsh", j_str(g_pwsh_path));
    j_set(sc, "powershell51", j_str(g_ps5_path));
    j_set(sc, "tools", tools_obj);
    j_set(sc, "comPorts", com_arr);
    j_set(sc, "disks", disk_arr);
    j_set(sc, "wslExe", j_str(wsl_exe));
    j_set(sc, "wslDistros", j_str(wsl_names));
    j_set(sc, "proxy", j_str(proxy));
    j_set(sc, "temp", j_str(temp));
    j_set(sc, "cwd", j_str(cwd8 ? cwd8 : ""));
    j_set(res, "structuredContent", sc);
    send_json(resp_result(id, res));
    free(user8);
    free(cwd8);
}

/* ============ 后台作业（windows_bg_start / poll / kill / list） ============
   长驻进程从"一次调用的赌注"变为会话级对象：start 拿 job_id；poll 增量取输出
   （可长轮询等新数据/退出）；kill 杀整树。完整输出落盘 bg-*.log（纳入 spill 清扫），
   poll 按服务端游标从文件读增量。服务退出时 root Job 连带回收所有 bg 进程（0.4.4 机制）。 */

#define BG_MAX_JOBS 16
typedef struct {
    int used;
    int id_num;
    char idstr[16];        /* "bg<N>" */
    char command[200];     /* 展示用，截断 */
    const char *shell;
    DWORD pid;
    HANDLE job;            /* per-job Job（kill 用）；NULL 则退回 TerminateProcess */
    HANDLE hproc;
    HANDLE hread;
    HANDLE hlog;
    char logpath[2048];
    volatile LONG running;
    volatile LONG killed;
    DWORD exit_code;
    long long total_bytes; /* 线程更新 */
    long long sent_bytes;  /* poll 游标 */
    DWORD start_ms;
    volatile LONG thread_done; /* 线程最后一行置位；置位前槽位不可复用 */
} BgJob;
static BgJob g_bg[BG_MAX_JOBS];
static CRITICAL_SECTION g_bg_cs;
static int g_bg_ready = 0;
static volatile LONG g_bg_seq = 0;

typedef struct { int slot; } BgArg;

/* 抽干管道 → 落盘 + 计数（锁内做写与计数，与 poll/收尾互斥） */
static void bg_drain_pipe(BgJob *j) {
    if (!j->hread) return;
    char buf[8192];
    EnterCriticalSection(&g_bg_cs);
    DWORD avail = 0;
    while (PeekNamedPipe(j->hread, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        DWORD rd = 0, wr = 0;
        if (!ReadFile(j->hread, buf, sizeof(buf), &rd, NULL) || rd == 0) break;
        if (j->hlog) WriteFile(j->hlog, buf, rd, &wr, NULL);
        j->total_bytes += wr;
    }
    LeaveCriticalSection(&g_bg_cs);
}

/* 读线程：50ms 轮询抽干管道（不用阻塞 ReadFile——进程退出但 stdout 被后代持有管道时，
   阻塞读会永远等不到 EOF）；主进程退出后读干残余、收退出码并收尾。 */
static DWORD WINAPI bg_reader_thread(LPVOID arg) {
    int slot = ((BgArg *)arg)->slot;
    free(arg);
    BgJob *j = &g_bg[slot];
    for (;;) {
        bg_drain_pipe(j);
        if (WaitForSingleObject(j->hproc, 0) == WAIT_OBJECT_0) { bg_drain_pipe(j); break; }
        Sleep(50);
    }
    DWORD code = 0;
    GetExitCodeProcess(j->hproc, &code);
    EnterCriticalSection(&g_bg_cs);
    if (j->hlog) { CloseHandle(j->hlog); j->hlog = NULL; }
    if (j->hread) { CloseHandle(j->hread); j->hread = NULL; }
    if (j->hproc) { CloseHandle(j->hproc); j->hproc = NULL; }
    j->exit_code = code;
    InterlockedExchange(&j->running, 0);
    InterlockedExchange(&j->thread_done, 1); /* 最后一步：此后槽位可被复用 */
    LeaveCriticalSection(&g_bg_cs);
    return 0;
}

/* 找槽（锁内调用）：返回槽下标；id 匹配用 idstr */
static int bg_find_locked(const char *idstr) {
    for (int i = 0; i < BG_MAX_JOBS; i++)
        if (g_bg[i].used && strcmp(g_bg[i].idstr, idstr) == 0) return i;
    return -1;
}

static void handle_bg_start(Json *id, Json *args) {
    const char *command = "";
    const char *shell = NULL;
    const char *cwd = NULL;
    Json *env_obj = NULL;
    if (args) {
        Json *c = j_get(args, "command");
        if (c && c->type == 2) command = c->str;
        Json *sh = j_get(args, "shell");
        if (sh && sh->type == 2 && sh->str && sh->str[0]) shell = sh->str;
        Json *wd = j_get(args, "cwd");
        if (wd && wd->type == 2 && wd->str && wd->str[0]) cwd = wd->str;
        Json *eo = j_get(args, "env");
        if (eo) env_obj = eo;
    }
    if (!command[0]) { fs_reply_error(id, "参数 command 不能为空"); return; }
    if (env_obj && env_obj->type != 0) { fs_reply_error(id, "env 必须是 JSON 对象，如 {\"KEY\":\"VAL\"}"); return; }
    int sh2 = parse_shell(shell);
    if (sh2 < 0) { fs_reply_error(id, "shell 参数仅支持 \"cmd\"、\"gitbash\" 或 \"powershell\""); return; }
    if (cwd) {
        wchar_t wc[MAX_PATH];
        int wc_ok = MultiByteToWideChar(CP_UTF8, 0, cwd, -1, wc, MAX_PATH) > 0;
        DWORD da = wc_ok ? GetFileAttributesW(wc) : INVALID_FILE_ATTRIBUTES;
        if (!wc_ok || da == INVALID_FILE_ATTRIBUTES || !(da & FILE_ATTRIBUTE_DIRECTORY)) {
            fs_reply_error(id, "cwd 不存在或不是目录");
            return;
        }
    }
    if (sh2 == 1 && !g_bash_path[0]) detect_git_bash();
    if (sh2 == 1 && !g_bash_path[0]) { fs_reply_error(id, "未找到 git-bash（可设置 WINEXEC_GITBASH 指向 bash.exe）"); return; }
    if (sh2 == 2 && !g_pwsh_path[0] && !g_ps5_path[0]) detect_powershell();
    if (sh2 == 2 && !g_pwsh_path[0] && !g_ps5_path[0]) { fs_reply_error(id, "未找到 PowerShell（pwsh 7 与 5.1 均不可用）"); return; }
    int env_failed = 0;
    wchar_t *env_block = build_env_block(env_obj, &env_failed);
    if (env_failed) { fs_reply_error(id, "env 非法：值须为字符串（null 表示删除）；键非空且不含 '='"); return; }

    char *line = NULL;
    int len = 0, cap = 0;
    if (sh2 == 1) {
        sbuf_append_quoted(&line, &len, &cap, g_bash_path);
        sbuf_append(&line, &len, &cap, " -c ");
        sbuf_append_quoted(&line, &len, &cap, command);
    } else if (sh2 == 2) {
        sbuf_append_quoted(&line, &len, &cap, g_pwsh_path[0] ? g_pwsh_path : g_ps5_path);
        sbuf_append(&line, &len, &cap, " -NoProfile -NonInteractive -Command ");
        sbuf_append_quoted(&line, &len, &cap, command);
    } else {
        int s0 = 0;
        while (command[s0] == ' ' || command[s0] == '\t') s0++;
        line = (char *)malloc(strlen(command) + 16);
        if (command[s0] == '"') sprintf(line, "cmd /c \"%s\"", command);
        else sprintf(line, "cmd /c %s", command);
    }

        if (!g_bg_ready) { fs_reply_error(id, "内部错误：后台作业表未初始化"); free(line); if (env_block) free(env_block); return; }
    EnterCriticalSection(&g_bg_cs);
    int slot = -1, oldest = -1;
    DWORD oldest_ms = 0;
    for (int i = 0; i < BG_MAX_JOBS; i++) {
        if (!g_bg[i].used) { slot = i; break; }
        if (g_bg[i].thread_done && (oldest < 0 || g_bg[i].start_ms < oldest_ms)) { oldest = i; oldest_ms = g_bg[i].start_ms; }
    }
    if (slot < 0 && oldest >= 0) {
        slot = oldest;
        if (g_bg[slot].job) CloseHandle(g_bg[slot].job);
        memset(&g_bg[slot], 0, sizeof(BgJob));
    }
    if (slot >= 0) {
        g_bg[slot].used = 1; /* 预占，防止并发 start 抢同一槽 */
        g_bg[slot].id_num = (int)InterlockedIncrement(&g_bg_seq);
    }
    LeaveCriticalSection(&g_bg_cs);
    if (slot < 0) {
        fs_reply_error(id, "后台作业已满（16 个且都在运行）；请先 windows_bg_kill 或等其结束");
        free(line);
        if (env_block) free(env_block);
        return;
    }

    SpawnResult sp = spawn_child(line, cwd, env_block, 0);
    free(line);
    if (env_block) free(env_block);
    if (!sp.ok) {
        EnterCriticalSection(&g_bg_cs);
        g_bg[slot].used = 0;
        LeaveCriticalSection(&g_bg_cs);
        fs_reply_error(id, "进程创建失败（请检查 command）");
        return;
    }
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job && !AssignProcessToJobObject(job, sp.pi.hProcess)) {
        fprintf(stderr, "bg job assign failed (err %lu); kill falls back to TerminateProcess\n",
                (unsigned long)GetLastError());
        CloseHandle(job);
        job = NULL;
    }
    char logpath[2048];
    if (!spill_make_path_ex(logpath, sizeof(logpath), "bg")) logpath[0] = 0;
    HANDLE hlog = INVALID_HANDLE_VALUE;
    if (logpath[0])
        hlog = CreateFileA(logpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!logpath[0] || hlog == INVALID_HANDLE_VALUE) {
        /* 落盘不可用（TEMP 不可写）：终止进程并报错（不提供无输出的 bg） */
        if (job) TerminateJobObject(job, 1); else TerminateProcess(sp.pi.hProcess, 1);
        if (job) CloseHandle(job);
        CloseHandle(sp.pi.hThread);
        CloseHandle(sp.pi.hProcess);
        CloseHandle(sp.hOutR);
        EnterCriticalSection(&g_bg_cs);
        g_bg[slot].used = 0;
        LeaveCriticalSection(&g_bg_cs);
        fs_reply_error(id, "无法创建输出文件（%TEMP%\\win-exec-mcp 不可写）");
        return;
    }

    EnterCriticalSection(&g_bg_cs);
    BgJob *bj = &g_bg[slot];
    snprintf(bj->idstr, sizeof(bj->idstr), "bg%d", bj->id_num);
    strncpy(bj->command, command, sizeof(bj->command) - 1);
    bj->command[sizeof(bj->command) - 1] = 0;
    bj->shell = (sh2 == 1) ? "gitbash" : (sh2 == 2) ? (g_pwsh_path[0] ? "pwsh" : "powershell") : "cmd";
    bj->pid = sp.pi.dwProcessId;
    bj->job = job;
    bj->hproc = sp.pi.hProcess;
    bj->hread = sp.hOutR;
    bj->hlog = hlog;
    snprintf(bj->logpath, sizeof(bj->logpath), "%s", logpath);
    bj->running = 1;
    bj->killed = 0;
    bj->exit_code = 0;
    bj->total_bytes = 0;
    bj->sent_bytes = 0;
    bj->start_ms = GetTickCount();
    bj->thread_done = 0;
    CloseHandle(sp.pi.hThread);
    BgArg *barg = (BgArg *)malloc(sizeof(BgArg));
    barg->slot = slot;
    HANDLE th = CreateThread(NULL, 0, bg_reader_thread, barg, 0, NULL);
    if (th) CloseHandle(th);
    else { bj->running = 0; bj->thread_done = 1; } /* 极少见；进程仍归 job 管，可 kill */
    char idcopy[16];
    strcpy(idcopy, bj->idstr);
    LeaveCriticalSection(&g_bg_cs);

    spill_sweep_for(logpath); /* 顺手回收旧落盘文件（out-* 与 bg-* 同池） */
    char text[640];
    snprintf(text, sizeof(text),
             "[windows_bg_start] %s 已启动（pid %lu, shell=%s）\n输出完整落盘：%s\n用 windows_bg_poll(job_id=\"%s\") 取增量输出；windows_bg_kill 终止。",
             idcopy, (unsigned long)sp.pi.dwProcessId, bj->shell, logpath, idcopy);
    Json *res = j_obj();
    Json *c = j_arr();
    j_add(c, j_text(text));
    j_set(res, "content", c);
    Json *sc = j_obj();
    j_set(sc, "jobId", j_str(idcopy));
    j_set(sc, "pid", j_num((double)sp.pi.dwProcessId));
    j_set(sc, "shell", j_str(bj->shell));
    j_set(sc, "logPath", j_str(logpath));
    j_set(sc, "running", j_bool(1));
    j_set(res, "structuredContent", sc);
    send_json(resp_result(id, res));
}

static void handle_bg_poll(Json *id, Json *args) {
    const char *job_id = "";
    double wait_d = 0, maxb_d = 65536;
    if (args) {
        Json *j = j_get(args, "job_id");
        if (j && j->type == 2) job_id = j->str;
        Json *w = j_get(args, "wait_ms");
        if (w && w->type == 3) wait_d = w->num;
        Json *m = j_get(args, "max_bytes");
        if (m && m->type == 3) maxb_d = m->num;
    }
    if (!job_id[0]) { fs_reply_error(id, "参数 job_id 不能为空（形如 \"bg1\"，见 windows_bg_start 返回）"); return; }
    int wait_ms = (int)wait_d;
    if (wait_ms < 0) wait_ms = 0;
    if (wait_ms > 30000) wait_ms = 30000;
    long long maxb = (long long)maxb_d;
    if (maxb < 1024) maxb = 1024;
    if (maxb > 1048576) maxb = 1048576;

    DWORD t0 = GetTickCount();
    int slot = -1;
    for (;;) {
        slot = -1;
        EnterCriticalSection(&g_bg_cs);
        slot = bg_find_locked(job_id);
        int ready = 0;
        if (slot >= 0) {
            long long newb = g_bg[slot].total_bytes - g_bg[slot].sent_bytes;
            ready = (newb > 0) || (!g_bg[slot].running) || ((GetTickCount() - t0) >= (DWORD)wait_ms);
        }
        LeaveCriticalSection(&g_bg_cs);
        if (slot < 0) { fs_reply_error(id, "未知 job_id（可用 windows_bg_list 查看；形如 \"bg1\"）"); return; }
        if (ready) break;
        Sleep(50);
    }

    char *newdata = NULL;
    long long got = 0, total = 0, sent_after = 0;
    int running = 0, killed = 0;
    DWORD exit_code = 0, elapsed = 0;
    char logcopy[2048];
    logcopy[0] = 0;
    EnterCriticalSection(&g_bg_cs);
    BgJob *j = &g_bg[slot];
    long long want = j->total_bytes - j->sent_bytes;
    if (want > maxb) want = maxb;
    if (want > 0 && j->logpath[0]) {
        HANDLE hr = CreateFileA(j->logpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hr != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER pos;
            pos.QuadPart = j->sent_bytes;
            SetFilePointerEx(hr, pos, NULL, FILE_BEGIN);
            newdata = (char *)malloc((size_t)want + 1);
            DWORD rd = 0;
            if (newdata && ReadFile(hr, newdata, (DWORD)want, &rd, NULL)) got = rd;
            if (newdata) newdata[got] = 0;
            CloseHandle(hr);
        }
        /* 行边界修剪：本次已读满且还有剩余时，把半个尾部行留给下次（\n 不可能是多字节序列的一部分） */
        if (got > 0 && got == maxb && j->sent_bytes + got < j->total_bytes) {
            int cut = -1;
            for (int k = (int)got - 1; k >= 0 && k > (int)got - 8192; k--)
                if (newdata[k] == '\n') { cut = k + 1; break; }
            if (cut >= 0 && cut < (int)got) {
                j->sent_bytes -= (got - cut);
                got = cut;
                newdata[got] = 0;
            }
        }
        j->sent_bytes += got;
    }
    running = (int)j->running;
    killed = (int)j->killed;
    exit_code = j->exit_code;
    total = j->total_bytes;
    sent_after = j->sent_bytes;
    elapsed = GetTickCount() - j->start_ms;
    strncpy(logcopy, j->logpath, sizeof(logcopy) - 1);
    logcopy[sizeof(logcopy) - 1] = 0;
    LeaveCriticalSection(&g_bg_cs);

    char *text;
    if (newdata && got > 0) {
        if (utf8_valid(newdata, (int)got)) {
            text = newdata; /* 直接复用（常见路径） */
        } else {
            char *u = to_utf8_lines(newdata, (int)got, 0);
            text = u ? u : newdata;
            if (u) free(newdata);
        }
    } else {
        free(newdata);
        text = _strdup("");
    }
    Json *res = j_obj();
    Json *c = j_arr();
    j_add(c, j_text(text ? text : ""));
    /* 状态行用独立 content 块：不能用 structuredContent（VS Code 会丢掉输出文本，见 windows_exec 处注释） */
    char meta[512];
    snprintf(meta, sizeof(meta), "[bg] jobId=%s running=%s exitCode=%lu killed=%s newBytes=%lld totalBytes=%lld unreadBytes=%lld elapsedMs=%lu\n  log: %s",
             job_id, running ? "true" : "false", (unsigned long)exit_code, killed ? "true" : "false",
             got, total, total - sent_after, (unsigned long)elapsed, logcopy);
    j_add(c, j_text(meta));
    j_set(res, "content", c);
    send_json(resp_result(id, res));
    free(text);
}

static void handle_bg_kill(Json *id, Json *args) {
    const char *job_id = "";
    if (args) {
        Json *j = j_get(args, "job_id");
        if (j && j->type == 2) job_id = j->str;
    }
    if (!job_id[0]) { fs_reply_error(id, "参数 job_id 不能为空（形如 \"bg1\"）"); return; }
    EnterCriticalSection(&g_bg_cs);
    int slot = bg_find_locked(job_id);
    if (slot < 0) {
        LeaveCriticalSection(&g_bg_cs);
        fs_reply_error(id, "未知 job_id（可用 windows_bg_list 查看）");
        return;
    }
    BgJob *j = &g_bg[slot];
    int was_running = (int)j->running;
    if (j->job) TerminateJobObject(j->job, 9);
    else if (j->pid) {
        HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, j->pid);
        if (hp) { TerminateProcess(hp, 9); CloseHandle(hp); }
    }
    j->killed = 1;
    LeaveCriticalSection(&g_bg_cs);
    char text[200];
    snprintf(text, sizeof(text), "[windows_bg_kill] %s 终止信号已发出（was_running=%d）；稍后用 windows_bg_poll 可确认最终 exit_code。",
             job_id, was_running);
    Json *res = j_obj();
    Json *c = j_arr();
    j_add(c, j_text(text));
    j_set(res, "content", c);
    Json *sc = j_obj();
    j_set(sc, "jobId", j_str(job_id));
    j_set(sc, "killed", j_bool(1));
    j_set(sc, "wasRunning", j_bool(was_running));
    j_set(res, "structuredContent", sc);
    send_json(resp_result(id, res));
}

static void handle_bg_list(Json *id) {
    char text[4096];
    text[0] = 0;
    Json *arr = j_arr();
    EnterCriticalSection(&g_bg_cs);
    for (int i = 0; i < BG_MAX_JOBS; i++) {
        BgJob *j = &g_bg[i];
        if (!j->used) continue;
        char one[560];
        snprintf(one, sizeof(one), "%s: %s, pid=%lu, %s, elapsed=%llus, out=%lldB\n  log: %s\n  cmd: %.160s\n",
                 j->idstr, (int)j->running ? "running" : "exited",
                 (unsigned long)j->pid, j->shell,
                 (unsigned long long)((GetTickCount() - j->start_ms) / 1000),
                 j->total_bytes, j->logpath, j->command);
        if (strlen(text) + strlen(one) < sizeof(text) - 1) strcat(text, one);
        Json *jo = j_obj();
        j_set(jo, "jobId", j_str(j->idstr));
        j_set(jo, "pid", j_num((double)j->pid));
        j_set(jo, "shell", j_str(j->shell));
        j_set(jo, "running", j_bool((int)j->running));
        j_set(jo, "killed", j_bool((int)j->killed));
        j_set(jo, "exitCode", j_num((double)j->exit_code));
        j_set(jo, "elapsedMs", j_num((double)(GetTickCount() - j->start_ms)));
        j_set(jo, "totalBytes", j_num((double)j->total_bytes));
        j_set(jo, "logPath", j_str(j->logpath));
        j_set(jo, "command", j_str(j->command));
        j_add(arr, jo);
    }
    LeaveCriticalSection(&g_bg_cs);
    if (!text[0]) strcpy(text, "(没有后台作业)\n");
    Json *res = j_obj();
    Json *c = j_arr();
    j_add(c, j_text(text));
    j_set(res, "content", c);
    Json *sc = j_obj();
    j_set(sc, "jobs", arr);
    j_set(res, "structuredContent", sc);
    send_json(resp_result(id, res));
}

static void handle_tools_call(Json *id, Json *params) {
    Json *name = j_get(params, "name");
    Json *args = j_get(params, "arguments");
    const char *tool = (name && name->type == 2) ? name->str : "";

    progress_begin(params); /* 客户端带 progressToken 时启用自动流式输出 */

    /* 调用信号灯：扩展捕获 stderr 里这行驱动活动指示（不进 stdout 避免污染协议；仅 HTTP 模式发，同 [done]） */
    if (tool[0] && g_http_mode) {
        fprintf(stderr, "[call] %s\n", tool);
        fflush(stderr);
    }

    if (strcmp(tool, "windows_exec") == 0) {
        const char *command = "";
        int timeout_ms = 30000;
        const char *shell = NULL;
        const char *cwd = NULL;
        Json *env_obj = NULL;
        const char *stdin_data = NULL;
        int stdin_len = 0;
        int tail_lines = 0;
        if (args) {
            Json *c = j_get(args, "command");
            if (c && c->type == 2) command = c->str;
            Json *t = j_get(args, "timeout_ms");
            if (t && t->type == 3) timeout_ms = (int)t->num;
            else if (t && t->type == 2) timeout_ms = atoi(t->str);
            /* 钳制：<=0 一律回缺省（否则 0/负数会变成"永久等待"），上限 24h */
            if (timeout_ms <= 0) timeout_ms = 30000;
            if (timeout_ms > 24 * 3600 * 1000) timeout_ms = 24 * 3600 * 1000;
            Json *sh = j_get(args, "shell");
            if (sh && sh->type == 2 && sh->str && sh->str[0]) shell = sh->str;
            Json *wd = j_get(args, "cwd");
            if (wd && wd->type == 2 && wd->str && wd->str[0]) cwd = wd->str;
            Json *eo = j_get(args, "env");
            if (eo) env_obj = eo; /* 类型校验在下面（需发错误响应） */
            Json *sd = j_get(args, "stdin");
            if (sd && sd->type == 2 && sd->str) {
                stdin_data = sd->str;
                stdin_len = (int)strlen(sd->str);
            }
            Json *tl = j_get(args, "tail_lines");
            if (tl && tl->type == 3) tail_lines = (int)tl->num;
            else if (tl && tl->type == 2) tail_lines = atoi(tl->str);
            if (tail_lines < 0) tail_lines = 0;
        }
        /* cwd 必须是存在的目录（UTF-8 → W 校验，避免中文路径被 ANSI 误判） */
        if (cwd) {
            wchar_t wc[MAX_PATH];
            int wc_ok = MultiByteToWideChar(CP_UTF8, 0, cwd, -1, wc, MAX_PATH) > 0;
            DWORD da = wc_ok ? GetFileAttributesW(wc) : INVALID_FILE_ATTRIBUTES;
            if (!wc_ok || da == INVALID_FILE_ATTRIBUTES || !(da & FILE_ATTRIBUTE_DIRECTORY)) {
                Json *r = j_obj();
                j_set(r, "isError", j_bool(1));
                Json *c = j_arr();
                char emsg[640];
                snprintf(emsg, sizeof(emsg), "cwd 不存在或不是目录: %.600s", cwd);
                j_add(c, j_text(emsg));
                j_set(r, "content", c);
                send_json(resp_result(id, r));
                return;
            }
        }
        if (env_obj && env_obj->type != 0) {
            Json *r = j_obj();
            j_set(r, "isError", j_bool(1));
            Json *c = j_arr();
            j_add(c, j_text("env 必须是 JSON 对象，如 {\"HTTP_PROXY\":\"http://127.0.0.1:7897\"}"));
            j_set(r, "content", c);
            send_json(resp_result(id, r));
            return;
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
        int sh = parse_shell(shell);
        if (sh < 0) {
            Json *r = j_obj();
            j_set(r, "isError", j_bool(1));
            Json *c = j_arr();
            char emsg[200];
            snprintf(emsg, sizeof(emsg), "shell 参数仅支持 \"cmd\"、\"gitbash\" 或 \"powershell\"（收到: %.80s）", shell);
            j_add(c, j_text(emsg));
            j_set(r, "content", c);
            send_json(resp_result(id, r));
            return;
        }
        int use_bash = (sh == 1);
        int use_ps = (sh == 2);
        if (use_bash && !g_bash_path[0]) detect_git_bash(); /* 用户可能刚装好 Git：重探一次 */
        if (use_bash && !g_bash_path[0]) {
            Json *r = j_obj();
            j_set(r, "isError", j_bool(1));
            Json *c = j_arr();
            j_add(c, j_text("未找到 git-bash（已检查注册表 GitForWindows、常见安装目录与 PATH）；请改用 cmd 语法（省略 shell 或传 shell:\"cmd\"），或设置环境变量 WINEXEC_GITBASH 指向 bash.exe。"));
            j_set(r, "content", c);
            send_json(resp_result(id, r));
            return;
        }
        if (use_ps && !g_pwsh_path[0] && !g_ps5_path[0]) detect_powershell(); /* 可能刚装好 pwsh：重探一次 */
        if (use_ps && !g_pwsh_path[0] && !g_ps5_path[0]) {
            Json *r = j_obj();
            j_set(r, "isError", j_bool(1));
            Json *c = j_arr();
            j_add(c, j_text("未找到 PowerShell（pwsh 7 与系统 Windows PowerShell 5.1 均不可用）；请改用 cmd 语法。"));
            j_set(r, "content", c);
            send_json(resp_result(id, r));
            return;
        }
        int env_failed = 0;
        wchar_t *env_block = build_env_block(env_obj, &env_failed);
        if (env_failed) {
            Json *r = j_obj();
            j_set(r, "isError", j_bool(1));
            Json *c = j_arr();
            j_add(c, j_text("env 非法：需为 {\"KEY\":\"VAL\"} 对象（值须为字符串，null 表示删除；键非空且不含 '='）"));
            j_set(r, "content", c);
            send_json(resp_result(id, r));
            return;
        }
        /* 记录本请求 id（stdio 取消轮询 / HTTP 在飞表用） */
        g_cur_reqid[0] = 0;
        if (id) {
            char *rs = json_serialize(id);
            strncpy(g_cur_reqid, rs, sizeof(g_cur_reqid) - 1);
            g_cur_reqid[sizeof(g_cur_reqid) - 1] = 0;
            free(rs);
        }
        /* 在飞登记提前到进程创建之前：取消若在"进程还没起来"的窗口内到达也不会丢 */
        if (g_http_mode && g_inflight_ready && g_cur_reqid[0]) inflight_begin(g_cur_reqid);
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
        DWORD t0 = GetTickCount();
        RunResult rr = use_bash ? run_bash_cmd(command, timeout_ms, cwd, env_block, stdin_data, stdin_len)
                      : use_ps  ? run_ps_cmd(command, timeout_ms, cwd, env_block, stdin_data, stdin_len)
                      :           run_cmd(command, timeout_ms, cwd, env_block, stdin_data, stdin_len);
        if (env_block) free(env_block);
        char prefix[192];
        const char *shmark = use_bash ? " [shell: gitbash]"
                            : use_ps ? (g_pwsh_path[0] ? " [shell: pwsh]" : " [shell: powershell]")
                            : "";
        unsigned long dur = (unsigned long)(GetTickCount() - t0);
        if (rr.spawn_err) sprintf(prefix, "[spawn error]%s", shmark);
        else if (rr.aborted == 1) sprintf(prefix, "[exit: cancelled（命令已被调用方取消，进程树已终止）| %lu ms]%s", dur, shmark);
        else if (rr.aborted == 2) sprintf(prefix, "[exit: client-disconnected（客户端断开，进程树已终止）| %lu ms]%s", dur, shmark);
        else if (rr.timed_out) sprintf(prefix, "[exit: timeout(%dms 上限，实际 %lu ms) 命令超时被终止]%s", timeout_ms, dur, shmark);
        else sprintf(prefix, "[exit: %d | %lu ms]%s", rr.exit_code, dur, shmark);
        char *text = (char *)malloc(strlen(prefix) + (rr.output ? strlen(rr.output) : 0) + 16);
        sprintf(text, "%s\n%s", prefix, rr.output ? rr.output : "");
        /* 超大结果：全文落盘（保留全量，事后可查），回传体下面再收缩 */
        char *spill_repl = NULL;
        if (g_spill_bytes > 0 && (int)strlen(text) > g_spill_bytes) {
            spill_repl = spill_result(text, prefix, &rr.spill_path);
        }
        /* 调用方指定 tail_lines：只回传末尾 N 行（缺省 0 = 全给）。
           落的盘仍是全量；progress 通道不受影响（执行中已实时全量推送）。
           落盘提示行在 spill_repl 里（原文没有），摘出来交给 tail_last_lines 保住，
           否则截尾会把“日志在哪”这条线索一起截掉。 */
        if (tail_lines > 0) {
            char *hint = spill_repl ? spill_hint_line(spill_repl) : NULL;
            char *t = tail_last_lines(text, tail_lines, hint);
            if (hint) free(hint);
            if (t) { free(text); text = t; }
            if (spill_repl) { free(spill_repl); spill_repl = NULL; }
        } else if (spill_repl) {
            free(text);
            text = spill_repl;
            spill_repl = NULL;
        }
        /* 首行换成机器可读的 JSON 哨兵（旧文本前缀保留在同一行后面，兼容既有解析）。
           放最后做：此时 spill 与 tail_lines 都已定型（spilled/log 字段要准）。 */
        {
            const char *shell_id = use_bash ? "gitbash" : use_ps ? (g_pwsh_path[0] ? "pwsh" : "powershell") : "cmd";
            char *json_line = exit_json_line(&rr, dur, shell_id, prefix);
            if (json_line) {
                char *nt = replace_first_line(text, json_line);
                free(json_line);
                if (nt) { free(text); text = nt; }
            }
        }
        Json *r = j_obj();
        Json *c = j_arr();
        j_add(c, j_text(text));
        j_set(r, "content", c);
        /* 不带 structuredContent：VS Code 对带它的结果只把 JSON 喂给模型、丢弃文本（stdout 会全部丢失）。
           元数据已全部在文本前缀里（退出码/耗时/超时/取消/spillPath 标注） */
        send_json(resp_result(id, r));
        free(text);
        free(rr.output);
        free(rr.spill_path);
        g_ptok[0] = 0; g_ptok_num = 0; /* 清理本次进度上下文 */
        return;
    }

    if (strcmp(tool, "windows_fs_put") == 0) { handle_fs_put(id, args); return; }
    if (strcmp(tool, "windows_fs_get") == 0) { handle_fs_get(id, args); return; }
    if (strcmp(tool, "windows_bg_start") == 0) { handle_bg_start(id, args); return; }
    if (strcmp(tool, "windows_bg_poll") == 0) { handle_bg_poll(id, args); return; }
    if (strcmp(tool, "windows_bg_kill") == 0) { handle_bg_kill(id, args); return; }
    if (strcmp(tool, "windows_bg_list") == 0) { handle_bg_list(id); return; }
    if (strcmp(tool, "windows_env") == 0) { handle_windows_env(id); return; }

    Json *r = j_obj();
    j_set(r, "isError", j_bool(1));
    Json *c = j_arr();
    j_add(c, j_text("未知工具"));
    j_set(r, "content", c);
    send_json(resp_result(id, r));
}

/* MCP 标准 annotations：告知客户端工具的只读/破坏性，UI 可据此免确认或加警告 */
static Json *tool_ann(int ro, int dest) {
    Json *a = j_obj();
    j_set(a, "readOnlyHint", j_bool(ro));
    j_set(a, "destructiveHint", j_bool(dest));
    return a;
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
        j_set(info, "version", j_str(WINEXEC_VERSION));
        j_set(r, "serverInfo", info);
        char instr[2400];
        snprintf(instr, sizeof(instr),
            "windows_exec 在 Windows 主机上执行命令：默认 cmd 语法（shell 另有 gitbash / powershell；env 可叠加环境变量；stdin 可喂输入）。%s"
            "长命令的输出会以 progress 通知实时推送，最终结果包含完整输出与退出码。"
            "文件互传用 windows_fs_put / windows_fs_get（二进制走 base64；单次 ≤4MB，大文件按 offset 分块）。"
            "长驻进程用 windows_bg_start / windows_bg_poll / windows_bg_kill / windows_bg_list（起任务拿 job_id，poll 增量取输出）。"
            "环境摸底（OS/工具链/串口/磁盘/WSL）用 windows_env。"
            "经验法则：①预计超过 ~1 分钟的命令（安装/编译/长测试）改用 windows_bg_start + windows_bg_poll 循环（wait_ms 建议 10000-30000，直到 running=false）；"
            "②输出超过 512KB 时结果只含尾部与 spillPath，需要完整内容用 windows_fs_get 读该文件；"
            "③长命令给足 timeout_ms（默认仅 30 秒，超时会杀整棵进程树）；"
            "④结果首行是机器可读哨兵 [exit-json: {\"code\":0,\"ms\":625,\"shell\":\"pwsh\",\"timedOut\":false,\"cancelled\":false,\"clientGone\":false,\"spawnError\":false,\"spilled\":false,\"log\":null}]，退不出时 code 为 null；旧文本前缀跟在同一行后面（正文一字不改）。",
            g_bash_path[0]
                ? "本机已安装 git-bash：需要 Linux 风格命令或 .sh 脚本时请传 shell:\"gitbash\"（服务端自动定位 bash.exe，不会误用 WSL）。"
                : "本机未检测到 git-bash。");
        j_set(r, "instructions", j_str(instr));
        send_json(resp_result(id, r));
    } else if (strcmp(m, "notifications/initialized") == 0 || strcmp(m, "initialized") == 0) {
        /* 无响应 */
    } else if (strcmp(m, "notifications/cancelled") == 0) {
        /* 取消在飞请求：HTTP 模式下通过在飞表杀掉对应进程树（stdio 在执行循环里轮询处理） */
        Json *rq = params ? j_get(params, "requestId") : NULL;
        if (rq && g_http_mode) {
            char *rs = json_serialize(rq);
            inflight_terminate(rs);
            free(rs);
        }
        /* 无响应 */
    } else if (strcmp(m, "ping") == 0) {
        send_json(resp_result(id, j_obj()));
    } else if (strcmp(m, "tools/list") == 0) {
        Json *r = j_obj();
        Json *tools = j_arr();
        Json *t = j_obj();
        j_set(t, "name", j_str("windows_exec"));
        char desc[1800];
        snprintf(desc, sizeof(desc),
            "在 Windows 客户端执行一条命令（供 Remote-SSH/Linux 端的 agent 调用，跑在用户电脑的 Windows 上）。"
            "任何 Windows 命令/CLI/脚本皆可（如 dir、ipconfig、PowerShell、adb、esptool 等）；"
            "结果返回 stdout/stderr，首行是机器可读的 JSON 哨兵（[exit-json: {\"code\",\"ms\",\"shell\",\"timedOut\",\"cancelled\",\"clientGone\",\"spawnError\",\"spilled\",\"log\"}]，无法确定退出码时 code 为 null），旧文本前缀跟在同一行后面；超大输出自动落盘并只返回尾部与日志路径（标注 outputTruncated/spillPath）；"
            "参数: command(必填, Windows 命令字符串, 支持 && 和管道), timeout_ms(可选, 超时毫秒, 默认 30000, 上限 24h), "
            "cwd(可选, 工作目录, 如 D:\\proj\\app；不传则用服务所在目录), "
            "shell(可选, \"cmd\"(默认) / \"gitbash\" / \"powershell\"), "
            "env(可选, {\"KEY\":\"VAL\"} 叠加/覆盖子进程环境变量, 值为 null 表示删除), "
            "stdin(可选, 文本写入子进程 stdin 后关闭)。"
            "【其它工具】长驻/后台任务（服务器、监听、串口监控）用 windows_bg_start 起任务（不要用超长 timeout 硬等）；"
            "传文件用 windows_fs_put / windows_fs_get；环境摸底用 windows_env。%s%s",
            g_bash_path[0]
                ? "【环境】本机已安装 git-bash：需要 Linux 风格命令（grep/sed/awk/ls/单引号/$变量 等）或 .sh 脚本时，传 shell:\"gitbash\" 即可（服务端自动定位 bash.exe，无需自行拼路径与引号，也不会误用 WSL）。"
                : "【环境】本机未检测到 git-bash。",
            g_pwsh_path[0]
                ? "本机已安装 PowerShell 7：需要 PS 语法（注册表/对象管道/WMI 等）时传 shell:\"powershell\"，将优先使用 pwsh。"
                : (g_ps5_path[0] ? "需要 PS 语法时传 shell:\"powershell\"（将使用系统 Windows PowerShell 5.1）。" : ""));
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
        Json *p4 = j_obj();
        j_set(p4, "type", j_str("string"));
        j_set(p4, "description", j_str("工作目录（可选）；不传则用服务所在目录。Windows 绝对路径，如 D:\\proj\\app"));
        j_set(props, "cwd", p4);
        Json *p5 = j_obj();
        j_set(p5, "type", j_str("object"));
        j_set(p5, "description", j_str("叠加/覆盖到子进程的环境变量（可选），如 {\"PYTHONIOENCODING\":\"utf-8\",\"HTTP_PROXY\":\"http://127.0.0.1:7897\"}；值为 null 表示删除该变量"));
        j_set(props, "env", p5);
        Json *p6 = j_obj();
        j_set(p6, "type", j_str("string"));
        j_set(p6, "description", j_str("写入子进程 stdin 的文本（可选，写完关闭）；适合喂 python/sqlite/补丁类命令"));
        j_set(props, "stdin", p6);
        Json *p7 = j_obj();
        j_set(p7, "type", j_str("number"));
        j_set(p7, "description", j_str("只回传末尾 N 行（可选，缺省 0 = 全部）。完整输出仍会落盘（若超阈值），只是不塞进上下文；适合只要看最后几行的场景。流式显示始终全量，不受影响"));
        j_set(props, "tail_lines", p7);
        j_set(schema, "properties", props);
        Json *req = j_arr();
        j_add(req, j_str("command"));
        j_set(schema, "required", req);
        j_set(t, "inputSchema", schema);
        j_set(t, "annotations", tool_ann(0, 1));
        j_add(tools, t);

        /* ---- windows_fs_put ---- */
        Json *tp = j_obj();
        j_set(tp, "name", j_str("windows_fs_put"));
        j_set(tp, "description", j_str(
            "把内容写入 Windows 文件（Linux→Windows 传文件的推荐方式：不受 32K 命令行上限与 echo 转义限制；"
            "脚本、配置、固件、二进制均可）。参数: path(必填, Windows 绝对路径, 父目录自动创建); "
            "content 与 content_base64 二选一（content=文本按 UTF-8 写入; content_base64=base64 解码后写入，"
            "二进制或从 shell 拷贝的内容用它）；offset(可选, 写入起始字节偏移, 默认 0=创建或整文件覆盖; "
            ">0 需文件已存在，用于分块续传)。单次最多 4MB；大文件分块：首块 offset=0，之后 offset=已写入总字节数。"
            "返回文本确认：写入字节数（offset）、文件当前大小。"));
        Json *sp = j_obj();
        j_set(sp, "type", j_str("object"));
        Json *pp = j_obj();
        Json *f1 = j_obj(); j_set(f1, "type", j_str("string"));
        j_set(f1, "description", j_str("Windows 绝对路径，如 D:\\proj\\out\\fw.bin"));
        j_set(pp, "path", f1);
        Json *f2 = j_obj(); j_set(f2, "type", j_str("string"));
        j_set(f2, "description", j_str("文本内容（按 UTF-8 写入）；与 content_base64 二选一"));
        j_set(pp, "content", f2);
        Json *f3 = j_obj(); j_set(f3, "type", j_str("string"));
        j_set(f3, "description", j_str("base64 编码的内容（解码后写入）；与 content 二选一"));
        j_set(pp, "content_base64", f3);
        Json *f4 = j_obj(); j_set(f4, "type", j_str("number"));
        j_set(f4, "description", j_str("写入起始字节偏移，默认 0（创建/覆盖）；分块续传时=已写入总字节数"));
        j_set(pp, "offset", f4);
        j_set(sp, "properties", pp);
        Json *rp = j_arr(); j_add(rp, j_str("path"));
        j_set(sp, "required", rp);
        j_set(tp, "inputSchema", sp);
        j_set(tp, "annotations", tool_ann(0, 0));
        j_add(tools, tp);

        /* ---- windows_fs_get ---- */
        Json *tg = j_obj();
        j_set(tg, "name", j_str("windows_fs_get"));
        j_set(tg, "description", j_str(
            "读取 Windows 文件内容（二进制安全）。参数: path(必填); offset(可选, 默认 0); "
            "length(可选, 单次最多 4MB, 超出自动截断); as(可选, \"base64\"(默认, 无损) 或 \"text\")。"
            "content[0].text 为数据本身；content[1] 为元数据行 [win-exec meta]（size/offset/bytesRead/eof/as）；"
            "eof=false 时以 offset+bytesRead 作为下一次 offset 续读。二进制/大文件用 base64；"
            "text 仅适合文本文件（自动做编码探测，GBK/UTF-8 混排可正确解码）。"));
        Json *sg = j_obj();
        j_set(sg, "type", j_str("object"));
        Json *pg = j_obj();
        Json *g1 = j_obj(); j_set(g1, "type", j_str("string"));
        j_set(g1, "description", j_str("Windows 文件绝对路径"));
        j_set(pg, "path", g1);
        Json *g2 = j_obj(); j_set(g2, "type", j_str("number"));
        j_set(g2, "description", j_str("起始字节偏移，默认 0"));
        j_set(pg, "offset", g2);
        Json *g3 = j_obj(); j_set(g3, "type", j_str("number"));
        j_set(g3, "description", j_str("最多读取字节数（单次上限 4MB）"));
        j_set(pg, "length", g3);
        Json *g4 = j_obj(); j_set(g4, "type", j_str("string"));
        j_set(g4, "description", j_str("\"base64\"(默认) 或 \"text\"（编码探测转 UTF-8，仅文本文件）"));
        j_set(pg, "as", g4);
        j_set(sg, "properties", pg);
        Json *rg = j_arr(); j_add(rg, j_str("path"));
        j_set(sg, "required", rg);
        j_set(tg, "inputSchema", sg);
        j_set(tg, "annotations", tool_ann(1, 0));
        j_add(tools, tg);

        /* ---- windows_bg_start ---- */
        Json *tbs = j_obj();
        j_set(tbs, "name", j_str("windows_bg_start"));
        j_set(tbs, "description", j_str(
            "启动一个后台作业（长驻进程：服务器、监听、串口监控、长构建等）并立即返回 job_id。"
            "参数: command(必填, 同 windows_exec 的命令语法), shell(可选 \"cmd\"/\"gitbash\"/\"powershell\"), cwd(可选), env(可选 叠加环境变量)。"
            "完整输出落盘（logPath）；用 windows_bg_poll(job_id) 取增量输出。同时最多 16 个作业；"
            "服务退出时作业进程树会被一并回收（不会占端口/串口）。"));
        Json *sbs = j_obj();
        j_set(sbs, "type", j_str("object"));
        Json *pbs = j_obj();
        Json *b1 = j_obj(); j_set(b1, "type", j_str("string"));
        j_set(b1, "description", j_str("要后台运行的命令"));
        j_set(pbs, "command", b1);
        Json *b2 = j_obj(); j_set(b2, "type", j_str("string"));
        j_set(b2, "description", j_str("\"cmd\"(默认) / \"gitbash\" / \"powershell\""));
        j_set(pbs, "shell", b2);
        Json *b3 = j_obj(); j_set(b3, "type", j_str("string"));
        j_set(b3, "description", j_str("工作目录（可选）"));
        j_set(pbs, "cwd", b3);
        Json *b4 = j_obj(); j_set(b4, "type", j_str("object"));
        j_set(b4, "description", j_str("叠加环境变量（可选）"));
        j_set(pbs, "env", b4);
        j_set(sbs, "properties", pbs);
        Json *rbs = j_arr(); j_add(rbs, j_str("command"));
        j_set(sbs, "required", rbs);
        j_set(tbs, "inputSchema", sbs);
        j_set(tbs, "annotations", tool_ann(0, 0));
        j_add(tools, tbs);

        /* ---- windows_bg_poll ---- */
        Json *tbp = j_obj();
        j_set(tbp, "name", j_str("windows_bg_poll"));
        j_set(tbp, "description", j_str(
            "取后台作业的增量输出与状态。参数: job_id(必填, 形如 \"bg1\"), "
            "wait_ms(可选, 0-30000：等新输出或作业退出最多这么久，默认 0=立即返回；等长任务结束建议 10000-30000), "
            "max_bytes(可选, 单次最多返回字节, 默认 65536, 上限 1MB)。"
            "content[0].text = 自上次 poll 以来的新增输出；content[1] 为状态行 [bg]（jobId/running/exitCode/killed/newBytes/totalBytes/unreadBytes/elapsedMs/logPath）。"));
        Json *sbp = j_obj();
        j_set(sbp, "type", j_str("object"));
        Json *pbp = j_obj();
        Json *p1b = j_obj(); j_set(p1b, "type", j_str("string"));
        j_set(p1b, "description", j_str("后台作业 id（\"bg1\"…，来自 windows_bg_start / windows_bg_list）"));
        j_set(pbp, "job_id", p1b);
        Json *p2b = j_obj(); j_set(p2b, "type", j_str("number"));
        j_set(p2b, "description", j_str("最长等待毫秒（0-30000），默认 0 立即返回"));
        j_set(pbp, "wait_ms", p2b);
        Json *p3b = j_obj(); j_set(p3b, "type", j_str("number"));
        j_set(p3b, "description", j_str("单次最多返回字节数（默认 65536，上限 1048576）"));
        j_set(pbp, "max_bytes", p3b);
        j_set(sbp, "properties", pbp);
        Json *rbp = j_arr(); j_add(rbp, j_str("job_id"));
        j_set(sbp, "required", rbp);
        j_set(tbp, "inputSchema", sbp);
        j_set(tbp, "annotations", tool_ann(1, 0));
        j_add(tools, tbp);

        /* ---- windows_bg_kill ---- */
        Json *tbk = j_obj();
        j_set(tbk, "name", j_str("windows_bg_kill"));
        j_set(tbk, "description", j_str("终止后台作业（杀整棵进程树）。参数: job_id(必填)。之后用 windows_bg_poll 可确认最终 exit_code（killed=true）。"));
        Json *sbk = j_obj();
        j_set(sbk, "type", j_str("object"));
        Json *pbk = j_obj();
        Json *p1k = j_obj(); j_set(p1k, "type", j_str("string"));
        j_set(p1k, "description", j_str("后台作业 id"));
        j_set(pbk, "job_id", p1k);
        j_set(sbk, "properties", pbk);
        Json *rbk = j_arr(); j_add(rbk, j_str("job_id"));
        j_set(sbk, "required", rbk);
        j_set(tbk, "inputSchema", sbk);
        j_set(tbk, "annotations", tool_ann(0, 1));
        j_add(tools, tbk);

        /* ---- windows_bg_list ---- */
        Json *tbl = j_obj();
        j_set(tbl, "name", j_str("windows_bg_list"));
        j_set(tbl, "description", j_str("列出所有后台作业：id/pid/命令/shell/运行中或已退出/耗时/输出字节数/日志路径。无参数。"));
        Json *sbl = j_obj();
        j_set(sbl, "type", j_str("object"));
        j_set(sbl, "properties", j_obj());
        j_set(tbl, "inputSchema", sbl);
        j_set(tbl, "annotations", tool_ann(1, 0));
        j_add(tools, tbl);

        /* ---- windows_env ---- */
        Json *ten = j_obj();
        j_set(ten, "name", j_str("windows_env"));
        j_set(ten, "description", j_str(
            "Windows 环境快照（开工摸底用，一次拿全）：OS/版本/架构/主机/用户/是否管理员；"
            "git-bash、pwsh、Windows PowerShell 5.1 路径；常用工具链（python/node/git/adb/esptool/ssh/curl/tar/gcc）的 PATH 存在性；"
            "串口列表（COM#）；固定磁盘余量；WSL 发行版；系统代理。无参数。"));
        Json *sen = j_obj();
        j_set(sen, "type", j_str("object"));
        j_set(sen, "properties", j_obj());
        j_set(ten, "inputSchema", sen);
        j_set(ten, "annotations", tool_ann(1, 0));
        j_add(tools, ten);

        j_set(r, "tools", tools);
        send_json(resp_result(id, r));
    } else if (strcmp(m, "tools/call") == 0) {
        handle_tools_call(id, params);
        /* 调用结束信号（与开头 [call] 配对；扩展据此精确控制活动指示——长命令期间保持亮，不靠定时猜）。
           仅 HTTP 模式发：唯一听众是拉起本进程的扩展；stdio 模式下 stderr 会被客户端记进日志，别噪 */
        if (g_http_mode) {
            fprintf(stderr, "[done]\n");
            fflush(stderr);
        }
    } else {
        send_json(resp_error(id, -32601, "method not found"));
    }
    j_free(msg);
}

/* 读一行（去掉 \r），动态分配；EOF 且无内容返回 NULL。见 read_message 的帧解析说明。 */
static char *read_line_alloc(int *out_len) {
    int cap = 256, len = 0, c;
    char *b = (char *)malloc(cap);
    while ((c = getchar()) != EOF) {
        if (c == '\n') break;
        if (c != '\r') {
            if (len + 1 >= cap) { cap *= 2; b = (char *)realloc(b, cap); }
            b[len++] = (char)c;
        }
    }
    if (c == EOF && len == 0) { free(b); return NULL; }
    b[len] = 0;
    if (out_len) *out_len = len;
    return b;
}

/* 读取一条消息：支持 newline-delimited JSON 和 Content-Length 帧。
   帧解析要点：读完 Content-Length 行后必须一直读到"空行"为止再取 body，
   多读一个字符就会吃掉 body 首字节（JSON 解析失败且客户端表现为无响应）。 */
static char *read_message(void) {
    /* 0) 执行期间已缓存的完整行优先消费 */
    if (g_pending_n > 0) {
        char *r = g_pending[0];
        for (int i = 1; i < g_pending_n; i++) g_pending[i - 1] = g_pending[i];
        g_pending_n--;
        return r;
    }
    /* 1) 执行期间读到的"半行"：拼上后续字节补齐本行 */
    if (g_carry_len > 0) {
        int cap2 = g_carry_len + 128;
        char *r = (char *)malloc(cap2);
        int n2 = g_carry_len;
        memcpy(r, g_carry, n2);
        g_carry_len = 0;
        int c;
        while ((c = getchar()) != EOF) {
            if (c == '\n') break;
            if (c == '\r') continue;
            if (n2 + 1 >= cap2) { cap2 *= 2; r = (char *)realloc(r, cap2); }
            r[n2++] = (char)c;
        }
        r[n2] = 0;
        return r;
    }
    int first_len = 0;
    char *first = read_line_alloc(&first_len);
    if (!first) return NULL;
    if (_strnicmp(first, "Content-Length:", 15) == 0) {
        int body_len = atoi(first + 15);
        free(first);
        char *hline;
        while ((hline = read_line_alloc(NULL)) != NULL && hline[0]) free(hline); /* 读到空行为止 */
        free(hline);
        if (body_len < 0) body_len = 0;
        char *line = (char *)malloc(body_len + 1);
        for (int i = 0; i < body_len; i++) {
            int c4 = getchar();
            if (c4 == EOF) { free(line); return NULL; }
            line[i] = (char)c4;
        }
        line[body_len] = 0;
        return line;
    }
    return first; /* newline-delimited：整行已读完 */
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

static int http_send(SOCKET s, int status, const char *status_text,
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
    if (!send_all(s, head, n)) return 0;
    if (body && body_len > 0 && !send_all(s, body, body_len)) return 0;
    return 1;
}

static const char *g_token = NULL;

static DWORD WINAPI http_client_serve(LPVOID arg) {
    SOCKET s = (SOCKET)(intptr_t)arg;
    g_conn_sock = s; /* 供 client_probe 探活（每连接一线程，run_cmd 与探活同线程） */
    /* 读超时：半开/卡住的客户端不会永久占住一个线程 */
    DWORD rcv_to = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv_to, sizeof(rcv_to));
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
            /* 401 带人话提示：agent（尤其服务器端 Claude Code）拿到 401 时能自己走修复路径，而不是反复重试 */
            const char *e = "{\"error\":\"unauthorized\",\"hint\":\"token mismatch: the project .mcp.json is stale or the Authorization header is wrong. Fix: reopen the project window on Windows so the extension resyncs the file (the SSH tunnel only exists while VS Code is connected), or run command 'WinExec MCP: Regenerate HTTP token' and re-sync; check that the header is exactly 'Bearer <token>'.\"}";
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
            if (want_progress) send_all(s, sse, sn); /* 响应头已随流先行发出，这里只追加事件 */
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

/* 每连接一线程；计数限流，避免异常客户端堆线程（正常调用远达不到） */
static LONG g_conn_count = 0;
#define MAX_HTTP_CONN 64
static DWORD WINAPI http_client_thread(LPVOID arg) {
    DWORD rc = http_client_serve(arg);
    InterlockedDecrement(&g_conn_count);
    return rc;
}

static DWORD g_parent_pid = 0;

/* 父进程看护：--parent-pid 指定的进程（扩展 host）退出后本服务自动退出，
   避免 VS Code 关闭/卸载/崩溃后留下占用端口的孤儿进程 */
/* 监听 socket 提为全局：任何退出路径都要先关它，端口才能立刻释放 */
static SOCKET g_listen_socket = INVALID_SOCKET;
static volatile LONG g_stop_requested = 0;

/* 请求停止：先关监听 socket（立即释放端口 + 唤醒阻塞中的 accept），主循环随后正常退出。
   不用 ExitProcess 硬退，避免跳过清理、也让端口尽早可用。 */
static void http_request_stop(void) {
    InterlockedExchange(&g_stop_requested, 1);
    SOCKET s = g_listen_socket;
    if (s != INVALID_SOCKET) {
        g_listen_socket = INVALID_SOCKET;
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}

static DWORD WINAPI parent_watchdog(LPVOID arg) {
    (void)arg;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, g_parent_pid);
    if (!h) { http_request_stop(); return 0; } /* 父进程已不存在，无需看护 */
    for (;;) {
        DWORD w = WaitForSingleObject(h, 2000);
        if (w == WAIT_OBJECT_0) { CloseHandle(h); http_request_stop(); return 0; } /* 父进程已退出 */
        if (w == WAIT_FAILED) { CloseHandle(h); http_request_stop(); return 0; }
    }
    return 0;
}

static int http_main(int port, const char *token, DWORD parent_pid, const char *bind_addr) {
    g_http_mode = 1;
    /* token 也可以从环境变量拿：避免它出现在进程命令行（任务管理器/进程列表可见） */
    if (!token || !token[0]) token = getenv("WINEXEC_TOKEN");
    g_token = token;
    if (parent_pid) {
        g_parent_pid = parent_pid;
        CreateThread(NULL, 0, parent_watchdog, NULL, 0, NULL);
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) { fprintf(stderr, "socket failed\n"); return 1; }
    /* socket 默认【可继承】：若不禁止，run_cmdline() 用 bInheritHandles=TRUE 创建子进程时
       会把监听 socket 一并交给子进程 -> 本进程退出后 socket 不销毁，端口被孤儿子进程占住；
       在 SO_EXCLUSIVEADDRUSE 下这意味着该端口永久无法再 bind。 */
    SetHandleInformation((HANDLE)ls, HANDLE_FLAG_INHERIT, 0);
    int one = 1;
    /* Windows 独占绑定：同端口第二个实例 bind 直接失败退出，避免多实例共享端口收不到连接 */
#ifdef SO_EXCLUSIVEADDRUSE
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
#else
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    /* 默认只绑回环（SSH 隧道场景足够且更安全）；--bind <addr> 可显式指定（如 0.0.0.0 / 局域网 IP） */
    addr.sin_addr.s_addr = (bind_addr && *bind_addr) ? inet_addr(bind_addr) : htonl(0x7f000001); /* 127.0.0.1 */
    addr.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int err = WSAGetLastError();
        const char *host = (bind_addr && *bind_addr) ? bind_addr : "127.0.0.1";
        if (err == WSAEADDRINUSE) {
            SOCKET pr = socket(AF_INET, SOCK_STREAM, 0);
            int still_alive = (pr != INVALID_SOCKET &&
                               connect(pr, (struct sockaddr *)&addr, sizeof(addr)) == 0);
            if (pr != INVALID_SOCKET) closesocket(pr);
            fprintf(stderr,
                    "bind %s:%d failed: WSAEADDRINUSE (%s)\n"
                    "  %s\n", host, port,
                    still_alive ? "port still accepting connections" : "port not responding (stale)",
                    still_alive ? "  -> another instance is running; if not, end leftover win-exec-mcp.exe and retry"
                                : "  -> likely an orphan socket (listen handle inherited by a child process); end leftovers or use another port");
        } else {
            fprintf(stderr, "bind %s:%d failed (WSA error %d)\n", host, port, err);
        }
        closesocket(ls);
        return 1;
    }
    if (listen(ls, 16) != 0) { fprintf(stderr, "listen failed\n"); closesocket(ls); return 1; }
    g_listen_socket = ls; /* 交给退出路径清理 */
    fprintf(stderr, "win-exec-mcp HTTP listening on %s:%d (token: %s)\n", (bind_addr && *bind_addr) ? bind_addr : "127.0.0.1", port, g_token ? "set" : "none");
    for (;;) {
        SOCKET cl = accept(ls, NULL, NULL);
        if (cl == INVALID_SOCKET) {
            if (InterlockedCompareExchange(&g_stop_requested, 0, 0)) break; /* 收到停止请求 */
            continue;
        }
        if (InterlockedIncrement(&g_conn_count) > MAX_HTTP_CONN) {
            InterlockedDecrement(&g_conn_count);
            fprintf(stderr, "too many concurrent connections (>%d); dropping one\n", MAX_HTTP_CONN);
            closesocket(cl);
            continue;
        }
        HANDLE th = CreateThread(NULL, 0, http_client_thread, (LPVOID)(intptr_t)cl, 0, NULL);
        if (th) CloseHandle(th); /* 线程句柄不用 join，不关就是句柄泄漏 */
        else { InterlockedDecrement(&g_conn_count); closesocket(cl); }
    }
    WSACleanup();
    return 0;
}

int main(int argc, char **argv) {
    load_env_config();
    init_root_job();
    InitializeCriticalSection(&g_inflight_cs);
    g_inflight_ready = 1;
    InitializeCriticalSection(&g_bg_cs);
    g_bg_ready = 1;
    detect_git_bash();
    detect_powershell();
    if (argc >= 3 && strcmp(argv[1], "--http") == 0) {
        const char *token = NULL;
        const char *bind_addr = NULL;
        DWORD parent_pid = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) token = argv[i + 1];
            else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) bind_addr = argv[i + 1];
            else if (strcmp(argv[i], "--parent-pid") == 0 && i + 1 < argc) parent_pid = (DWORD)atol(argv[i + 1]);
        }
        return http_main(atoi(argv[2]), token, parent_pid, bind_addr);
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    for (;;) {
        char *msg = read_message();
        if (!msg) break;
        if (msg[0]) handle_message(msg);
        free(msg);
    }
    return 0;
}
