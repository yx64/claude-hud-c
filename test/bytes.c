// Test helper, built by CMakeLists.txt as test-tools/bytes: the byte-exact file work of the test drivers.
// CMake cannot do it: on Windows file(WRITE) turns every \n into \r\n and file(READ) every \r\n into \n,
// and its strings stop at a NUL byte. Usage:
//   bytes subst <Src> <Dst> <Count> [<Name>=<Value>]...
//       <Src> with every \r dropped (a checkout may have added them), then @REP:<N>:<Text>@ expanded,
//       @<Name>@ replaced by <Value>, @x<hh>@ turned into that byte; written <Count> times to <Dst>
//   bytes canon <Src> <Dst> [<Name>=<Value>]...
//       <Src> with every \r written as @x0d@ and every <Value> as @<Name>@, in the order given; exit 3
//       when it holds a NUL byte
//   bytes join <Lines> <Dst> <Mode>
//       the lines of <Lines> (\r dropped, empty lines skipped), each ended by the terminator of <Mode>:
//       lf crlf cr ls (U+2028) ps (U+2029), or mixed for all five in turn. Prints a line per line
//       written: <End>,<Length of its terminator>,<1 for a lone \r, else 0>
//   bytes append <Src> <Dst> <From> <To>    appends bytes [<From>, <To>) of <Src> to <Dst>
//   bytes splice <File> <At> <Text>         overwrites the bytes at <At> with <Text>, in place
//   bytes cut <File> <Bytes>                keeps the first <Bytes> bytes, in place
// Arguments and paths are UTF-8; every failure exits non-zero with a message on stderr.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    wchar_t *w = malloc((size_t)n * sizeof *w);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static FILE *open_file(const char *path, const char *mode) {
    wchar_t *p = wide(path), *m = wide(mode);
    FILE *f = _wfopen(p, m);
    free(p);
    free(m);
    return f;
}
#else
static FILE *open_file(const char *path, const char *mode) { return fopen(path, mode); }
#endif

typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static void put(Buf *b, const void *x, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, x, n);
    b->n += n;
}

[[noreturn]] static void fail(const char *what, const char *path) {
    fprintf(stderr, "bytes: %s %s\n", what, path);
    exit(1);
}

static Buf slurp(const char *path) {
    FILE *f = open_file(path, "rb");
    if (!f) fail("cannot read", path);
    Buf b = {0};
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f))) put(&b, chunk, got);
    if (ferror(f)) fail("cannot read", path);
    fclose(f);
    return b;
}

static void spill(const char *path, const char *mode, const Buf *b, long count) {
    FILE *f = open_file(path, mode);
    if (!f) fail("cannot write", path);
    for (long i = 0; i < count; i++)
        if (b->n && fwrite(b->p, 1, b->n, f) != b->n) fail("cannot write", path);
    if (fclose(f)) fail("cannot write", path);
}

static long number(const char *s) {
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < 0) fail("not a count:", s);
    return v;
}

static Buf drop_cr(Buf in) {
    Buf out = {0};
    for (size_t i = 0; i < in.n; i++)
        if (in.p[i] != '\r') put(&out, in.p + i, 1);
    return out;
}

static bool starts(const Buf *b, size_t i, const char *s, size_t n) { return i + n <= b->n && memcmp(b->p + i, s, n) == 0; }

static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int subst(int argc, char **argv) {
    Buf b = drop_cr(slurp(argv[2]));
    long count = number(argv[4]);
    Buf out = {0};
    for (size_t i = 0; i < b.n;) {
        if (b.p[i] != '@') {
            put(&out, b.p + i, 1);
            i++;
            continue;
        }
        if (starts(&b, i, "@REP:", 5)) {  // @REP:<N>:<Text>@
            size_t j = i + 5;
            long n = 0;
            while (j < b.n && b.p[j] >= '0' && b.p[j] <= '9') n = n * 10 + (b.p[j++] - '0');
            size_t text = j + 1, end = text;
            while (end < b.n && b.p[end] != '@') end++;
            if (j == i + 5 || j >= b.n || b.p[j] != ':' || end >= b.n) fail("malformed @REP: in", argv[2]);
            for (long k = 0; k < n; k++) put(&out, b.p + text, end - text);
            i = end + 1;
            continue;
        }
        if (i + 4 < b.n && b.p[i + 1] == 'x' && hex(b.p[i + 2]) >= 0 && hex(b.p[i + 3]) >= 0 && b.p[i + 4] == '@') {
            char c = (char)(hex(b.p[i + 2]) * 16 + hex(b.p[i + 3]));
            if (!c) fail("@x00@ (a NUL byte) in", argv[2]);
            put(&out, &c, 1);
            i += 5;
            continue;
        }
        bool done = false;
        for (int a = 5; a < argc && !done; a++) {
            const char *eq = strchr(argv[a], '=');
            if (!eq) fail("not <Name>=<Value>:", argv[a]);
            size_t n = (size_t)(eq - argv[a]);
            if (i + n + 1 < b.n && b.p[i + n + 1] == '@' && memcmp(b.p + i + 1, argv[a], n) == 0) {
                put(&out, eq + 1, strlen(eq + 1));
                i += n + 2;
                done = true;
            }
        }
        if (!done) {
            put(&out, "@", 1);
            i++;
        }
    }
    spill(argv[3], "wb", &out, count);
    return 0;
}

