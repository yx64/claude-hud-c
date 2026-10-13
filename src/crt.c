// The part of the C library that this program, yyjson, utf8proc and the compiler's own code call,
// in place of the CRT, which is not linked: its start-up costs every refresh about 0.8 ms. The
// process entry is hud_start in os-win32.c.
//
// Built apart from the rest, freestanding and without LTO (see CMakeLists.txt): freestanding keeps
// the compiler from turning a loop here into a call to the very function it is in, and the build
// checks the assembly for any call or jump out of a function before linking that same assembly.
// Elsewhere the compiler keeps its builtins, so a memcpy of a constant size stays inline.
//
// x86-64 and ARM64. Where the two differ, x86-64 uses rep movsb / rep stosb, SSE2 and x87;
// ARM64 has none of them and does the same in plain C.
#if defined(__x86_64__)
#include <emmintrin.h>
#elif !defined(__aarch64__)
#error "src/crt.c has code for x86-64 and ARM64 only"
#endif
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// Unaligned loads and stores, legal for any object type.
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef unsigned char __attribute__((vector_size(16), aligned(1), may_alias)) v16u;

int _fltused;  // the compiler references it from any code that uses floating point

// ---- memory ----

// Small blocks are moved by overlapping loads and stores, all loads first, which also makes them
// safe for memmove. On x86-64 anything larger goes to rep movsb / rep stosb, which current CPUs
// run at cache speed.
static inline void move_small(unsigned char *d, const unsigned char *s, size_t n) {
    if (n >= 33) {
        v16u a = *(const v16u *)s, b = *(const v16u *)(s + 16);
        v16u c = *(const v16u *)(s + n - 32), e = *(const v16u *)(s + n - 16);
        *(v16u *)d = a, *(v16u *)(d + 16) = b;
        *(v16u *)(d + n - 32) = c, *(v16u *)(d + n - 16) = e;
    } else if (n >= 17) {
        v16u a = *(const v16u *)s, b = *(const v16u *)(s + n - 16);
        *(v16u *)d = a, *(v16u *)(d + n - 16) = b;
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

#if defined(__aarch64__)
// More than 64 bytes, 32 a step. The last 32 are loaded before the loop and stored after it, and
// every step loads before it stores, so a forward copy is safe whenever d is not inside s.
static void move_forward(unsigned char *d, const unsigned char *s, size_t n) {
    v16u t0 = *(const v16u *)(s + n - 32), t1 = *(const v16u *)(s + n - 16);
    unsigned char *e = d + n - 32;
    for (; d < e; d += 32, s += 32) {
        v16u a = *(const v16u *)s, b = *(const v16u *)(s + 16);
        *(v16u *)d = a, *(v16u *)(d + 16) = b;
    }
    *(v16u *)e = t0, *(v16u *)(e + 16) = t1;
}

// The mirror image, from the end down with the first 32 kept for last, for d inside s.
static void move_backward(unsigned char *d, const unsigned char *s, size_t n) {
    v16u h0 = *(const v16u *)s, h1 = *(const v16u *)(s + 16);
    unsigned char *de = d + n;
    const unsigned char *se = s + n;
    for (; de > d + 32; de -= 32, se -= 32) {
        v16u a = *(const v16u *)(se - 32), b = *(const v16u *)(se - 16);
        *(v16u *)(de - 32) = a, *(v16u *)(de - 16) = b;
    }
    *(v16u *)d = h0, *(v16u *)(d + 16) = h1;
}
#endif

void *memcpy(void *restrict d, const void *restrict s, size_t n) {
    if (n <= 64) {
        move_small(d, s, n);
        return d;
    }
#if defined(__x86_64__)
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
#else
    move_forward(d, s, n);
    return d;
#endif
}

void *memmove(void *d, const void *s, size_t n) {
    if (n <= 64) {
        move_small(d, s, n);
        return d;
    }
    void *r = d;
    if ((uintptr_t)d - (uintptr_t)s >= n) {  // d before s, or past its end: a forward copy is safe
#if defined(__x86_64__)
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
        move_forward(d, s, n);
#endif
    } else {
#if defined(__x86_64__)
        unsigned char *de = (unsigned char *)d + n - 1;
        const unsigned char *se = (const unsigned char *)s + n - 1;
        __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(de), "+S"(se), "+c"(n) : : "memory");
#else
        move_backward(d, s, n);
#endif
    }
    return r;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    v16u x = (v16u){} + (unsigned char)c;
    if (n <= 32) {
        uint64_t v = 0x0101010101010101u * (unsigned char)c;
        if (n >= 16) *(v16u *)p = x, *(v16u *)(p + n - 16) = x;
        else if (n >= 8) *(u64u *)p = v, *(u64u *)(p + n - 8) = v;
        else if (n >= 4) *(u32u *)p = (uint32_t)v, *(u32u *)(p + n - 4) = (uint32_t)v;
        else if (n) p[0] = p[n / 2] = p[n - 1] = (unsigned char)c;
        return d;
    }
#if defined(__x86_64__)
    __asm__ volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(c) : "memory");
#else
    unsigned char *e = p + n - 32;
    for (; p < e; p += 32) *(v16u *)p = x, *(v16u *)(p + 16) = x;
    *(v16u *)e = x, *(v16u *)(e + 16) = x;
#endif
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

// Aligned blocks: a block never crosses a page, so reading the whole of one that holds the first
// or last byte asked for cannot fault.
#if defined(__x86_64__)
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
#else
// 8 bytes a step. In a word XORed with c, (x - ones) & ~x & highs marks the bytes that are zero;
// a borrow can mark a byte above a zero one as well, never one below, so the lowest mark is exact.
// The bytes before s are set to 0xFF first: they then neither match nor borrow.
void *memchr(const void *s, int c, size_t n) {
    if (!n) return nullptr;
    constexpr uint64_t ones = 0x0101010101010101u, highs = 0x8080808080808080u;
    const unsigned char *p = s, *a = (const unsigned char *)((uintptr_t)p & ~(uintptr_t)7);
    uint64_t v = ones * (unsigned char)c;
    uint64_t x = (*(const u64u *)a ^ v) | (((uint64_t)1 << ((p - a) * 8)) - 1);
    for (;;) {
        uint64_t m = (x - ones) & ~x & highs;
        if (m) {
            ptrdiff_t i = (a - p) + __builtin_ctzll(m) / 8;
            return (size_t)i < n ? (void *)(p + i) : nullptr;
        }
        a += 8;
        if ((size_t)(a - p) >= n) return nullptr;
        x = *(const u64u *)a ^ v;
    }
}
#endif

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

#if defined(__x86_64__)
// fprem is exact, as fmod must be; it reduces by at most 2^63 a step, hence the loop.
double fmod(double x, double y) {
    double r;
    __asm__("1:\n\tfprem\n\tfnstsw %%ax\n\ttestb $4, %%ah\n\tjnz 1b" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
    return r;
}
#else
typedef union {
    double d;
    uint64_t u;
} f64bits;

// The significand with its implicit bit at bit 52, a subnormal's shifted up to there; *e is the
// biased exponent, below 1 for a subnormal.
static uint64_t significand(uint64_t u, int *e) {
    uint64_t m = u & (((uint64_t)1 << 52) - 1);
    *e = (int)(u >> 52 & 0x7FF);
    if (*e) return m | (uint64_t)1 << 52;
    int shift = __builtin_clzll(m) - 11;
    *e = 1 - shift;
    return m << shift;
}

// No x87 here. Long division of the significands, one bit a step: every step is an exact integer
// subtraction, so the remainder is exact, as fmod must be.
double fmod(double x, double y) {
    f64bits bx = {x}, by = {y};
    uint64_t ax = bx.u << 1, ay = by.u << 1;  // magnitudes, sign shifted out
    constexpr uint64_t inf2 = (uint64_t)0x7FF << 53;
    if (ay == 0 || ax >= inf2 || ay > inf2) return (x * y) / (x * y);  // NaN
    if (ax <= ay) return ax == ay ? 0 * x : x;
    int ex, ey;
    uint64_t mx = significand(bx.u, &ex), my = significand(by.u, &ey);
    for (; ex > ey; ex--) {
        if (mx >= my) {
            mx -= my;
            if (!mx) return 0 * x;
        }
        mx <<= 1;
    }
    if (mx >= my) {
        mx -= my;
        if (!mx) return 0 * x;
    }
    for (; !(mx >> 52); mx <<= 1) ex--;
    if (ex > 0) mx = (mx & (((uint64_t)1 << 52) - 1)) | (uint64_t)ex << 52;
    else mx >>= 1 - ex;
    f64bits r = {.u = mx | (bx.u & (uint64_t)1 << 63)};
    return r.d;
}
#endif

// ---- stack probe ----

// A frame larger than a page calls this before moving the stack pointer. Windows grows the stack
// one guard page at a time, so every page between the stack limit in the TEB and the new stack
// pointer is touched in order, from the top down.
#if defined(__x86_64__)
// The frame size comes in rax. Preserves every register but the flags, as the compiler expects.
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
#else
// The frame size comes in x15, in 16-byte units, and the caller uses x15 again afterwards; x16,
// x17 and the flags are free to clobber. The TEB is at x18.
__asm__(".text\n"
        ".globl __chkstk\n"
        ".p2align 2\n"
        "__chkstk:\n"
        "\tmov x16, sp\n"
        "\tsubs x16, x16, x15, lsl #4\n"  // the new stack pointer
        "\tcsel x16, xzr, x16, lo\n"      // below zero: probe down to the bottom, which faults
        "\tldr x17, [x18, #16]\n"         // TEB StackLimit
        "\tcmp x16, x17\n"
        "\tb.hs 2f\n"
        "\tand x16, x16, #0xFFFFFFFFFFFFF000\n"
        "1:\n"
        "\tsub x17, x17, #4096\n"
        "\tstr xzr, [x17]\n"
        "\tcmp x17, x16\n"
        "\tb.ne 1b\n"
        "2:\n"
        "\tret\n");
#endif
