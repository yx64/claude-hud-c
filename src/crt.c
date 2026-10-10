// The part of the C library that this program, yyjson, utf8proc and the compiler's own code call,
// in place of the CRT, which is not linked: its start-up costs every refresh about 0.8 ms. The
// process entry is hud_start in os-win32.c.
//
// Built apart from the rest, freestanding and without LTO (see CMakeLists.txt): freestanding keeps
// the compiler from turning a loop here into a call to the very function it is in, and the build
// checks the assembly for any call or jump out of a function before linking that same assembly.
// Elsewhere the compiler keeps its builtins, so a memcpy of a constant size stays inline.
#include <emmintrin.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// Unaligned loads and stores, legal for any object type.
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;

int _fltused;  // the compiler references it from any code that uses floating point

// ---- memory ----

// Small blocks are moved by overlapping loads and stores, all loads first, which also makes them
// safe for memmove; anything larger goes to rep movsb / rep stosb, which current CPUs run at
// cache speed.
static inline void move_small(unsigned char *d, const unsigned char *s, size_t n) {
    if (n >= 33) {
        __m128i a = _mm_loadu_si128((const __m128i *)s), b = _mm_loadu_si128((const __m128i *)(s + 16));
        __m128i c = _mm_loadu_si128((const __m128i *)(s + n - 32)), e = _mm_loadu_si128((const __m128i *)(s + n - 16));
        _mm_storeu_si128((__m128i *)d, a), _mm_storeu_si128((__m128i *)(d + 16), b);
        _mm_storeu_si128((__m128i *)(d + n - 32), c), _mm_storeu_si128((__m128i *)(d + n - 16), e);
    } else if (n >= 17) {
        __m128i a = _mm_loadu_si128((const __m128i *)s), b = _mm_loadu_si128((const __m128i *)(s + n - 16));
        _mm_storeu_si128((__m128i *)d, a), _mm_storeu_si128((__m128i *)(d + n - 16), b);
    } else if (n >= 8) {
        uint64_t a = *(const u64u *)s, b = *(const u64u *)(s + n - 8);
        *(u64u *)d = a, *(u64u *)(d + n - 8) = b;
    } else if (n >= 4) {
        uint32_t a = *(const u32u *)s, b = *(const u32u *)(s + n - 4);
        *(u32u *)d = a, *(u32u *)(d + n - 4) = b;
    } else if (n) {
        unsigned char a = s[0], b = s[n / 2], c = s[n - 1];
        d[0] = a, d[n / 2] = b, d[n - 1] = c;
    }
}

void *memcpy(void *restrict d, const void *restrict s, size_t n) {
    if (n <= 64) {
        move_small(d, s, n);
        return d;
    }
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n) {
    if (n <= 64) {
        move_small(d, s, n);
        return d;
    }
    void *r = d;
    if ((uintptr_t)d - (uintptr_t)s >= n) {  // d before s, or past its end: a forward copy is safe
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        unsigned char *de = (unsigned char *)d + n - 1;
        const unsigned char *se = (const unsigned char *)s + n - 1;
        __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(de), "+S"(se), "+c"(n) : : "memory");
    }
    return r;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    if (n <= 32) {
        uint64_t v = 0x0101010101010101u * (unsigned char)c;
        if (n >= 16) {
            __m128i x = _mm_set1_epi8((char)c);
            _mm_storeu_si128((__m128i *)p, x), _mm_storeu_si128((__m128i *)(p + n - 16), x);
        } else if (n >= 8) *(u64u *)p = v, *(u64u *)(p + n - 8) = v;
        else if (n >= 4) *(u32u *)p = (uint32_t)v, *(u32u *)(p + n - 4) = (uint32_t)v;
        else if (n) p[0] = p[n / 2] = p[n - 1] = (unsigned char)c;
        return d;
    }
    __asm__ volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(c) : "memory");
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    for (; n >= 8; p += 8, q += 8, n -= 8) {
        uint64_t x = *(const u64u *)p, y = *(const u64u *)q;
        if (x != y) {
            x = __builtin_bswap64(x), y = __builtin_bswap64(y);
            return x < y ? -1 : 1;
        }
    }
    for (; n; p++, q++, n--)
        if (*p != *q) return *p < *q ? -1 : 1;
    return 0;
}