static int canon(int argc, char **argv) {
    Buf b = slurp(argv[2]);
    if (memchr(b.p, 0, b.n)) {
        fprintf(stderr, "holds a NUL byte\n");
        return 3;
    }
    Buf cur = {0};
    for (size_t i = 0; i < b.n; i++) {
        if (b.p[i] == '\r') put(&cur, "@x0d@", 5);
        else put(&cur, b.p + i, 1);
    }
    for (int a = 4; a < argc; a++) {
        const char *eq = strchr(argv[a], '=');
        if (!eq || !eq[1]) fail("not <Name>=<Value>:", argv[a]);
        size_t vn = strlen(eq + 1);
        Buf next = {0};
        for (size_t i = 0; i < cur.n;) {
            if (starts(&cur, i, eq + 1, vn)) {
                put(&next, "@", 1);
                put(&next, argv[a], (size_t)(eq - argv[a]));
                put(&next, "@", 1);
                i += vn;
            } else {
                put(&next, cur.p + i, 1);
                i++;
            }
        }
        free(cur.p);
        cur = next;
    }
    spill(argv[3], "wb", &cur, 1);
    return 0;
}

static int join(char **argv) {
    static const char *const names[] = {"lf", "crlf", "cr", "ls", "ps"};
    static const char *const terms[] = {"\n", "\r\n", "\r", "\xe2\x80\xa8", "\xe2\x80\xa9"};
    int mode = -1;
    for (int k = 0; k < 5; k++)
        if (!strcmp(argv[4], names[k])) mode = k;
    if (mode < 0 && strcmp(argv[4], "mixed")) fail("unknown mode", argv[4]);
    Buf b = drop_cr(slurp(argv[2])), out = {0};
    int line = 0;
    for (size_t i = 0; i < b.n;) {
        size_t e = i;
        while (e < b.n && b.p[e] != '\n') e++;
        if (e > i) {
            int m = mode >= 0 ? mode : line % 5;
            put(&out, b.p + i, e - i);
            put(&out, terms[m], strlen(terms[m]));
            printf("%zu,%zu,%d\n", out.n, strlen(terms[m]), m == 2);
            line++;
        }
        i = e + 1;
    }
    spill(argv[3], "wb", &out, 1);
    return 0;
}

static int append(char **argv) {
    Buf b = slurp(argv[2]);
    long from = number(argv[4]), to = number(argv[5]);
    if (from > to || (size_t)to > b.n) fail("range outside", argv[2]);
    Buf piece = {b.p + from, (size_t)(to - from), 0};
    spill(argv[3], "ab", &piece, 1);
    return 0;
}

static int splice(char **argv) {
    Buf b = slurp(argv[2]);
    long at = number(argv[3]);
    size_t n = strlen(argv[4]);
    if ((size_t)at + n > b.n) fail("splice past the end of", argv[2]);
    memcpy(b.p + at, argv[4], n);
    spill(argv[2], "r+b", &b, 1);
    return 0;
}

static int cut(char **argv) {
    Buf b = slurp(argv[2]);
    long keep = number(argv[3]);
    if ((size_t)keep > b.n) fail("shorter than asked:", argv[2]);
    b.n = (size_t)keep;
    spill(argv[2], "wb", &b, 1);
    return 0;
}

static int run(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "";
    if (!strcmp(m, "subst") && argc >= 5) return subst(argc, argv);
    if (!strcmp(m, "canon") && argc >= 4) return canon(argc, argv);
    if (!strcmp(m, "join") && argc == 5) return join(argv);
    if (!strcmp(m, "append") && argc == 6) return append(argv);
    if (!strcmp(m, "splice") && argc == 5) return splice(argv);
    if (!strcmp(m, "cut") && argc == 4) return cut(argv);
    fputs("usage: bytes subst|canon|join|append|splice|cut ... (see test/bytes.c)\n", stderr);
    return 2;
}

#if defined(_WIN32)
// The arguments as UTF-8: main's argv would be in the ANSI code page, which cannot hold every path.
int wmain(int argc, wchar_t **wargv) {
    char **argv = malloc(((size_t)argc + 1) * sizeof *argv);
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        argv[i] = malloc((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, nullptr, nullptr);
    }
    argv[argc] = nullptr;
    return run(argc, argv);
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
