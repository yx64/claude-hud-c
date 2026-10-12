// Test stand-in for jj, built by CMakeLists.txt as fake-jj/jj and used by test/compare/fuzz.mjs: appends
// its arguments to FAKE_JJ_LOG, one line per call, so the arguments each side passed can be
// compared, then prints FAKE_JJ_OUT and exits with FAKE_JJ_EXIT. On Windows the line is the
// command line after argv[0] as it arrived, quoting included; elsewhere the arguments joined by
// spaces.
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

static const wchar_t *args_after_argv0(const wchar_t *cmd) {
    if (*cmd == L'"') {
        cmd++;
        while (*cmd && *cmd != L'"') cmd++;
        if (*cmd) cmd++;
    } else {
        while (*cmd && *cmd != L' ' && *cmd != L'\t') cmd++;
    }
    while (*cmd == L' ' || *cmd == L'\t') cmd++;
    return cmd;
}

static void write_utf8(HANDLE h, const wchar_t *s, int n) {
    int len = WideCharToMultiByte(CP_UTF8, 0, s, n, nullptr, 0, nullptr, nullptr);
    char *buf = malloc((size_t)len + 1);
    WideCharToMultiByte(CP_UTF8, 0, s, n, buf, len, nullptr, nullptr);
    DWORD got;
    WriteFile(h, buf, (DWORD)len, &got, nullptr);
    free(buf);
}

int main(void) {
    wchar_t path[32768], out[32768], code[32];
    if (GetEnvironmentVariableW(L"FAKE_JJ_LOG", path, 32768)) {
        HANDLE log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (log == INVALID_HANDLE_VALUE) return 90;
        const wchar_t *args = args_after_argv0(GetCommandLineW());
        write_utf8(log, args, (int)wcslen(args));
        write_utf8(log, L"\n", 1);
        CloseHandle(log);
    }
    DWORD n = GetEnvironmentVariableW(L"FAKE_JJ_OUT", out, 32768);
    if (n) write_utf8(GetStdHandle(STD_OUTPUT_HANDLE), out, (int)n);
    return GetEnvironmentVariableW(L"FAKE_JJ_EXIT", code, 32) ? _wtoi(code) : 0;
}
#else
#include <stdio.h>

int main(int argc, char **argv) {
    const char *log = getenv("FAKE_JJ_LOG"), *out = getenv("FAKE_JJ_OUT"), *code = getenv("FAKE_JJ_EXIT");
    if (log) {
        FILE *f = fopen(log, "a");
        if (!f) return 90;
        for (int i = 1; i < argc; i++) fprintf(f, i > 1 ? " %s" : "%s", argv[i]);
        fputc('\n', f);
        fclose(f);
    }
    if (out) fputs(out, stdout);
    return code ? atoi(code) : 0;
}
#endif
