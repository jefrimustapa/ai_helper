/*
 * ai-helper.c - Tray/GUI app that listens on a localhost TCP port and launches
 * allowlisted applications on request.
 *
 * Protocol: one line per connection:   <app-name> [parameters]\n
 *   <app-name> is the file name (with or without ".exe") of an allowlisted app.
 *   Reply: "OK\r\n" or "ERR <reason>\r\n".
 *
 * HTTP bridge (COH-435): if the first line starts with "GET ", the connection is
 * served as minimal HTTP:  GET /open?path=..&select=1&token=..  (Explorer only).
 * Off by default; see [Bridge] in settings.ini and README.md.
 *
 * Build: see build.bat
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <stdio.h>
#include <wchar.h>

#define APP_NAME      L"AiHelper"
#define MAX_APPS      64
#define DEFAULT_PORT  5055
#define MAX_LINE      4096
#define MAX_REQ       8192
#define MAX_ROOTS     16
#define TOKEN_LEN     32

#define WM_SERVER_STATE (WM_APP + 1)
#define WM_TRAY         (WM_APP + 2)

enum {
    ID_AUTOSTART = 100, ID_PORT, ID_LIST, ID_ADD, ID_REMOVE, ID_SAVE, ID_STATUS, ID_BRIDGE,
    ID_TAB, ID_ROOTLIST, ID_ROOTADD, ID_ROOTREMOVE,
    ID_TRAY_SHOW = 200, ID_TRAY_EXIT
};

static HWND  g_hwnd, g_chkAuto, g_chkBridge, g_edPort, g_list, g_status, g_tab, g_rootList;
static wchar_t g_ini[MAX_PATH];
static wchar_t g_exe[MAX_PATH];

static CRITICAL_SECTION g_cs;           /* guards g_apps/g_appCount */
static wchar_t g_apps[MAX_APPS][MAX_PATH];
static int     g_appCount;
static int     g_port = DEFAULT_PORT;

/* HTTP bridge config (guarded by g_cs) */
static int     g_bridgeOn;                       /* [Bridge] Enabled */
static char    g_token[TOKEN_LEN + 1];           /* [Bridge] Token (hex) */
static wchar_t g_roots[MAX_ROOTS][MAX_PATH];     /* [Bridge] Root0..RootN */
static int     g_rootCount;

/* test mode: /port:N /ini:path  -> separate instance, no registry writes */
static int     g_testMode;
static int     g_portOverride;

static HANDLE        g_thread;
static volatile LONG g_stop;

/* ---------- config ---------- */

static void config_init_path(void)
{
    wchar_t dir[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir);
    wcscat(dir, L"\\" APP_NAME);
    CreateDirectoryW(dir, NULL);
    swprintf(g_ini, MAX_PATH, L"%ls\\settings.ini", dir);
}

/* The port lives in the user environment variable AIHELPER_PORT. */
#define ENV_KEY  L"Environment"
#define PORT_VAR L"AIHELPER_PORT"

static int port_load(void)
{
    HKEY k;
    wchar_t buf[16] = L"";
    DWORD size = sizeof buf - sizeof(wchar_t);
    int port = DEFAULT_PORT;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ENV_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, PORT_VAR, NULL, NULL, (BYTE *)buf, &size) == ERROR_SUCCESS) {
            int v = _wtoi(buf);
            if (v >= 1 && v <= 65535) port = v;
        }
        RegCloseKey(k);
    }
    return port;
}

