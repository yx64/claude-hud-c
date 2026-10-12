// The platform layer on Linux and macOS (see os.h), on the C library and POSIX. It aims at working
// output, not at the start-up time the Windows side is tuned for; that is left to whoever builds
// here. Paths follow path.posix; child processes run in a process group of their own.
#if defined(__linux__)
#define _GNU_SOURCE  // pipe2
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#include "os.h"

extern char **environ;

static const char *u8(us s) { return us_to_utf8(s, nullptr); }  // NUL-terminated, for system calls

static us from8(const char *s) { return s ? us_utf8(s, strlen(s)) : US_EMPTY; }

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

// ---- process ----

static int g_argc;
static char **g_argv;
static double g_start;

int main(int argc, char **argv) {
    g_start = mono_ms();
    g_argc = argc, g_argv = argv;
    return hud_main();
}

[[noreturn]] void os_exit(int code) { exit(code); }

uint32_t os_pid(void) { return (uint32_t)getpid(); }

#if defined(__APPLE__)
const char *const OS_INIT_NOTE = "[claude-hud] Note: On macOS, you may need to restart Claude Code for the HUD to appear.";
#else
const char *const OS_INIT_NOTE = nullptr;
#endif

int os_args(us **out) {
    int n = g_argc > 1 ? g_argc - 1 : 0;
    us *a = arena_alloc(sizeof(us) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) a[i] = from8(g_argv[i + 1]);
    *out = a;
    return n;
}

char *os_read_stdin(size_t *n) {
    size_t cap = 1 << 16, k = 0;
    char *buf = xmalloc(cap + YYJSON_PADDING_SIZE);
    if (!isatty(0)) {
        for (;;) {
            if (k + 65536 > cap) cap *= 2, buf = xrealloc(buf, cap + YYJSON_PADDING_SIZE);
            ssize_t got = read(0, buf + k, 65536);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            k += (size_t)got;
        }
    }
    memset(buf + k, 0, YYJSON_PADDING_SIZE);
    *n = k;
    return buf;
}

static bool write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t got = write(fd, p, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        p += got, n -= (size_t)got;
    }
    return true;
}

void os_write_out(const char *p, size_t n) { write_all(1, p, n); }

void write_err(const char *s) { write_all(2, s, strlen(s)); }

// From main(): unlike Windows, loading the program before that is not counted.
double os_run_ms(void) { return mono_ms() - g_start; }

// ---- environment and time ----

us env_get(const char *name) { return from8(getenv(name)); }

bool env_has(const char *name) { return getenv(name) != nullptr; }

// libuv's uv_os_homedir: HOME, else the password database.
us os_home(void) {
    const char *h = getenv("HOME");
    if (h && *h) return from8(h);
    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_dir) UNSUPPORTED("no home directory");
    return pw ? from8(pw->pw_dir) : US_EMPTY;
}

// os.tmpdir()
us os_temp_dir(void) {
    const char *names[] = {"TMPDIR", "TMP", "TEMP"};
    us t = US("/tmp");
    for (size_t i = 0; i < countof(names); i++) {
        const char *v = getenv(names[i]);
        if (v && *v) {
            t = from8(v);
            break;
        }
    }
    while (t.n > 1 && t.p[t.n - 1] == '/') t.n--;
    return t;
}