// Aligned 16-byte blocks: a block never crosses a page, so reading the whole of one that holds
// the first or last byte asked for cannot fault.
void *memchr(const void *s, int c, size_t n) {
    if (!n) return nullptr;
    const unsigned char *p = s, *a = (const unsigned char *)((uintptr_t)p & ~(uintptr_t)15);
    __m128i needle = _mm_set1_epi8((char)c);
    unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128((const __m128i *)a), needle)) & (0xFFFFu << (p - a));
    for (;;) {
        if (m) {
            ptrdiff_t i = (a - p) + __builtin_ctz(m);
            return (size_t)i < n ? (void *)(p + i) : nullptr;
        }
        a += 16;
        if ((size_t)(a - p) >= n) return nullptr;
        m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128((const __m128i *)a), needle));
    }
}

void *malloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n); }

void *realloc(void *p, size_t n) { return p ? HeapReAlloc(GetProcessHeap(), 0, p, n) : HeapAlloc(GetProcessHeap(), 0, n); }

void free(void *p) {
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

// ---- strings: short ones only, a plain loop each ----

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return nullptr;
    }
}

int strcmp(const char *a, const char *b) {
    for (; *a && *a == *b; a++, b++) {}
    return (unsigned char)*a - (unsigned char)*b;
}

// The CRT's own behaviour in its default "C" locale: only ASCII letters fold.
static int lower_ascii(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

size_t wcslen(const wchar_t *s) {
    const wchar_t *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int wcscmp(const wchar_t *a, const wchar_t *b) {
    for (; *a && *a == *b; a++, b++) {}
    return *a < *b ? -1 : *a > *b;
}

int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (; n; a++, b++, n--) {
        int x = lower_ascii(*a), y = lower_ascii(*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}

// ---- math ----

// Past 2^52 every double is an integer already; below it the integer conversion truncates, and
// copysign keeps the sign of a zero result, as in trunc(-0.5) == -0.0.
double trunc(double x) {
    if (!(__builtin_fabs(x) < 4503599627370496.0)) return x;  // NaN too
    return __builtin_copysign((double)(int64_t)x, x);
}

double floor(double x) {
    double t = trunc(x);
    return t > x ? t - 1 : t;
}

double ceil(double x) {
    double t = trunc(x);
    return t < x ? t + 1 : t;
}

// fprem is exact, as fmod must be; it reduces by at most 2^63 a step, hence the loop.
double fmod(double x, double y) {
    double r;
    __asm__("1:\n\tfprem\n\tfnstsw %%ax\n\ttestb $4, %%ah\n\tjnz 1b" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}

// ---- stack probe ----

// A frame larger than a page calls this with its size in rax before moving rsp. Windows grows
// the stack one guard page at a time, so every page between the stack limit in the TEB and the
// new rsp is touched in order, from the top down. Preserves every register but the flags, as the
// compiler expects.
__asm__(".text\n"
        ".globl __chkstk\n"
        ".p2align 4\n"
        "__chkstk:\n"
        "\tsubq $16, %rsp\n"
        "\tmovq %r10, (%rsp)\n"
        "\tmovq %r11, 8(%rsp)\n"
        "\txorl %r11d, %r11d\n"
        "\tleaq 24(%rsp), %r10\n"  // rsp in the caller
        "\tsubq %rax, %r10\n"
        "\tcmovbq %r11, %r10\n"
        "\tmovq %gs:16, %r11\n"  // TEB StackLimit
        "\tcmpq %r11, %r10\n"
        "\tjae 2f\n"
        "\tandw $0xF000, %r10w\n"
        "1:\n"
        "\tleaq -4096(%r11), %r11\n"
        "\tmovb $0, (%r11)\n"
        "\tcmpq %r11, %r10\n"
        "\tjne 1b\n"
        "2:\n"
        "\tmovq (%rsp), %r10\n"
        "\tmovq 8(%rsp), %r11\n"
        "\taddq $16, %rsp\n"
        "\tretq\n");