static void port_save(int port)
{
    HKEY k;
    if (g_testMode) return;
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", port);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ENV_KEY, 0, KEY_WRITE, &k) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, PORT_VAR, 0, REG_SZ, (const BYTE *)buf,
                   (DWORD)((wcslen(buf) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    /* tell running programs (Explorer, new shells) the environment changed */
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 2000, NULL);
}

/* ---------- HTTP bridge config ---------- */

typedef BOOLEAN (WINAPI *RTLGENRANDOM)(PVOID, ULONG);

static int token_generate(char *out)
{
    unsigned char rnd[TOKEN_LEN / 2];
    if (getenv("AIHELPER_TEST_NORNG")) return 0;     /* test hook: simulate RNG failure */
    RTLGENRANDOM f = (RTLGENRANDOM)GetProcAddress(LoadLibraryW(L"advapi32.dll"), "SystemFunction036");
    if (!f || !f(rnd, sizeof rnd)) return 0;
    for (int i = 0; i < (int)sizeof rnd; i++) sprintf(out + 2 * i, "%02x", rnd[i]);
    out[TOKEN_LEN] = 0;
    return 1;
}

static int token_valid(const char *t)
{
    if (strlen(t) != TOKEN_LEN) return 0;
    for (int i = 0; i < TOKEN_LEN; i++)
        if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f'))) return 0;
    return 1;
}

/* Loads [Bridge]. First run (no valid Token): writes Enabled=0 (if unset), a random
 * permanent Token and the default Root0 (if unset). To regenerate the token:
 * delete the Token line from settings.ini and restart ai-helper. */
static void bridge_load(void)
{
    wchar_t wt[64], key[32], r0[MAX_PATH];
    char tok[TOKEN_LEN + 1] = "";
    GetPrivateProfileStringW(L"Bridge", L"Token", L"", wt, 64, g_ini);
    if (WideCharToMultiByte(CP_UTF8, 0, wt, -1, tok, sizeof tok, NULL, NULL) <= 0) tok[0] = 0;

    if (!token_valid(tok)) {
        if (token_generate(tok)) {
            MultiByteToWideChar(CP_UTF8, 0, tok, -1, wt, 64);
            WritePrivateProfileStringW(L"Bridge", L"Token", wt, g_ini);
            GetPrivateProfileStringW(L"Bridge", L"Enabled", L"", key, 32, g_ini);
            if (!key[0])
                WritePrivateProfileStringW(L"Bridge", L"Enabled", L"0", g_ini);
            GetPrivateProfileStringW(L"Bridge", L"Root0", L"", r0, MAX_PATH, g_ini);
            if (!r0[0])
                WritePrivateProfileStringW(L"Bridge", L"Root0", L"D:\\ai_project\\paperclip_setup", g_ini);
        } else {
            tok[0] = 0;                 /* no token -> bridge stays closed */
        }
    }

    EnterCriticalSection(&g_cs);
    g_bridgeOn = GetPrivateProfileIntW(L"Bridge", L"Enabled", 0, g_ini) == 1 && tok[0];
    strcpy(g_token, tok);
    g_rootCount = 0;
    for (int i = 0; i < MAX_ROOTS; i++) {
        swprintf(key, 32, L"Root%d", i);
        GetPrivateProfileStringW(L"Bridge", key, L"", g_roots[g_rootCount], MAX_PATH, g_ini);
        if (g_roots[g_rootCount][0]) g_rootCount++;
    }
    LeaveCriticalSection(&g_cs);
}

static void config_load(void)
{
    wchar_t key[32];
    g_port = g_portOverride ? g_portOverride : port_load();

    EnterCriticalSection(&g_cs);
    g_appCount = 0;
    if (GetPrivateProfileIntW(L"Settings", L"Initialized", 0, g_ini)) {
        for (int i = 0; i < MAX_APPS; i++) {
            swprintf(key, 32, L"App%d", i);
            GetPrivateProfileStringW(L"Apps", key, L"", g_apps[g_appCount], MAX_PATH, g_ini);
            if (g_apps[g_appCount][0]) g_appCount++;
        }
    } else {
        wchar_t win[MAX_PATH];
        GetWindowsDirectoryW(win, MAX_PATH);
        swprintf(g_apps[0], MAX_PATH, L"%ls\\explorer.exe", win);
        g_appCount = 1;
    }
    LeaveCriticalSection(&g_cs);
    bridge_load();
}

static void config_save(void)
{
    wchar_t key[32];
    port_save(g_port);
    WritePrivateProfileStringW(L"Settings", L"Initialized", L"1", g_ini);
    WritePrivateProfileStringW(L"Apps", NULL, NULL, g_ini);   /* clear section */

    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_appCount; i++) {
        swprintf(key, 32, L"App%d", i);
        WritePrivateProfileStringW(L"Apps", key, g_apps[i], g_ini);
    }
    LeaveCriticalSection(&g_cs);
}

/* ---------- start with Windows ---------- */

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static BOOL autostart_get(void)
{
    HKEY k;
    BOOL on = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}

static void autostart_set(BOOL on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &k) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t cmd[MAX_PATH + 16];
        swprintf(cmd, MAX_PATH + 16, L"\"%ls\"", g_exe);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)cmd,
                       (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

/* ---------- command handling ---------- */

static BOOL name_matches(const wchar_t *path, const wchar_t *name)
{
    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (_wcsicmp(base, name) == 0) return TRUE;
    size_t n = wcslen(name);
    return n + 4 == wcslen(base) && _wcsnicmp(base, name, n) == 0 &&
           _wcsicmp(base + n, L".exe") == 0;
}

/* Returns NULL on success, otherwise an error string. */
static const char *handle_command(char *line)
{
    wchar_t wline[MAX_LINE];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, wline, MAX_LINE);
    if (n <= 0) return "bad encoding";

    for (wchar_t *p = wline; *p; p++)
        if (*p < 0x20) return "control characters not allowed";

    wchar_t *p = wline;
    while (*p == L' ') p++;
    wchar_t *name = p;
    if (*p == L'"') {
        name = ++p;
        while (*p && *p != L'"') p++;
    } else {
        while (*p && *p != L' ') p++;
    }
    if (*p) *p++ = 0;
    while (*p == L' ') p++;
    if (!*name) return "empty command";

    wchar_t path[MAX_PATH] = L"";
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_appCount; i++) {
        if (name_matches(g_apps[i], name)) {
            wcscpy(path, g_apps[i]);
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    if (!path[0]) return "application not allowed";

    HINSTANCE r = ShellExecuteW(NULL, L"open", path, *p ? p : NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) return "launch failed";
    return NULL;
}

/* ---------- HTTP bridge (GET /open) ---------- */

static void http_reply(SOCKET c, int code, const char *reason, const char *ctype, const char *body)
{
    char out[1024];
    int bl = (int)strlen(body);
    int n = snprintf(out, sizeof out,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n%s",
        code, reason, ctype, bl, body);
    if (n > 0 && n < (int)sizeof out) send(c, out, n, 0);
}

#define HTTP_FAIL(code, reason, msg) \
    http_reply(c, code, reason, "text/plain; charset=utf-8", msg)

/* Case-insensitive header lookup in the header block (after the request line).
 * Returns the number of occurrences; copies the first value (trimmed) to out. */
static int hdr_get(const char *req, const char *name, char *out, size_t outsz)
{
    size_t nl = strlen(name);
    int count = 0;
    const char *p = strstr(req, "\r\n");
    out[0] = 0;
    while (p) {
        p += 2;
        if (strncmp(p, "\r\n", 2) == 0 || !*p) break;
        if (_strnicmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            const char *e = strstr(v, "\r\n");
            size_t vl = e ? (size_t)(e - v) : strlen(v);
            while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
            while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
            if (count == 0) {
                if (vl >= outsz) vl = outsz - 1;
                memcpy(out, v, vl);
                out[vl] = 0;
            }
            count++;
        }
        p = strstr(p, "\r\n");
    }
    return count;
}

static int hexv(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Percent-decode [s, s+n) into out (NUL-terminated). Returns length or -1. */
static int pct_decode(const char *s, size_t n, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '%') {
            int h, l;
            if (i + 2 >= n) return -1;
            h = hexv(s[i + 1]); l = hexv(s[i + 2]);
            if (h < 0 || l < 0) return -1;
            ch = (unsigned char)(h * 16 + l);
            i += 2;
        }
        if (ch == 0 || o + 1 >= outsz) return -1;     /* embedded NUL would truncate */
        out[o++] = (char)ch;
    }
    out[o] = 0;
    return (int)o;
}