double os_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (double)((int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

bool local_time(double ms, ltime *o) {
    if (!isfinite(ms) || ms < 0) return false;
    time_t s = (time_t)floor(ms / 1000);
    struct tm tm;
    if (!localtime_r(&s, &tm)) return false;
    *o = (ltime){tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec};
    return true;
}

// ICU, which node formats times with, takes its default locale from LC_ALL, then LC_MESSAGES,
// then LANG, without codeset or modifier; none, C and POSIX all mean en-US to V8.
int os_locale(void) {
    const char *names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    const char *v = "";
    for (size_t i = 0; i < countof(names) && !*v; i++)
        if (getenv(names[i])) v = getenv(names[i]);
    size_t n = strcspn(v, ".@");
    if (n == 0 || (n == 1 && v[0] == 'C') || (n == 5 && !strncmp(v, "POSIX", 5)) || (n == 5 && !strncmp(v, "en_US", 5))) return 1;
    if (n == 5 && !strncmp(v, "zh_CN", 5)) return 0;
    return -1;
}

#if defined(__linux__)
// parseLinuxMeminfo: MemTotal and MemAvailable, in kB.
static bool meminfo(const char *text, const char *key, double *v) {
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : nullptr) {
        if (strncmp(p, key, kl) || p[kl] != ':') continue;
        const char *q = p + kl + 1;
        while (*q == ' ' || *q == '\t') q++;
        if (*q < '0' || *q > '9') return false;
        double x = 0;
        while (*q >= '0' && *q <= '9') x = x * 10 + (*q++ - '0');
        if (*q != ' ' && *q != '\t') return false;
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, "kB", 2)) return false;
        *v = x * 1024;
        return true;
    }
    return false;
}

bool os_memory(double *total, double *free) {
    size_t n;
    char *text = read_file(US("/proc/meminfo"), &n);
    if (text && meminfo(text, "MemTotal", total) && meminfo(text, "MemAvailable", free)) return true;
    // os.totalmem() and os.freemem(), which libuv takes from the same file when it can.
    if (!text || !meminfo(text, "MemTotal", total)) {
        long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
        if (pages <= 0 || size <= 0) return false;
        *total = (double)pages * (double)size;
    }
    if (!text || !meminfo(text, "MemAvailable", free)) {
        long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
        *free = pages > 0 && size > 0 ? (double)pages * (double)size : 0;
    }
    return true;
}
#elif defined(__APPLE__)
// What vm_stat reports: total minus active and wired pages.
bool os_memory(double *total, double *free) {
    uint64_t mem;
    size_t len = sizeof mem;
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0)) return false;
    *total = (double)mem;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page;
    if (host_page_size(mach_host_self(), &page) == KERN_SUCCESS &&
        host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        *free = *total - (double)(vm.active_count + vm.wire_count) * (double)page;
        return true;
    }
    *free = 0;
    return true;
}
#else
#error "os-posix.c knows how to read memory on Linux and macOS only"
#endif

// SHA-256 (FIPS 180-4), for the output speed cache's file names.
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

us sha256_hex(us s) {
    size_t n;
    const uint8_t *p = (const uint8_t *)us_to_utf8(s, &n);
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = n / 64 * 64;
    for (size_t i = 0; i < full; i += 64) sha256_block(h, p + i);
    uint8_t last[128] = {0};
    size_t rest = n - full, blocks = rest + 9 > 64 ? 2 : 1;
    memcpy(last, p + full, rest);
    last[rest] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) last[blocks * 64 - 1 - i] = (uint8_t)(bits >> (i * 8));
    for (size_t b = 0; b < blocks; b++) sha256_block(h, last + b * 64);
    static const char hex[] = "0123456789abcdef";
    uint16_t *o = arena_alloc(64 * 2);
    for (int i = 0; i < 32; i++) {
        uint8_t x = (uint8_t)(h[i / 4] >> (24 - i % 4 * 8));
        o[i * 2] = (uint8_t)hex[x >> 4], o[i * 2 + 1] = (uint8_t)hex[x & 15];
    }
    return (us){o, 64};
}

// ---- files ----

// A handle is the descriptor plus one, so that null stays the failure.
static int fd_of(void *h) { return (int)(intptr_t)h - 1; }

void *open_read(us path) {
    int fd = open(u8(path), O_RDONLY | O_CLOEXEC);
    return fd < 0 ? nullptr : (void *)(intptr_t)(fd + 1);
}

void os_close(void *h) { close(fd_of(h)); }

