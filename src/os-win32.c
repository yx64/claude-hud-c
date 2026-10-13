// The platform layer on Windows (see os.h): Win32 only, no C runtime. Paths are drive-absolute
// and follow path.win32; child processes run in a job object.
#include <math.h>
#include <string.h>
#include <windows.h>

#include <bcrypt.h>

#include "os.h"

// A NUL-terminated copy for Win32 calls.
static const wchar_t *wz(us s) {
    uint16_t *p = arena_alloc((size_t)s.n * 2 + 2);
    if (s.n) memcpy(p, s.p, (size_t)s.n * 2);
    p[s.n] = 0;
    return (const wchar_t *)p;
}

static us wus(const wchar_t *p, size_t n) { return (us){(const uint16_t *)p, (int32_t)n}; }

// ---- process ----

// The process entry, in place of the CRT's (see crt.c). No crash or critical-error dialog, which
// nobody is there to dismiss: Git Bash's sh already starts the HUD in this mode, but without Git
// Bash Claude Code runs the status line through PowerShell, which does not. git and jj inherit it,
// as they inherit node's under upstream.
[[noreturn]] void hud_start(void) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ExitProcess((UINT)hud_main());
}

[[noreturn]] void os_exit(int code) { ExitProcess((UINT)code); }

uint32_t os_pid(void) { return GetCurrentProcessId(); }

const char *const OS_INIT_NOTE = nullptr;

// Split as the CRT splits argv: the program name runs to the next quote when quoted, else to a
// space or tab; in the arguments 2n backslashes and a quote are n backslashes and a toggle, 2n+1
// are n and a literal quote, and a doubled quote inside quotes is a literal one.
int os_args(us **out) {
    const wchar_t *p = GetCommandLineW();
    bool quoted = false;
    for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++)
        if (*p == '"') quoted = !quoted;
    quoted = false;
    int n = 0, cap = 8;
    us *args = arena_alloc(sizeof(us) * (size_t)cap);
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        sb arg = {0};
        for (;;) {
            bool copy = true;
            int slashes = 0;
            while (*p == '\\') p++, slashes++;
            if (*p == '"') {
                if (slashes % 2 == 0) {
                    if (quoted && p[1] == '"') p++;
                    else copy = false, quoted = !quoted;
                }
                slashes /= 2;
            }
            while (slashes--) sb_ch(&arg, '\\');
            if (!*p || (!quoted && (*p == ' ' || *p == '\t'))) break;
            if (copy) sb_ch(&arg, *p);
            p++;
        }
        if (n == cap) {
            us *more = arena_alloc(sizeof(us) * (size_t)(cap *= 2));
            memcpy(more, args, sizeof(us) * (size_t)n);
            args = more;
        }
        args[n++] = sb_done(&arg);
    }
    *out = args;
    return n;
}

char *os_read_stdin(size_t *n) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    size_t cap = 1 << 16, k = 0;
    char *buf = xmalloc(cap + YYJSON_PADDING_SIZE);
    if (GetFileType(in) != FILE_TYPE_CHAR) {
        for (;;) {
            if (k + 65536 > cap) cap *= 2, buf = xrealloc(buf, cap + YYJSON_PADDING_SIZE);
            DWORD got;
            if (!ReadFile(in, buf + k, 65536, &got, nullptr) || !got) break;
            k += got;
        }
    }
    memset(buf + k, 0, YYJSON_PADDING_SIZE);
    *n = k;
    return buf;
}

static void write_all(HANDLE h, const char *p, size_t n) {
    DWORD got;
    for (size_t k = 0; k < n; k += got)
        if (!WriteFile(h, p + k, (DWORD)(n - k), &got, nullptr) || !got) break;
}

void os_write_out(const char *p, size_t n) { write_all(GetStdHandle(STD_OUTPUT_HANDLE), p, n); }

void write_err(const char *s) { write_all(GetStdHandle(STD_ERROR_HANDLE), s, strlen(s)); }

// From process creation, so loading the exe counts.
double os_run_ms(void) {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimePreciseAsFileTime(&now);
    uint64_t c = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
    uint64_t t = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return t > c ? (double)(t - c) / 10000 : 0;
}

// ---- environment and time ----