/* Find query parameter `key`; returns decoded length, -1 (missing) or -2 (invalid
 * escape / duplicated key). */
static int query_get(const char *q, const char *key, char *out, size_t outsz)
{
    size_t kl = strlen(key);
    int found = -1;
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t len = amp ? (size_t)(amp - q) : strlen(q);
        if (len > kl && strncmp(q, key, kl) == 0 && q[kl] == '=') {
            if (found != -1) return -2;
            found = pct_decode(q + kl + 1, len - kl - 1, out, outsz);
            if (found < 0) return -2;
        }
        if (!amp) break;
        q = amp + 1;
    }
    return found;
}

static int ct_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned d = (unsigned)(la ^ lb);
    for (size_t i = 0; i < lb; i++) d |= (unsigned)((unsigned char)a[i < la ? i : 0] ^ (unsigned char)b[i]);
    return d == 0;
}

/* Canonical (symlink/junction-resolved) path via a real handle. 0 = failed. */
static int canon_path(const wchar_t *in, wchar_t *out, DWORD outsz)
{
    wchar_t full[MAX_PATH + 8];
    DWORD fl = GetFullPathNameW(in, MAX_PATH, full, NULL);
    if (fl == 0 || fl >= MAX_PATH) return 0;
    HANDLE h = CreateFileW(full, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    wchar_t fin[MAX_PATH + 8];
    DWORD r = GetFinalPathNameByHandleW(h, fin, MAX_PATH, VOLUME_NAME_DOS);
    CloseHandle(h);
    if (r == 0 || r >= MAX_PATH) return 0;
    const wchar_t *p = fin;
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) return 0;      /* network paths: no */
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    if (wcslen(p) >= outsz) return 0;
    wcscpy(out, p);
    size_t n = wcslen(out);
    while (n > 3 && out[n - 1] == L'\\') out[--n] = 0;       /* keep "D:\" intact */
    return 1;
}

/* path is under root, matching only on a path-component boundary. */
static int under_root(const wchar_t *path, const wchar_t *root)
{
    size_t rl = wcslen(root);
    while (rl > 0 && root[rl - 1] == L'\\') rl--;
    if (rl == 0) return 0;
    if (_wcsnicmp(path, root, rl) != 0) return 0;
    return path[rl] == 0 || path[rl] == L'\\';
}

/* Validates a decoded UTF-8 path; returns NULL and fills canon on success. */
static const char *bridge_resolve(const char *upath, wchar_t *canon)
{
    wchar_t w[MAX_PATH], ok[MAX_PATH];
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, upath, -1, w, MAX_PATH);
    if (n <= 0) return "bad path";
    for (wchar_t *p = w; *p; p++) {
        if (*p < 0x20 || wcschr(L"*?<>|\"", *p)) return "bad path";
        if (*p == L'/') *p = L'\\';
    }
    if (!((w[0] >= L'A' && w[0] <= L'Z') || (w[0] >= L'a' && w[0] <= L'z')) ||
        w[1] != L':' || w[2] != L'\\') return "bad path";          /* drive-absolute only */
    for (const wchar_t *seg = w + 3; *seg; ) {                      /* reject ".." component */
        const wchar_t *e = wcschr(seg, L'\\');
        size_t len = e ? (size_t)(e - seg) : wcslen(seg);
        if (len == 2 && seg[0] == L'.' && seg[1] == L'.') return "bad path";
        if (!e) break;
        seg = e + 1;
    }
    if (!canon_path(w, ok, MAX_PATH)) return "not found";

    int allowed = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_rootCount && !allowed; i++) {
        wchar_t rc[MAX_PATH];
        if (canon_path(g_roots[i], rc, MAX_PATH) && under_root(ok, rc)) allowed = 1;
    }
    LeaveCriticalSection(&g_cs);
    if (!allowed) return "outside allowed roots";
    wcscpy(canon, ok);
    return NULL;
}

static int explorer_path(wchar_t *out)
{
    int found = 0;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_appCount; i++)
        if (name_matches(g_apps[i], L"explorer")) { wcscpy(out, g_apps[i]); found = 1; break; }
    LeaveCriticalSection(&g_cs);
    return found;
}