bool os_size(void *h, uint64_t *size) {
    struct stat st;
    if (fstat(fd_of(h), &st)) return false;
    *size = (uint64_t)st.st_size;
    return true;
}

char *read_rest(void *h, uint64_t from, size_t *n) {
    uint64_t size;
    if (!os_size(h, &size)) return nullptr;
    // A byte more than what is left, so the read that finds the end needs no bigger buffer.
    size_t cap = (size > from ? (size_t)(size - from) : 0) + 1, k = 0;
    char *p = xmalloc(cap + YYJSON_PADDING_SIZE + 1);
    for (;;) {
        if (k == cap) p = xrealloc(p, (cap *= 2) + YYJSON_PADDING_SIZE + 1);
        ssize_t got = pread(fd_of(h), p + k, cap - k, (off_t)(from + k));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        k += (size_t)got;
    }
    memset(p + k, 0, YYJSON_PADDING_SIZE + 1);
    *n = k;
    return p;
}

bool read_at(void *h, uint64_t at, void *dst, uint32_t n) {
    for (uint32_t k = 0; k < n;) {
        ssize_t got = pread(fd_of(h), (char *)dst + k, n - k, (off_t)(at + k));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        k += (uint32_t)got;
    }
    return true;
}

bool os_identity(void *h, uint64_t *volume, uint64_t *file, uint64_t *size) {
    struct stat st;
    if (fstat(fd_of(h), &st)) return false;
    *volume = (uint64_t)st.st_dev, *file = (uint64_t)st.st_ino, *size = (uint64_t)st.st_size;
    return true;
}

// node's mtimeMs, seconds and nanoseconds combined in doubles.
static double node_ms(const struct stat *st) {
#if defined(__APPLE__)
    struct timespec t = st->st_mtimespec;
#else
    struct timespec t = st->st_mtim;
#endif
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

fstat_t stat_path(us path) {
    fstat_t r = {0};
    struct stat st;
    if (stat(u8(path), &st)) return r;
    r.ok = true;
    r.is_dir = S_ISDIR(st.st_mode);
    r.is_file = S_ISREG(st.st_mode);
    r.mtime_ms = node_ms(&st);
    r.size = (uint64_t)st.st_size;
    return r;
}

bool file_exists(us path) { return access(u8(path), F_OK) == 0; }

int os_lstat_kind(us path) {
    struct stat st;
    if (lstat(u8(path), &st)) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 2;
}

bool real_path(us p, us *out) {
    char *r = realpath(u8(p), nullptr);
    if (!r) return false;
    *out = from8(r);
    free(r);
    return true;
}

int os_list_dir(us dir, DirEntry **out, int max) {
    DIR *d = opendir(u8(dir));
    if (!d) return -1;
    int n = 0, cap = 64;
    DirEntry *e = arena_alloc(sizeof(DirEntry) * (size_t)cap);
    struct dirent *de;
    while (n < max && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) {
            DirEntry *more = arena_alloc(sizeof(DirEntry) * (size_t)(cap *= 2));
            memcpy(more, e, sizeof(DirEntry) * (size_t)n);
            e = more;
        }
        e[n++] = (DirEntry){from8(de->d_name), de->d_type == DT_REG ? DE_FILE : de->d_type == DT_DIR ? DE_DIR : DE_OTHER};
    }
    closedir(d);
    *out = e;
    return n;
}

// fs.mkdirSync(dir, { recursive: true, mode: 0o700 }), as upstream makes its cache directories.
static void ensure_dir(us dir) {
    for (int32_t i = 1; i <= dir.n; i++)
        if (i == dir.n || dir.p[i] == '/') mkdir(u8(us_slice(dir, 0, i)), 0700);
}

void os_mkdir(us dir) { mkdir(u8(dir), 0777); }