us env_get(const char *name) {
    wchar_t wn[128];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 128);
    DWORD n = GetEnvironmentVariableW(wn, nullptr, 0);
    if (!n) return US_EMPTY;
    uint16_t *p = arena_alloc((size_t)n * 2);
    DWORD got = GetEnvironmentVariableW(wn, (LPWSTR)p, n);
    return (us){p, (int32_t)got};
}

bool env_has(const char *name) {
    wchar_t wn[128];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 128);
    SetLastError(0);
    DWORD n = GetEnvironmentVariableW(wn, nullptr, 0);
    return n > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

us os_home(void) {
    us h = env_get("USERPROFILE");
    if (!h.n) UNSUPPORTED("USERPROFILE unset");
    return h;
}

us os_temp_dir(void) {
    wchar_t t[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, t);
    if (!n || n > MAX_PATH) return US_EMPTY;
    while (n && t[n - 1] == '\\') n--;
    return us_cat(wus(t, n));
}

double os_now_ms(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t v = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // Whole milliseconds, as Date.now() gives them: the truncating division is the point.
    int64_t ms = (int64_t)(v - 116444736000000000ULL) / 10000;
    return (double)ms;
}

bool local_time(double ms, ltime *o) {
    static DYNAMIC_TIME_ZONE_INFORMATION tz;
    static bool init;
    // From 1970 to FILETIME's end in 30828; past that the arithmetic below would wrap around to a
    // plausible date.
    if (!(ms >= 0 && ms <= 910692730085477)) return false;
    if (!init) {
        GetDynamicTimeZoneInformation(&tz);
        init = true;
    }
    uint64_t v = (uint64_t)(int64_t)floor(ms) * 10000 + 116444736000000000ULL;
    FILETIME ft = {(DWORD)v, (DWORD)(v >> 32)};
    SYSTEMTIME st, lt;
    if (!FileTimeToSystemTime(&ft, &st) || !SystemTimeToTzSpecificLocalTimeEx(&tz, &st, &lt)) return false;
    *o = (ltime){lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond};
    return true;
}

// node's ICU takes its locale from the user's Windows setting, unless LANG and the like or TZ
// say otherwise, which this port does not follow.
int os_locale(void) {
    if (env_get("LC_ALL").n || env_get("LC_MESSAGES").n || env_get("LANG").n || env_get("TZ").n) return -1;
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) return -1;
    us n = wus(name, wcslen(name));
    return us_eq_c(n, "zh-CN") ? 0 : us_eq_c(n, "en-US") ? 1 : -1;
}

// os.totalmem() and os.freemem(): memory.js reads nothing finer on Windows.
bool os_memory(double *total, double *free) {
    MEMORYSTATUSEX ms = {sizeof ms};
    if (!GlobalMemoryStatusEx(&ms)) return false;
    *total = (double)ms.ullTotalPhys, *free = (double)ms.ullAvailPhys;
    return true;
}

us sha256_hex(us s) {
    size_t n;
    char *u8 = us_to_utf8(s, &n);
    // bcrypt is loaded here, on the first hash, not with the exe: only the output-speed option
    // hashes, and loading it at start costs every refresh about 0.7 ms. (The linker's delay-load
    // helper would do the same, but it needs the CRT.)
    static typeof(BCryptHash) *hash;
    if (!hash) {
        HMODULE dll = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        hash = dll ? (typeof(BCryptHash) *)(void *)GetProcAddress(dll, "BCryptHash") : nullptr;
    }
    uint8_t h[32];
    if (!hash || hash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)u8, (ULONG)n, h, 32) != 0) {
        write_err("claude-hud-c: BCryptHash from bcrypt.dll failed\r\n");
        ExitProcess(1);
    }
    static const char hex[] = "0123456789abcdef";
    uint16_t *o = arena_alloc(64 * 2);
    for (int i = 0; i < 32; i++) o[i * 2] = (uint8_t)hex[h[i] >> 4], o[i * 2 + 1] = (uint8_t)hex[h[i] & 15];
    return (us){o, 64};
}

// ---- files ----