static void handle_http(SOCKET c, char *req)
{
    char line[2048], val[512], tok[128], path[MAX_PATH * 3], sel[16];
    char *eol = strstr(req, "\r\n");
    size_t ll = eol ? (size_t)(eol - req) : strlen(req);
    if (ll >= sizeof line) { HTTP_FAIL(414, "URI Too Long", "uri too long"); return; }
    memcpy(line, req, ll); line[ll] = 0;

    /* request line: GET <target> HTTP/1.x */
    char *sp1 = strchr(line, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) { HTTP_FAIL(400, "Bad Request", "bad request"); return; }
    *sp1 = 0; *sp2 = 0;
    char *target = sp1 + 1;
    if (strcmp(line, "GET") != 0) { HTTP_FAIL(405, "Method Not Allowed", "method not allowed"); return; }

    char *qm = strchr(target, '?');
    const char *query = "";
    if (qm) { *qm = 0; query = qm + 1; }
    int isOpen = strcmp(target, "/open") == 0, isPing = strcmp(target, "/ping") == 0;
    if (!isOpen && !isPing) { HTTP_FAIL(404, "Not Found", ""); return; }

    EnterCriticalSection(&g_cs);
    int on = g_bridgeOn;
    char token[TOKEN_LEN + 1];
    strcpy(token, g_token);
    LeaveCriticalSection(&g_cs);
    if (!on) { HTTP_FAIL(403, "Forbidden", "bridge disabled"); return; }

    /* 1. Host must be exactly 127.0.0.1:<port> (anti DNS-rebinding) */
    char want[32];
    snprintf(want, sizeof want, "127.0.0.1:%d", g_port);
    if (hdr_get(req, "Host", val, sizeof val) != 1 || strcmp(val, want) != 0) {
        HTTP_FAIL(403, "Forbidden", "bad host"); return;
    }
    if (isPing) { http_reply(c, 200, "OK", "text/plain; charset=utf-8", "ai-helper"); return; }

    /* 2. Sec-Fetch-*: only a real user-initiated top-level navigation */
    if (hdr_get(req, "Sec-Fetch-Mode", val, sizeof val) != 1 || strcmp(val, "navigate") != 0 ||
        hdr_get(req, "Sec-Fetch-User", val, sizeof val) != 1 || strcmp(val, "?1") != 0 ||
        hdr_get(req, "Sec-Fetch-Dest", val, sizeof val) != 1 || strcmp(val, "document") != 0) {
        HTTP_FAIL(403, "Forbidden", "not a user navigation"); return;
    }
    /* 3. Sec-Fetch-Site: same-origin / same-site / cross-site accepted (cross-site = a click from
       Paperclip on another origin; step 2 already guarantees a user-click top-level navigation).
       "none" (link opened from outside a web page, e.g. a desktop shell) is accepted only together with step 2 and the token; a missing header is refused. */
    if (hdr_get(req, "Sec-Fetch-Site", val, sizeof val) != 1 ||
        (strcmp(val, "same-site") != 0 && strcmp(val, "same-origin") != 0 && strcmp(val, "cross-site") != 0 && strcmp(val, "none") != 0)) {
        HTTP_FAIL(403, "Forbidden", "bad fetch site"); return;
    }
    /* 4. token */
    if (query_get(query, "token", tok, sizeof tok) < 0 || !ct_equal(tok, token)) {
        HTTP_FAIL(403, "Forbidden", "bad token"); return;
    }
    /* 5-6. path: canonical + inside a whitelisted root */
    if (query_get(query, "path", path, sizeof path) <= 0) { HTTP_FAIL(400, "Bad Request", "bad path"); return; }
    if (query_get(query, "select", sel, sizeof sel) == -2) { HTTP_FAIL(400, "Bad Request", "bad select"); return; }
    wchar_t canon[MAX_PATH];
    const char *err = bridge_resolve(path, canon);
    if (err) {
        if (strcmp(err, "not found") == 0) HTTP_FAIL(404, "Not Found", "not found");
        else if (strcmp(err, "bad path") == 0) HTTP_FAIL(400, "Bad Request", "bad path");
        else HTTP_FAIL(403, "Forbidden", err);
        return;
    }

    /* 7. launch Explorer only: folder = open, file = highlight (never execute) */
    wchar_t exe[MAX_PATH], args[MAX_PATH + 16];
    if (!explorer_path(exe)) { HTTP_FAIL(500, "Internal Server Error", "explorer not allowlisted"); return; }
    DWORD at = GetFileAttributesW(canon);
    if (at == INVALID_FILE_ATTRIBUTES) { HTTP_FAIL(404, "Not Found", "not found"); return; }
    if (at & FILE_ATTRIBUTE_DIRECTORY) swprintf(args, MAX_PATH + 16, L"\"%ls\"", canon);
    else                               swprintf(args, MAX_PATH + 16, L"/select,\"%ls\"", canon);
    int stub = getenv("AIHELPER_TEST_NOLAUNCH") != NULL;     /* test hook: skip the real launch */
    if (!stub) {
        HINSTANCE r = ShellExecuteW(NULL, L"open", exe, args, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) { HTTP_FAIL(500, "Internal Server Error", "launch failed"); return; }
    }

    char page[384];
    snprintf(page, sizeof page,
        "<!doctype html><meta charset=utf-8><title>AI Helper</title>"
        "<body>Opened in Explorer. You can close this tab."
        "<script>setTimeout(function(){window.close()},200)</script>%s</body>",
        stub ? (wcsncmp(args, L"/select,", 8) != 0 ? "<!--would-launch:open-->" : "<!--would-launch:select-->") : "");
    http_reply(c, 200, "OK", "text/html; charset=utf-8", page);
}

static void serve_client(SOCKET c)
{
    char buf[MAX_REQ];
    int len = 0, http = 0, done = 0;
    DWORD timeout = 3000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);

    while (len < MAX_REQ - 1 && !done) {
        int r = recv(c, buf + len, MAX_REQ - 1 - len, 0);
        if (r <= 0) break;
        len += r;
        buf[len] = 0;
        if (len >= 4 && strncmp(buf, "GET ", 4) == 0) {
            http = 1;
            done = strstr(buf, "\r\n\r\n") != NULL;      /* end of HTTP headers */
        } else {
            done = memchr(buf, '\n', len) != NULL;       /* legacy: one line */
        }
    }
    buf[len] = 0;

    if (http) {
        if (!done) { http_reply(c, 431, "Request Header Fields Too Large", "text/plain", "incomplete or too large"); return; }
        handle_http(c, buf);
        return;
    }
    char *nl = strpbrk(buf, "\r\n");
    if (nl) *nl = 0;

    const char *err = len ? handle_command(buf) : "no data";
    char reply[96];
    int rl = err ? snprintf(reply, sizeof reply, "ERR %s\r\n", err)
                 : snprintf(reply, sizeof reply, "OK\r\n");
    send(c, reply, rl, 0);
}