bool write_bytes(us path, const void *p, size_t n, double mtime, int mode) {
    const char *f = u8(path);
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (mode == WRITE_NEW ? O_EXCL : O_TRUNC);
    int fd = open(f, flags, 0666);
    // The directory is made only once the first attempt says it is missing.
    if (fd < 0 && errno == ENOENT) {
        ensure_dir(dir_of(path));
        fd = open(f, flags, 0666);
    }
    if (fd < 0) return false;
    bool ok = write_all(fd, p, n);
    if (ok && mtime > 0) {
        double s = floor(mtime / 1000);
        struct timespec t = {(time_t)s, (long)((mtime - s * 1000) * 1e6)};
        futimens(fd, (struct timespec[]){t, t});
    }
    close(fd);
    return ok;
}

bool os_rename(us from, us to) { return rename(u8(from), u8(to)) == 0; }

bool os_append(us path, const void *p, size_t n) {
    int fd = open(u8(path), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    bool ok = write_all(fd, p, n);
    close(fd);
    return ok;
}

us os_exe_dir(void) {
    char buf[PATH_MAX];
#if defined(__APPLE__)
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size)) return US_EMPTY;
    char *r = realpath(buf, nullptr);
    us self = from8(r);
    free(r);
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return US_EMPTY;
    us self = us_utf8(buf, (size_t)n);
#endif
    return dir_of(self);
}

// ---- paths: path.posix ----

const uint16_t OS_SEP = '/';

bool is_sep(uint16_t c) { return c == '/'; }

// path.posix.resolve: a relative path is taken from the current directory.
us path_resolve(us p) {
    if (!path_is_absolute(p)) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) {
            UNSUPPORTED("current directory unreadable");
            return p;
        }
        p = us_cat(from8(cwd), US("/"), p);
    }
    us parts[512];
    int np = 0;
    for (int32_t i = 1; i < p.n;) {
        int32_t j = i;
        while (j < p.n && p.p[j] != '/') j++;
        us seg = {p.p + i, j - i};
        if (seg.n == 0 || us_eq_c(seg, ".")) {
        } else if (us_eq_c(seg, "..")) {
            if (np) np--;
        } else if (np < (int)countof(parts)) parts[np++] = seg;
        else UNSUPPORTED("path with too many segments");
        i = j + 1;
    }
    sb b = {0};
    if (!np) sb_ch(&b, '/');
    for (int i = 0; i < np; i++) sb_ch(&b, '/'), sb_us(&b, parts[i]);
    return sb_done(&b);
}

us path_join_n(int count, const us *parts) {
    sb b = {0};
    for (int i = 0; i < count; i++) {
        if (i && b.n && b.p[b.n - 1] != '/') sb_ch(&b, '/');
        sb_us(&b, parts[i]);
    }
    return path_resolve(sb_done(&b));
}

bool path_is_absolute(us p) { return p.n && p.p[0] == '/'; }

us dir_of(us p) {
    int32_t i = p.n - 1;
    while (i > 0 && p.p[i] != '/') i--;
    return i <= 0 ? US("/") : us_slice(p, 0, i);
}

us path_key(us p) { return p; }

us file_url_path(us resolved) { return resolved; }

// ---- child processes ----

// The environment with extra NAME=value entries replacing any of the same name.
static char **env_block(const char *const *extra) {
    size_t n = 0, ne = 0;
    while (environ[n]) n++;
    while (extra[ne]) ne++;
    char **e = arena_alloc(sizeof(char *) * (n + ne + 1));
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        bool replaced = false;
        for (size_t j = 0; j < ne && !replaced; j++) {
            size_t kl = (size_t)(strchr(extra[j], '=') - extra[j]);
            replaced = !strncmp(environ[i], extra[j], kl + 1);
        }
        if (!replaced) e[k++] = environ[i];
    }
    for (size_t j = 0; j < ne; j++) e[k++] = (char *)extra[j];
    e[k] = nullptr;
    return e;
}