void *open_read(us path) {
    HANDLE h = CreateFileW(wz(path), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

void os_close(void *h) { CloseHandle(h); }

bool os_size(void *h, uint64_t *size) {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) return false;
    *size = (uint64_t)sz.QuadPart;
    return true;
}

char *read_rest(void *h, uint64_t from, size_t *n) {
    LARGE_INTEGER sz, at = {.QuadPart = (LONGLONG)from};
    if (!GetFileSizeEx(h, &sz) || !SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) return nullptr;
    // A byte more than what is left, so the read that finds the end needs no bigger buffer; the
    // buffer only grows when the file grew since. The zeros yyjson wants follow whatever was read.
    size_t cap = ((uint64_t)sz.QuadPart > from ? (size_t)((uint64_t)sz.QuadPart - from) : 0) + 1, k = 0;
    char *p = xmalloc(cap + YYJSON_PADDING_SIZE + 1);
    for (;;) {
        if (k == cap) p = xrealloc(p, (cap *= 2) + YYJSON_PADDING_SIZE + 1);
        DWORD want = cap - k > (1u << 30) ? (1u << 30) : (DWORD)(cap - k), got;
        if (!ReadFile(h, p + k, want, &got, nullptr) || !got) break;
        k += got;
    }
    memset(p + k, 0, YYJSON_PADDING_SIZE + 1);
    *n = k;
    return p;
}

bool read_at(void *h, uint64_t at, void *dst, uint32_t n) {
    OVERLAPPED o = {.Offset = (DWORD)at, .OffsetHigh = (DWORD)(at >> 32)};
    DWORD got;
    return !n || (ReadFile(h, dst, n, &got, &o) && got == n);
}

bool os_identity(void *h, uint64_t *volume, uint64_t *file, uint64_t *size) {
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) return false;
    *volume = fi.dwVolumeSerialNumber;
    *file = ((uint64_t)fi.nFileIndexHigh << 32) | fi.nFileIndexLow;
    *size = ((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
    return true;
}

// node's mtimeMs: libuv splits the FILETIME into seconds and nanoseconds, JS recombines in doubles.
static double node_ms(FILETIME ft) {
    int64_t v = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000LL;
    int64_t sec = v / 10000000, nsec = (v % 10000000) * 100;
    if (nsec < 0) sec -= 1, nsec += 1000000000;
    return (double)sec * 1e3 + (double)nsec / 1e6;
}

fstat_t stat_path(us path) {
    fstat_t r = {0};
    const wchar_t *w = wz(path);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fa)) return r;
    // One call answers for anything but a link; a link is followed through a handle.
    if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        r.ok = true;
        r.is_dir = fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        r.is_file = !r.is_dir;
        r.mtime_ms = node_ms(fa.ftLastWriteTime);
        r.size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        return r;
    }
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileInformationByHandle(h, &fi)) {
        r.ok = true;
        r.is_dir = fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        r.is_file = !r.is_dir;
        r.mtime_ms = node_ms(fi.ftLastWriteTime);
        r.size = ((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
    }
    CloseHandle(h);
    return r;
}

bool file_exists(us path) { return GetFileAttributesW(wz(path)) != INVALID_FILE_ATTRIBUTES; }

int os_lstat_kind(us path) {
    DWORD a = GetFileAttributesW(wz(path));
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 2;
}

bool real_path(us p, us *out) {
    HANDLE h = CreateFileW(wz(p), 0, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = GetFinalPathNameByHandleW(h, nullptr, 0, VOLUME_NAME_DOS);
    uint16_t *buf = n ? arena_alloc((size_t)n * 2 + 2) : nullptr;
    DWORD got = buf ? GetFinalPathNameByHandleW(h, (LPWSTR)buf, n, VOLUME_NAME_DOS) : 0;
    CloseHandle(h);
    if (!got || got >= n) return false;
    us r = {buf, (int32_t)got};
    if (us_starts_c(r, "\\\\?\\UNC\\")) {
        buf[6] = '\\';
        *out = us_slice(r, 6, r.n);
    } else if (us_starts_c(r, "\\\\?\\")) *out = us_slice(r, 4, r.n);
    else return false;
    return true;
}

int os_list_dir(us dir, DirEntry **out, int max) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wz(path_join(dir, US("*"))), &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int n = 0, cap = 64;
    DirEntry *e = arena_alloc(sizeof(DirEntry) * (size_t)cap);
    do {
        us name = wus(fd.cFileName, wcslen(fd.cFileName));
        if (us_eq_c(name, ".") || us_eq_c(name, "..")) continue;
        if (n == cap) {
            DirEntry *more = arena_alloc(sizeof(DirEntry) * (size_t)(cap *= 2));
            memcpy(more, e, sizeof(DirEntry) * (size_t)n);
            e = more;
        }
        e[n++] = (DirEntry){us_cat(name), fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT ? DE_OTHER
                                             : fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY   ? DE_DIR
                                                                                                : DE_FILE};
    } while (n < max && FindNextFileW(h, &fd));
    FindClose(h);
    *out = e;
    return n;
}

// fs.mkdirSync(dir, { recursive: true })
static void ensure_dir(us dir) {
    for (int32_t i = 3; i <= dir.n; i++) {
        if (i == dir.n || dir.p[i] == '\\') CreateDirectoryW(wz(us_slice(dir, 0, i)), nullptr);
    }
}

void os_mkdir(us dir) { CreateDirectoryW(wz(dir), nullptr); }

static void stamp(HANDLE h, double ms) {
    uint64_t v = (uint64_t)(int64_t)(ms * 10000) + 116444736000000000ULL;
    FILETIME ft = {(DWORD)v, (DWORD)(v >> 32)};
    SetFileTime(h, nullptr, &ft, &ft);
}

bool write_bytes(us path, const void *p, size_t n, double mtime, int mode) {
    const wchar_t *w = wz(path);
    DWORD disposition = mode == WRITE_NEW ? CREATE_NEW : CREATE_ALWAYS;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    // The directory is made only once the first attempt says it is missing.
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
        ensure_dir(dir_of(path));
        h = CreateFileW(w, GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got;
    bool ok = WriteFile(h, p, (DWORD)n, &got, nullptr) && got == n;
    if (ok && mtime > 0) stamp(h, mtime);
    CloseHandle(h);
    return ok;
}

bool os_rename(us from, us to) { return MoveFileExW(wz(from), wz(to), MOVEFILE_REPLACE_EXISTING); }

bool os_append(us path, const void *p, size_t n) {
    HANDLE h = CreateFileW(wz(path), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got;
    bool ok = WriteFile(h, p, (DWORD)n, &got, nullptr) && got == n;
    CloseHandle(h);
    return ok;
}

us os_exe_dir(void) {
    wchar_t self[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    while (n > 0 && self[n - 1] != L'\\') n--;
    if (n > 0) n--;
    return us_cat(wus(self, n));
}

// ---- paths: path.win32 ----

const uint16_t OS_SEP = '\\';

bool is_sep(uint16_t c) { return c == '/' || c == '\\'; }

// path.resolve for drive-absolute paths. UNC and relative paths are reported unsupported and
// passed through as they are.
us path_resolve(us p) {
    if (!(p.n >= 3 && ((p.p[0] | 32) >= 'a' && (p.p[0] | 32) <= 'z') && p.p[1] == ':' && is_sep(p.p[2]))) {
        UNSUPPORTED("path that is not drive-absolute");
        return p;
    }
    us parts[512];
    int np = 0;
    for (int32_t i = 3; i < p.n;) {
        int32_t j = i;
        while (j < p.n && !is_sep(p.p[j])) j++;
        us seg = {p.p + i, j - i};
        if (seg.n == 0 || us_eq_c(seg, ".")) {
        } else if (us_eq_c(seg, "..")) {
            if (np) np--;
        } else if (np < (int)countof(parts)) parts[np++] = seg;
        else UNSUPPORTED("path with too many segments");
        i = j + 1;
    }
    sb b = {0};
    sb_ch(&b, p.p[0]);
    sb_us(&b, US(":\\"));
    for (int i = 0; i < np; i++) {
        if (i) sb_ch(&b, '\\');
        sb_us(&b, parts[i]);
    }
    return sb_done(&b);
}

us path_join_n(int count, const us *parts) {
    sb b = {0};
    for (int i = 0; i < count; i++) {
        if (i && b.n && !is_sep(b.p[b.n - 1])) sb_ch(&b, '\\');
        sb_us(&b, parts[i]);
    }
    return path_resolve(sb_done(&b));
}

bool path_is_absolute(us p) {
    if (!p.n) return false;
    if (p.p[0] == '/' || p.p[0] == '\\') return true;
    return p.n > 2 && ((p.p[0] | 32) >= 'a' && (p.p[0] | 32) <= 'z') && p.p[1] == ':' && (p.p[2] == '/' || p.p[2] == '\\');
}

us dir_of(us p) {
    int32_t i = p.n - 1;
    while (i > 0 && p.p[i] != '\\') i--;
    if (i == 2 && p.p[1] == ':') return us_slice(p, 0, 3);
    return us_slice(p, 0, i);
}

us path_key(us p) { return us_lower(p); }

// pathToFileURL: "C:\a b" becomes "/C:/a b", encoded afterwards.
us file_url_path(us resolved) {
    sb b = {0};
    sb_ch(&b, '/');
    for (int32_t i = 0; i < resolved.n; i++) sb_ch(&b, resolved.p[i] == '\\' ? '/' : resolved.p[i]);
    return sb_done(&b);
}

// ---- child processes ----

typedef struct {
    HANDLE pipe, overflow;
    char *buf;
    size_t n, max;
    bool over;
} Reader;

static DWORD WINAPI read_all(void *arg) {
    Reader *r = arg;
    for (;;) {
        char chunk[65536];
        DWORD got;
        if (!ReadFile(r->pipe, chunk, sizeof chunk, &got, nullptr) || !got) break;
        if (r->n + got > r->max) {
            r->over = true;
            SetEvent(r->overflow);
            break;
        }
        memcpy(r->buf + r->n, chunk, got);
        r->n += got;
    }
    return 0;
}

// The environment block with extra NAME=value entries replacing any of the same name, the name
// compared without case as Windows does.
static wchar_t *env_block(const char *const *extra) {
    wchar_t *cur = GetEnvironmentStringsW();
    sb b = {0};
    for (wchar_t *p = cur; *p; p += wcslen(p) + 1) {
        bool replaced = false;
        for (int i = 0; extra[i] && !replaced; i++) {
            size_t kl = strchr(extra[i], '=') - extra[i];
            replaced = _wcsnicmp(p, wz(us_utf8(extra[i], kl)), kl) == 0 && p[kl] == L'=';
        }
        if (replaced) continue;
        sb_us(&b, wus(p, wcslen(p)));
        sb_ch(&b, 0);
    }
    FreeEnvironmentStringsW(cur);
    for (int i = 0; extra[i]; i++) sb_c(&b, extra[i]), sb_ch(&b, 0);
    sb_ch(&b, 0);
    return (wchar_t *)sb_done(&b).p;
}

// An argument as libuv quotes it for the command line (quote_cmd_arg), which is how node passes
// arguments upstream: bare when nothing needs quoting, else in quotes with each quote escaped and
// the backslashes before an escaped or closing quote doubled.
static void quote_arg(sb *b, us a) {
    bool special = false, inner = false;
    for (int32_t i = 0; i < a.n; i++) {
        special |= a.p[i] == ' ' || a.p[i] == '\t' || a.p[i] == '"';
        inner |= a.p[i] == '"' || a.p[i] == '\\';
    }
    if (!a.n) return sb_us(b, US("\"\""));
    if (!special) return sb_us(b, a);
    sb_ch(b, '"');
    if (!inner) {
        sb_us(b, a);
        return sb_ch(b, '"');
    }
    // Built backwards, as libuv does, then reversed into place.
    sb r = {0};
    bool quote_hit = true;
    for (int32_t i = a.n; i > 0; i--) {
        uint16_t c = a.p[i - 1];
        sb_ch(&r, c);
        if (quote_hit && c == '\\') sb_ch(&r, '\\');
        else if (c == '"') quote_hit = true, sb_ch(&r, '\\');
        else quote_hit = false;
    }
    for (int32_t i = r.n; i > 0; i--) sb_ch(b, r.p[i - 1]);
    sb_ch(b, '"');
}

// stdin and stderr on NUL. The child and anything it starts run in a job that is ended on
// timeout, and also when this process dies first (kill on close); that limit is lifted before a
// normal close so a daemon git leaves behind survives.
int os_run(us exe, const char *const *args, us cwd, const char *const *env, uint32_t timeout, size_t max_out, us *out) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return RUN_FAILED;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE job = CreateJobObjectW(nullptr, nullptr), overflow = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {0};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    // A child that could not be bounded by the job counts as a failed run: it is never started.
    if (nul == INVALID_HANDLE_VALUE || !job || !overflow || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim)) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (job) CloseHandle(job);
        if (overflow) CloseHandle(overflow);
        CloseHandle(rd);
        CloseHandle(wr);
        return RUN_FAILED;
    }
    sb cmdline = {0};
    sb_ch(&cmdline, '"'), sb_us(&cmdline, exe), sb_ch(&cmdline, '"');
    for (int i = 0; args[i]; i++) sb_ch(&cmdline, ' '), quote_arg(&cmdline, us_c(args[i]));
    wchar_t *cmd = (wchar_t *)wz(sb_done(&cmdline));  // CreateProcessW may write into it
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul, si.hStdOutput = wr, si.hStdError = nul;
    PROCESS_INFORMATION pi;
    bool started = CreateProcessW(wz(exe), cmd, nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                  env[0] ? env_block(env) : nullptr, wz(cwd), &si, &pi);
    CloseHandle(wr);
    CloseHandle(nul);
    Reader r = {rd, overflow, arena_alloc(max_out + 1), 0, max_out, false};
    HANDLE th = nullptr;
    if (started && AssignProcessToJobObject(job, pi.hProcess)) th = CreateThread(nullptr, 0, read_all, &r, 0, nullptr);
    if (!th) {
        // Still suspended: outside the job, or with nobody to read its output, it does not run.
        if (started) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        CloseHandle(rd);
        CloseHandle(overflow);
        CloseHandle(job);
        return RUN_FAILED;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    ULONGLONG t0 = GetTickCount64();
    HANDLE waits[2] = {pi.hProcess, r.overflow};
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeout);
    int result = RUN_OK;
    if (w == WAIT_OBJECT_0) {
        // The output may still be draining; what is left of the time bounds that too.
        ULONGLONG spent = GetTickCount64() - t0;
        HANDLE w2[2] = {th, r.overflow};
        DWORD left = spent >= timeout ? 0 : (DWORD)(timeout - spent);
        DWORD d = WaitForMultipleObjects(2, w2, FALSE, left);
        if (d == WAIT_TIMEOUT) result = RUN_TIMEOUT;
        else if (d == WAIT_OBJECT_0 + 1) result = RUN_FAILED;
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        if (result == RUN_OK && code != 0) result = RUN_FAILED;
    } else result = w == WAIT_TIMEOUT ? RUN_TIMEOUT : RUN_FAILED;
    if (result != RUN_OK) TerminateJobObject(job, 1);
    else {
        lim.BasicLimitInformation.LimitFlags = 0;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    }
    CancelSynchronousIo(th);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    CloseHandle(rd);
    CloseHandle(r.overflow);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    if (result == RUN_OK) *out = us_utf8(r.buf, r.n);
    return result;
}

// resolveWindowsGitExecutable: git from absolute PATH entries only, so a git planted in the
// repository is never picked up.
bool os_find_git(us *out) {
    us path = env_get("PATH");
    for (int32_t i = 0; i <= path.n && path.n;) {
        int32_t j = us_find_ch(path, ';', i);
        if (j < 0) j = path.n;
        us e = us_trim(us_slice(path, i, j));
        i = j + 1;
        if (e.n >= 2 && e.p[0] == '"' && e.p[e.n - 1] == '"') e = us_slice(e, 1, e.n - 1);
        if (!e.n || !path_is_absolute(e)) continue;
        us real;
        if (!real_path(path_join(e, US("git.exe")), &real)) continue;
        fstat_t st = stat_path(real);
        if (st.ok && st.is_file) {
            *out = real;
            return true;
        }
    }
    return false;
}

// runGit gives the worker it starts git through this long for its own start.
const uint32_t OS_GIT_EXTRA_MS = 1500;

// libuv's search for a bare name: the child's cwd first, then PATH, as .com then .exe
bool os_find_jj(us cwd, us *out) {
    us path = env_get("PATH");
    us dirs[256];
    int nd = 0;
    dirs[nd++] = cwd;
    for (int32_t i = 0; i <= path.n && path.n && nd < 256;) {
        int32_t j = us_find_ch(path, ';', i);
        if (j < 0) j = path.n;
        us e = us_slice(path, i, j);
        i = j + 1;
        if (e.n >= 2 && e.p[0] == '"' && e.p[e.n - 1] == '"') e = us_slice(e, 1, e.n - 1);
        if (e.n && path_is_absolute(e)) dirs[nd++] = e;
    }
    for (int d = 0; d < nd; d++)
        for (int x = 0; x < 2; x++) {
            us c = path_join(dirs[d], x ? US("jj.exe") : US("jj.com"));
            fstat_t st = stat_path(c);
            if (st.ok && st.is_file) {
                *out = c;
                return true;
            }
        }
    return false;
}