/* ---------- server thread ---------- */

static DWORD WINAPI server_thread(LPVOID arg)
{
    int port = (int)(INT_PTR)arg;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        PostMessageW(g_hwnd, WM_SERVER_STATE, 0, WSAGetLastError());
        return 1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);     /* localhost only */
    addr.sin_port = htons((u_short)port);

    if (bind(s, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR ||
        listen(s, 8) == SOCKET_ERROR) {
        PostMessageW(g_hwnd, WM_SERVER_STATE, 0, WSAGetLastError());
        closesocket(s);
        return 1;
    }
    PostMessageW(g_hwnd, WM_SERVER_STATE, 1, port);

    while (!g_stop) {
        fd_set fds;
        struct timeval tv = {0, 250000};
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        if (select(0, &fds, NULL, NULL, &tv) > 0) {
            SOCKET c = accept(s, NULL, NULL);
            if (c != INVALID_SOCKET) {
                serve_client(c);
                closesocket(c);
            }
        }
    }
    closesocket(s);
    return 0;
}

static void server_stop(void)
{
    if (g_thread) {
        InterlockedExchange(&g_stop, 1);
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

static void server_start(int port)
{
    server_stop();
    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(NULL, 0, server_thread, (LPVOID)(INT_PTR)port, 0, NULL);
}

/* ---------- GUI ---------- */

static void set_status(const wchar_t *text) { SetWindowTextW(g_status, text); }

static void list_fill(void)
{
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_appCount; i++)
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)g_apps[i]);
    LeaveCriticalSection(&g_cs);
}

/* ---------- bridge folders (COH-517) ---------- */

/* Decision/security function: validates a candidate bridge root. existing[0..n) are the
 * folders already in the list. Returns NULL and fills canon (canonical form) if acceptable,
 * else a human-readable reason. (a) drive-absolute, not UNC/relative; (b) exists and is a
 * folder; (c) not a drive root; (d) not a duplicate; (e) room left (< MAX_ROOTS). */
static const wchar_t *root_validate(const wchar_t *path, wchar_t *canon,
                                    wchar_t (*existing)[MAX_PATH], int n)
{
    wchar_t c[MAX_PATH];
    if (!path || !path[0]) return L"No folder selected.";
    if (path[0] == L'\\' && path[1] == L'\\') return L"Network (UNC) folders are not allowed.";
    if (!((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || path[2] != L'\\')
        return L"Folder must be an absolute local path such as D:\\folder.";
    if (!canon_path(path, c, MAX_PATH)) return L"Folder does not exist or is not accessible.";
    DWORD at = GetFileAttributesW(c);
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY))
        return L"Selected path is not a folder.";
    if (wcslen(c) <= 3) return L"A whole drive (for example C:\\ or D:\\) is not allowed. Pick a folder.";
    for (int i = 0; i < n; i++) {
        wchar_t e[MAX_PATH];
        if (!canon_path(existing[i], e, MAX_PATH)) wcscpy(e, existing[i]);
        if (_wcsicmp(e, c) == 0) return L"This folder is already in the list.";
    }
    if (n >= MAX_ROOTS) return L"Folder list is full (maximum 16).";
    wcscpy(canon, c);
    return NULL;
}

static int roots_collect(wchar_t (*out)[MAX_PATH])
{
    LRESULT cnt = SendMessageW(g_rootList, LB_GETCOUNT, 0, 0);
    int k = 0;
    for (int i = 0; i < cnt && k < MAX_ROOTS; i++)
        if (SendMessageW(g_rootList, LB_GETTEXTLEN, i, 0) < MAX_PATH)
            SendMessageW(g_rootList, LB_GETTEXT, i, (LPARAM)out[k++]);
    return k;
}

static void roots_fill(void)
{
    SendMessageW(g_rootList, LB_RESETCONTENT, 0, 0);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_rootCount; i++)
        SendMessageW(g_rootList, LB_ADDSTRING, 0, (LPARAM)g_roots[i]);
    LeaveCriticalSection(&g_cs);
}

/* Applies the list: swap into g_roots under g_cs (no I/O inside the lock), then persist
 * [Bridge] Root0..RootN and delete surplus slots. Effective at once, no restart. */
static void roots_apply(void)
{
    static wchar_t loc[MAX_ROOTS][MAX_PATH];
    wchar_t key[32];
    int n = roots_collect(loc);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < n; i++) wcscpy(g_roots[i], loc[i]);
    g_rootCount = n;
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < MAX_ROOTS; i++) {
        swprintf(key, 32, L"Root%d", i);
        WritePrivateProfileStringW(L"Bridge", key, i < n ? loc[i] : NULL, g_ini);
    }
}

static int pick_folder(HWND owner, wchar_t *out)
{
    IFileOpenDialog *d = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&d))) return 0;
    DWORD o = 0;
    d->lpVtbl->GetOptions(d, &o);
    d->lpVtbl->SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->lpVtbl->SetTitle(d, L"Add bridge folder");
    if (SUCCEEDED(d->lpVtbl->Show(d, owner))) {
        IShellItem *it = NULL;
        if (SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                if (wcslen(p) < MAX_PATH) { wcscpy(out, p); ok = 1; }
                CoTaskMemFree(p);
            }
            it->lpVtbl->Release(it);
        }
    }
    d->lpVtbl->Release(d);
    return ok;
}