// Where a bare name may be, in PATH order, as execvp searches; built before fork, so the child
// only has to try them.
static char **candidates(us exe) {
    if (us_find_ch(exe, '/', 0) >= 0) {
        char **c = arena_alloc(sizeof(char *) * 2);
        c[0] = (char *)u8(exe), c[1] = nullptr;
        return c;
    }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    size_t n = 1;
    for (const char *p = path; *p; p++) n += *p == ':';
    char **c = arena_alloc(sizeof(char *) * (n + 1));
    size_t k = 0;
    for (const char *p = path;;) {
        const char *q = strchr(p, ':');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        us dir = len ? us_utf8(p, len) : US(".");
        c[k++] = (char *)u8(us_cat(dir, US("/"), exe));
        if (!q) break;
        p = q + 1;
    }
    c[k] = nullptr;
    return c;
}

static double left_ms(double deadline) { return deadline - mono_ms(); }

int os_run(us exe, const char *const *args, us cwd, const char *const *env, uint32_t timeout, size_t max_out, us *out) {
    int pfd[2], nul = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nul < 0) return RUN_FAILED;
#if defined(__linux__)
    if (pipe2(pfd, O_CLOEXEC)) {
#else
    if (pipe(pfd) || fcntl(pfd[0], F_SETFD, FD_CLOEXEC) || fcntl(pfd[1], F_SETFD, FD_CLOEXEC)) {
#endif
        close(nul);
        return RUN_FAILED;
    }
    size_t na = 0;
    while (args[na]) na++;
    char **argv = arena_alloc(sizeof(char *) * (na + 2));
    argv[0] = (char *)u8(exe);
    for (size_t i = 0; i < na; i++) argv[i + 1] = (char *)args[i];
    argv[na + 1] = nullptr;
    char **envp = env[0] ? env_block(env) : environ, **cand = candidates(exe);
    const char *dir = u8(cwd);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(nul, 0), dup2(pfd[1], 1), dup2(nul, 2);
        if (chdir(dir)) _exit(127);
        for (char **c = cand; *c; c++) execve(*c, argv, envp);
        _exit(127);
    }
    close(pfd[1]);
    close(nul);
    if (pid < 0) {
        close(pfd[0]);
        return RUN_FAILED;
    }
    setpgid(pid, pid);  // also here, so the group exists whichever side runs first
    double deadline = mono_ms() + timeout;
    char *buf = arena_alloc(max_out + 1);
    size_t n = 0;
    int result = RUN_OK;
    for (;;) {
        double left = left_ms(deadline);
        struct pollfd p = {pfd[0], POLLIN, 0};
        int r = left > 0 ? poll(&p, 1, (int)ceil(left)) : 0;
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            result = r == 0 ? RUN_TIMEOUT : RUN_FAILED;
            break;
        }
        ssize_t got = read(pfd[0], buf + n, max_out + 1 - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        n += (size_t)got;
        if (n > max_out) {
            result = RUN_FAILED;
            break;
        }
    }
    close(pfd[0]);
    int status = 0;
    pid_t w = 0;
    // The output is complete; what is left of the time bounds the exit too.
    while (result == RUN_OK && (w = waitpid(pid, &status, WNOHANG)) == 0) {
        if (left_ms(deadline) <= 0) result = RUN_TIMEOUT;
        else poll(nullptr, 0, 1);
    }
    if (result != RUN_OK) {
        kill(-pid, SIGKILL);
        waitpid(pid, &status, 0);
        return result;
    }
    if (w < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return RUN_FAILED;
    *out = us_utf8(buf, n);
    return RUN_OK;
}

// execFile('git', ...): found on PATH when it starts.
bool os_find_git(us *out) {
    *out = US("git");
    return true;
}

const uint32_t OS_GIT_EXTRA_MS = 0;

bool os_find_jj(us cwd, us *out) {
    *out = US("jj");
    return true;
}
