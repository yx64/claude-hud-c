// Everything that differs between platforms, behind one interface: the system calls, and also the
// platform's own path rules, since node's path module follows the platform too. os-win32.c is the
// Windows side (no C runtime, see crt.c); os-posix.c is Linux and macOS, on the C library. Exactly
// one of them is compiled in, chosen by CMakeLists.txt; the rest of the program has no platform
// conditionals.
#pragma once
#include "base.h"

// ---- process ----

int hud_main(void);  // hud.c; the platform's entry point runs it and exits with its result
[[noreturn]] void os_exit(int code);
uint32_t os_pid(void);
int os_args(us **out);                 // the arguments after the program name
char *os_read_stdin(size_t *n);        // to EOF, padded for yyjson; nothing from a terminal
void os_write_out(const char *p, size_t n);
double os_run_ms(void);                // time this process has run so far, for the last line
extern const char *const OS_INIT_NOTE;  // a second line after "Initializing...", or null

// ---- environment and time ----

us env_get(const char *name);
bool env_has(const char *name);
us os_home(void);     // os.homedir()
us os_temp_dir(void); // without a trailing separator; empty when there is none
double os_now_ms(void);  // Date.now()
bool local_time(double ms, ltime *out);
int os_locale(void);  // node's default locale: 0 zh-CN, 1 en-US, -1 one this port does not reproduce
bool os_memory(double *total, double *free);  // memory.js readPlatformMemory, in bytes
us sha256_hex(us s);  // of the UTF-8 bytes

// ---- files ----

// An open file is a handle, null on failure.
void *open_read(us path);
void os_close(void *h);
bool os_size(void *h, uint64_t *size);
char *read_rest(void *h, uint64_t from, size_t *n);  // from an offset to the end, padded for yyjson
bool read_at(void *h, uint64_t at, void *dst, uint32_t n);  // exactly n bytes at an offset
bool os_identity(void *h, uint64_t *volume, uint64_t *file, uint64_t *size);  // what tells two files apart

typedef struct { bool ok, is_dir, is_file; double mtime_ms; uint64_t size; } fstat_t;
fstat_t stat_path(us path);  // fs.statSync: links followed
bool file_exists(us path);   // fs.existsSync
int os_lstat_kind(us path);  // fs.lstatSync: 0 missing, 1 a directory, 2 anything else
bool real_path(us p, us *out);  // fs.realpathSync.native, as libuv does it

// A directory listing without . and .., at most max entries; -1 when it cannot be read. A
// DE_OTHER entry (a link, or a file system that does not say) needs real_path and stat_path.
enum { DE_FILE, DE_DIR, DE_OTHER };
typedef struct { us name; int kind; } DirEntry;
int os_list_dir(us dir, DirEntry **out, int max);

// CREATE_NEW or CREATE_ALWAYS in Win32's terms, making the directory when it is missing; mtime > 0
// stamps both times, as fs.utimesSync does.
enum { WRITE_NEW, WRITE_ALWAYS };
bool write_bytes(us path, const void *p, size_t n, double mtime, int mode);
bool os_rename(us from, us to);  // replacing the target
bool os_append(us path, const void *p, size_t n);
void os_mkdir(us dir);
us os_exe_dir(void);

// ---- paths: node's path module on this platform ----

extern const uint16_t OS_SEP;
bool is_sep(uint16_t c);
us path_resolve(us p);
us path_join_n(int count, const us *parts);  // path.join of an absolute first part, which is then resolved too
#define path_join(...) path_join_n(countof(((us[]){__VA_ARGS__})), (us[]){__VA_ARGS__})  // an array: see us_cat
bool path_is_absolute(us p);
us dir_of(us p);           // path.dirname of a resolved path
us path_key(us p);         // the form two paths are compared in: folded where the platform folds case
us file_url_path(us resolved);  // the path part of pathToFileURL, before percent-encoding

// ---- child processes ----

enum { RUN_OK, RUN_FAILED, RUN_TIMEOUT };
// Runs exe with args (null-terminated), stdin and stderr empty, the environment plus env
// (null-terminated NAME=value entries, replacing any of the same name); the child and anything it
// starts end when it overruns timeout_ms. RUN_OK only for exit code 0 within max_out bytes.
int os_run(us exe, const char *const *args, us cwd, const char *const *env, uint32_t timeout_ms, size_t max_out, us *out);
bool os_find_git(us *out);         // git-runner.js: what runGit starts
extern const uint32_t OS_GIT_EXTRA_MS;  // and the start-up time it adds to git's timeout
bool os_find_jj(us cwd, us *out);  // what execFile('jj', ...) starts from cwd