static void on_root_add(HWND hwnd)
{
    static wchar_t ex[MAX_ROOTS][MAX_PATH];
    wchar_t picked[MAX_PATH], canon[MAX_PATH];
    if (!pick_folder(hwnd, picked)) return;
    const wchar_t *why = root_validate(picked, canon, ex, roots_collect(ex));
    if (why) { MessageBoxW(hwnd, why, APP_NAME, MB_OK | MB_ICONWARNING); return; }
    SendMessageW(g_rootList, LB_ADDSTRING, 0, (LPARAM)canon);
}

static void on_root_remove(void)
{
    LRESULT sel = SendMessageW(g_rootList, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) SendMessageW(g_rootList, LB_DELETESTRING, sel, 0);
}

static void on_add(HWND hwnd)
{
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Applications (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Add allowed application";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    if (SendMessageW(g_list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)file) != LB_ERR) return;
    if (SendMessageW(g_list, LB_GETCOUNT, 0, 0) >= MAX_APPS) {
        MessageBoxW(hwnd, L"Allowlist is full.", APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)file);
}

static void on_remove(void)
{
    LRESULT sel = SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) SendMessageW(g_list, LB_DELETESTRING, sel, 0);
}

/* Applies the 'Enable HTTP bridge' checkbox: updates g_bridgeOn under g_cs (effective at
 * once, no restart) and persists only [Bridge] Enabled. Enabling is refused without a token.
 * Returns 1 if the requested state was applied, 0 if ON was refused (no token). */
static int bridge_set_enabled(int on)
{
    int ok = 1;
    EnterCriticalSection(&g_cs);
    if (on && !g_token[0]) { on = 0; ok = 0; }
    g_bridgeOn = on;
    LeaveCriticalSection(&g_cs);
    WritePrivateProfileStringW(L"Bridge", L"Enabled", on ? L"1" : L"0", g_ini);
    return ok;
}

static void on_save(HWND hwnd)
{
    wchar_t buf[16];
    GetWindowTextW(g_edPort, buf, 16);
    int port = _wtoi(buf);
    if (port < 1 || port > 65535) {
        MessageBoxW(hwnd, L"Port must be between 1 and 65535.", APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }

    LRESULT cnt = SendMessageW(g_list, LB_GETCOUNT, 0, 0);
    EnterCriticalSection(&g_cs);
    g_appCount = 0;
    for (int i = 0; i < cnt && i < MAX_APPS; i++) {
        if (SendMessageW(g_list, LB_GETTEXTLEN, i, 0) < MAX_PATH)
            SendMessageW(g_list, LB_GETTEXT, i, (LPARAM)g_apps[g_appCount++]);
    }
    LeaveCriticalSection(&g_cs);

    roots_apply();
    g_port = port;
    config_save();
    if (!g_testMode)
        autostart_set(SendMessageW(g_chkAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);
    int bridgeOk = bridge_set_enabled(SendMessageW(g_chkBridge, BM_GETCHECK, 0, 0) == BST_CHECKED);
    server_start(g_port);
    if (bridgeOk) {
        set_status(L"Saved. Restarting listener...");
    } else {
        SendMessageW(g_chkBridge, BM_SETCHECK, BST_UNCHECKED, 0);
        set_status(L"Saved. Bridge not enabled: no token");
    }
}

/* ---------- DPI-aware layout ---------- */

static HWND  g_lblPort, g_lblApps, g_lblHelp, g_btnAdd, g_btnRemove, g_btnSave;
static HFONT g_font;
static UINT  g_dpi = 96;

#define S(x) MulDiv((x), (int)g_dpi, 96)

static FARPROC user32_proc(const char *name)
{
    return GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
}

static void dpi_enable(void)
{
    typedef BOOL (WINAPI *PFN)(HANDLE);
    PFN f = (PFN)user32_proc("SetProcessDpiAwarenessContext");
    if (f && f((HANDLE)-4))                 /* PER_MONITOR_AWARE_V2 */
        return;
    typedef BOOL (WINAPI *PFN_OLD)(void);
    PFN_OLD o = (PFN_OLD)user32_proc("SetProcessDPIAware");
    if (o) o();
}

static UINT dpi_of(HWND hwnd)
{
    typedef UINT (WINAPI *PFN)(HWND);
    PFN f = (PFN)user32_proc("GetDpiForWindow");
    UINT d = (f && hwnd) ? f(hwnd) : 0;
    if (!d) {
        HDC dc = GetDC(NULL);
        d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    return d ? d : 96;
}

static void adjust_rect(RECT *rc, DWORD style)
{
    typedef BOOL (WINAPI *PFN)(LPRECT, DWORD, BOOL, DWORD, UINT);
    PFN f = (PFN)user32_proc("AdjustWindowRectExForDpi");
    if (f) f(rc, style, FALSE, 0, g_dpi);
    else   AdjustWindowRectEx(rc, style, FALSE, 0);
}

static BOOL CALLBACK set_font_cb(HWND child, LPARAM font)
{
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

/* (Re)build the UI font for the current DPI and apply it to all controls. */
static void font_rebuild(HWND hwnd)
{
    typedef BOOL (WINAPI *PFN)(UINT, UINT, PVOID, UINT, UINT);
    NONCLIENTMETRICSW ncm = {0};
    PFN f = (PFN)user32_proc("SystemParametersInfoForDpi");
    LOGFONTW lf = {0};
    ncm.cbSize = sizeof ncm;
    if (f && f(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_dpi)) {
        lf = ncm.lfMessageFont;
    } else {
        lf.lfHeight = -MulDiv(9, (int)g_dpi, 72);
        wcscpy(lf.lfFaceName, L"Segoe UI");
    }
    HFONT nf = CreateFontIndirectW(&lf);
    if (!nf) return;
    EnumChildWindows(hwnd, set_font_cb, (LPARAM)nf);
    if (g_font) DeleteObject(g_font);
    g_font = nf;
}

static HWND g_lblRoots, g_lblRootNote, g_btnRootAdd, g_btnRootRemove;

static void tab_show(void)
{
    int br = (int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0) == 1;
    HWND gen[] = {g_chkAuto, g_lblPort, g_edPort, g_lblApps, g_list, g_btnAdd, g_btnRemove, g_lblHelp};
    HWND bri[] = {g_chkBridge, g_lblRoots, g_rootList, g_btnRootAdd, g_btnRootRemove, g_lblRootNote};
    for (size_t k = 0; k < sizeof gen / sizeof *gen; k++) ShowWindow(gen[k], br ? SW_HIDE : SW_SHOW);
    for (size_t k = 0; k < sizeof bri / sizeof *bri; k++) ShowWindow(bri[k], br ? SW_SHOW : SW_HIDE);
    RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

/* Position every control from the current client size and DPI. */
static void layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    int m = S(12), row = S(24), edH = S(26), btnW = S(88), btnH = S(28), gap = S(8);

    /* Save/status stay below the tabs (one Save for both pages) */
    int by = ch - m - btnH;
    MoveWindow(g_btnSave, cw - m - btnW, by, btnW, btnH, TRUE);
    MoveWindow(g_status, m, by + S(6), cw - 2 * m - btnW - gap, btnH - S(6), TRUE);

    int tb = by - gap;
    MoveWindow(g_tab, m, m, cw - 2 * m, tb - m, TRUE);
    RECT r = {m, m, cw - m, tb};
    SendMessageW(g_tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&r);
    int x0 = r.left + gap, w = r.right - r.left - 2 * gap, yb = r.bottom - gap;

    /* General page */
    int y = r.top + gap;
    MoveWindow(g_chkAuto, x0, y, w, row, TRUE);
    y += row + gap;
    MoveWindow(g_lblPort, x0, y + S(4), S(200), row - S(4), TRUE);
    MoveWindow(g_edPort, x0 + S(200), y, S(80), edH, TRUE);
    y += edH + gap;
    MoveWindow(g_lblApps, x0, y, w, row - S(4), TRUE);
    y += row - S(4) + S(2);
    int hy = yb - S(40);
    MoveWindow(g_lblHelp, x0, hy, w, S(40), TRUE);
    int ay = hy - gap - btnH;
    MoveWindow(g_btnAdd, x0, ay, btnW, btnH, TRUE);
    MoveWindow(g_btnRemove, x0 + btnW + gap, ay, btnW, btnH, TRUE);
    int lh = ay - gap - y;
    if (lh < S(40)) lh = S(40);
    MoveWindow(g_list, x0, y, w, lh, TRUE);

    /* Bridge page */
    int bw = S(110);
    y = r.top + gap;
    MoveWindow(g_chkBridge, x0, y, w, row, TRUE);
    y += row + gap;
    MoveWindow(g_lblRoots, x0, y, w, row - S(4), TRUE);
    y += row - S(4) + S(2);
    int ny = yb - S(34);
    MoveWindow(g_lblRootNote, x0, ny, w, S(34), TRUE);
    int rh = ny - gap - y;
    if (rh < S(40)) rh = S(40);
    MoveWindow(g_rootList, x0, y, w - bw - gap, rh, TRUE);
    MoveWindow(g_btnRootAdd, x0 + w - bw, y, bw, btnH, TRUE);
    MoveWindow(g_btnRootRemove, x0 + w - bw, y + btnH + gap, bw, btnH, TRUE);

    /* tab stays at the bottom of the z-order so page controls are never painted over;
     * full redraw (children too) avoids stale edges/ghosts while the window is dragged */
    SetWindowPos(g_tab, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static HWND make(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
    return CreateWindowExW(cls[0] == L'E' || cls[0] == L'L' ? WS_EX_CLIENTEDGE : 0,
                           cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0,
                           parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static NOTIFYICONDATAW g_nid;

/* Icon 1 is embedded from ai-helper.rc; falls back to the stock icon. */
static HICON app_icon(int cx, int cy)
{
    HICON i = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1),
                                IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    return i ? i : LoadIconW(NULL, IDI_APPLICATION);
}

static void tray_add(HWND hwnd)
{
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = app_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wcscpy(g_nid.szTip, L"AI Helper");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void show_settings(void)
{
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        wchar_t port[16];
        g_hwnd = hwnd;
        g_dpi = dpi_of(hwnd);
        g_tab = make(hwnd, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, ID_TAB);
        {
            TCITEMW ti = {0};
            ti.mask = TCIF_TEXT;
            ti.pszText = L"General";
            SendMessageW(g_tab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
            ti.pszText = L"Bridge";
            SendMessageW(g_tab, TCM_INSERTITEMW, 1, (LPARAM)&ti);
        }
        g_chkAuto = make(hwnd, L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, ID_AUTOSTART);
        g_chkBridge = make(hwnd, L"BUTTON", L"Enable HTTP bridge (GET /open, clickable links)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_BRIDGE);
        g_lblPort = make(hwnd, L"STATIC", L"Listening port (127.0.0.1):", 0, 0);
        swprintf(port, 16, L"%d", g_port);
        g_edPort = make(hwnd, L"EDIT", port, ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, ID_PORT);
        SendMessageW(g_edPort, EM_LIMITTEXT, 5, 0);
        g_lblApps = make(hwnd, L"STATIC", L"Allowed applications:", 0, 0);
        g_list = make(hwnd, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, ID_LIST);
        g_btnAdd = make(hwnd, L"BUTTON", L"Add...", BS_PUSHBUTTON | WS_TABSTOP, ID_ADD);
        g_btnRemove = make(hwnd, L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, ID_REMOVE);
        g_lblRoots = make(hwnd, L"STATIC", L"Bridge folders (files under these can be opened):", 0, 0);
        g_rootList = make(hwnd, L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, ID_ROOTLIST);
        g_btnRootAdd = make(hwnd, L"BUTTON", L"Add folder...", BS_PUSHBUTTON | WS_TABSTOP, ID_ROOTADD);
        g_btnRootRemove = make(hwnd, L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, ID_ROOTREMOVE);
        g_lblRootNote = make(hwnd, L"STATIC", L"Whole drives (C:\\, D:\\) and network (UNC) folders are not allowed. Click Save to apply.", 0, 0);
        g_lblHelp = make(hwnd, L"STATIC", L"Send one line per connection:  <app> [params]\r\nExample:  explorer C:\\Temp", 0, 0);
        g_status = make(hwnd, L"STATIC", L"Starting...", 0, ID_STATUS);
        g_btnSave = make(hwnd, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_SAVE);

        font_rebuild(hwnd);
        layout(hwnd);
        SendMessageW(g_chkAuto, BM_SETCHECK, autostart_get() ? BST_CHECKED : BST_UNCHECKED, 0);
        EnterCriticalSection(&g_cs);
        int bOn = g_bridgeOn, bTok = g_token[0] != 0;
        LeaveCriticalSection(&g_cs);
        SendMessageW(g_chkBridge, BM_SETCHECK, bOn ? BST_CHECKED : BST_UNCHECKED, 0);
        if (!bTok) EnableWindow(g_chkBridge, FALSE);
        list_fill();
        roots_fill();
        tab_show();
        tray_add(hwnd);
        server_start(g_port);
        if (!bTok) set_status(L"Bridge unavailable: no token (RNG failed)");
        return 0;
    }
    case WM_SIZE:
        if (g_btnSave && wp != SIZE_MINIMIZED) layout(hwnd);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        RECT r = {0, 0, S(400), S(410)};
        adjust_rect(&r, WS_OVERLAPPEDWINDOW);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;       /* suggested size/position at the new DPI */
        g_dpi = HIWORD(wp);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        font_rebuild(hwnd);
        layout(hwnd);
        return 0;
    }
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == ID_TAB && ((NMHDR *)lp)->code == TCN_SELCHANGE) tab_show();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ROOTADD:        on_root_add(hwnd); break;
        case ID_ROOTREMOVE:     on_root_remove(); break;
        case ID_ADD:            on_add(hwnd); break;
        case ID_REMOVE:         on_remove(); break;
        case ID_SAVE:           on_save(hwnd); break;
        case ID_TRAY_SHOW:      show_settings(); break;
        case ID_TRAY_EXIT:      DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_SERVER_STATE: {
        wchar_t t[160];
        EnterCriticalSection(&g_cs);
        int noTok = !g_token[0];
        LeaveCriticalSection(&g_cs);
        if (wp) swprintf(t, 160, L"Listening on 127.0.0.1:%d%ls", (int)lp,
                         noTok ? L" - bridge unavailable: no token" : L"");
        else    swprintf(t, 160, L"Cannot listen (Winsock error %d)", (int)lp);
        set_status(t);
        return 0;
    }
    case WM_TRAY:
        if (lp == WM_LBUTTONDBLCLK) {
            show_settings();
        } else if (lp == WM_RBUTTONUP) {
            POINT pt;
            HMENU m = CreatePopupMenu();
            GetCursorPos(&pt);
            AppendMenuW(m, MF_STRING, ID_TRAY_SHOW, L"Settings");
            AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);      /* keep running in the tray */
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        server_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int nShow)
{
    (void)hPrev; (void)nShow;
    wchar_t mname[64] = L"Local\\" APP_NAME L"_SingleInstance";
    wchar_t iniOverride[MAX_PATH] = L"";
    {
        const wchar_t *a = cmdLine ? wcsstr(cmdLine, L"/port:") : NULL;
        const wchar_t *b = cmdLine ? wcsstr(cmdLine, L"/ini:") : NULL;
        if (a && b) {                       /* test instance: needs both */
            g_portOverride = _wtoi(a + 6);
            wcsncpy(iniOverride, b + 5, MAX_PATH - 1);
            wchar_t *e = wcschr(iniOverride, L' ');
            if (e) *e = 0;
            if (g_portOverride >= 1 && g_portOverride <= 65535 && iniOverride[0]) {
                g_testMode = 1;
                swprintf(mname, 64, L"Local\\" APP_NAME L"_Test_%d", g_portOverride);
            } else {
                g_portOverride = 0;
            }
        }
    }

    dpi_enable();       /* must happen before any window is created */
    { INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_TAB_CLASSES }; InitCommonControlsEx(&ic); }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    HANDLE mutex = CreateMutexW(NULL, TRUE, mname);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"AI Helper is already running (see the tray icon).",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    InitializeCriticalSection(&g_cs);
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    config_init_path();
    if (g_testMode) wcscpy(g_ini, iniOverride);
    config_load();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = app_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.lpszClassName = APP_NAME L"Window";
    RegisterClassW(&wc);

    g_dpi = dpi_of(NULL);
    RECT rc = {0, 0, S(460), S(440)};
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    adjust_rect(&rc, style);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"AI Helper - Settings", style,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                              NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                 (LPARAM)app_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON)));
    /* always start minimized to the tray; open Settings from the tray icon */

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    WSACleanup();
    DeleteCriticalSection(&g_cs);
    CloseHandle(mutex);
    return 0;
}
