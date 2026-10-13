#include "base.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "utf8proc.h"

const char *g_unsupported;

[[noreturn]] static void out_of_memory(size_t n) {
    char msg[96] = "claude-hud-c: out of memory allocating ";
    char *p = put_int(msg + strlen(msg), (int64_t)n, 1);
    memcpy(p, " bytes\r\n", 9);
    write_err(msg);
    os_exit(1);
}

void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) out_of_memory(n);
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) out_of_memory(n);
    return q;
}

// ---- arena: the process lives for one refresh, so nothing is ever freed ----

static char *a_cur, *a_end;

void *arena_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if ((size_t)(a_end - a_cur) < n) {
        size_t sz = n > ((size_t)1 << 20) ? n : ((size_t)1 << 20);
        a_cur = xmalloc(sz);
        a_end = a_cur + sz;
    }
    void *p = a_cur;
    a_cur += n;
    return p;
}

void *arena_dup(const void *p, size_t n) {
    void *q = arena_alloc(n ? n : 1);
    if (n) memcpy(q, p, n);
    return q;
}

// ---- strings ----

// WHATWG UTF-8 decoding (maximal subpart -> U+FFFD), except that surrogate code points encoded
// as three bytes are kept: that is how lone \uD8xx escapes reach us (see json_fix_surrogates). Writes
// to o unless it is null; the number of code units either way.
static inline int32_t decode_utf8(const char *s, size_t n, uint16_t *o) {
#define PUT(u) ((o ? (void)(o[k] = (uint16_t)(u)) : (void)0), k++)
    int32_t k = 0;
    const uint8_t *p = (const uint8_t *)s, *e = p + n;
    while (p < e) {
        uint8_t c = *p;
        if (c < 0x80) {
            PUT(c);
            p++;
            continue;
        }
        uint32_t cp;
        int need;
        uint8_t lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) need = 1, cp = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF) need = 2, cp = c & 0x0F, lo = c == 0xE0 ? 0xA0 : 0x80;
        else if (c >= 0xF0 && c <= 0xF4) need = 3, cp = c & 0x07, lo = c == 0xF0 ? 0x90 : 0x80, hi = c == 0xF4 ? 0x8F : 0xBF;
        else {
            PUT(0xFFFD);
            p++;
            continue;
        }
        p++;
        int i = 0;
        for (; i < need; i++) {
            if (p >= e || *p < lo || *p > hi) break;
            cp = (cp << 6) | (*p & 0x3F);
            p++;
            lo = 0x80, hi = 0xBF;
        }
        if (i < need) {
            PUT(0xFFFD);
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            PUT(0xD800 + (cp >> 10));
            PUT(0xDC00 + (cp & 0x3FF));
        } else {
            PUT(cp);
        }
    }
    return k;
#undef PUT
}

us us_utf8(const char *s, size_t n) {
    uint16_t *o = arena_alloc((n + 1) * 2);
    return (us){o, decode_utf8(s, n, o)};
}

int32_t us_utf8_len(const char *s, size_t n) { return decode_utf8(s, n, nullptr); }

us us_c(const char *s) { return us_utf8(s, strlen(s)); }

char *us_to_utf8(us s, size_t *n) {
    char *o = arena_alloc((size_t)s.n * 3 + 1);
    size_t k = 0;
    for (int32_t i = 0; i < s.n; i++) {
        uint32_t c = s.p[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.n && s.p[i + 1] >= 0xDC00 && s.p[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s.p[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) o[k++] = (char)c;
        else if (c < 0x800) o[k++] = (char)(0xC0 | (c >> 6)), o[k++] = (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000) o[k++] = (char)(0xE0 | (c >> 12)), o[k++] = (char)(0x80 | ((c >> 6) & 0x3F)), o[k++] = (char)(0x80 | (c & 0x3F));
        else o[k++] = (char)(0xF0 | (c >> 18)), o[k++] = (char)(0x80 | ((c >> 12) & 0x3F)), o[k++] = (char)(0x80 | ((c >> 6) & 0x3F)), o[k++] = (char)(0x80 | (c & 0x3F));
    }
    o[k] = 0;
    if (n) *n = k;
    return o;
}

bool us_eq(us a, us b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, (size_t)a.n * 2) == 0); }

bool us_eq_c(us a, const char *s) {
    int32_t i = 0;
    for (; s[i]; i++)
        if (i >= a.n || a.p[i] != (uint8_t)s[i]) return false;
    return i == a.n;
}

us us_slice(us s, int32_t a, int32_t b) {
    if (a < 0) a = a + s.n < 0 ? 0 : a + s.n;
    if (b < 0) b = b + s.n < 0 ? 0 : b + s.n;
    if (a > s.n) a = s.n;
    if (b > s.n) b = s.n;
    if (b <= a) return US_EMPTY;
    return (us){s.p + a, b - a};
}

bool js_is_space(uint32_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000 || c == 0xFEFF;
}

us us_trim_end(us s) {
    while (s.n > 0 && js_is_space(s.p[s.n - 1])) s.n--;
    return s;
}

us us_trim(us s) {
    s = us_trim_end(s);
    while (s.n > 0 && js_is_space(s.p[0])) s.p++, s.n--;
    return s;
}

bool us_starts(us s, us a) { return s.n >= a.n && (a.n == 0 || memcmp(s.p, a.p, (size_t)a.n * 2) == 0); }

bool us_starts_c(us s, const char *a) {
    int32_t i = 0;
    for (; a[i]; i++)
        if (i >= s.n || s.p[i] != (uint8_t)a[i]) return false;
    return true;
}

bool us_ends_c(us s, const char *a) {
    int32_t m = (int32_t)strlen(a);
    if (m > s.n) return false;
    for (int32_t i = 0; i < m; i++)
        if (s.p[s.n - m + i] != (uint8_t)a[i]) return false;
    return true;
}

int32_t us_find_c(us s, const char *a, int32_t from) {
    int32_t m = (int32_t)strlen(a);
    for (int32_t i = from < 0 ? 0 : from; i + m <= s.n; i++) {
        int32_t j = 0;
        while (j < m && s.p[i + j] == (uint8_t)a[j]) j++;
        if (j == m) return i;
    }
    return -1;
}

int32_t us_find_ch(us s, uint16_t ch, int32_t from) {
    for (int32_t i = from < 0 ? 0 : from; i < s.n; i++)
        if (s.p[i] == ch) return i;
    return -1;
}

uint32_t us_cp_at(us s, int32_t i, int32_t *len) {
    uint32_t c = s.p[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.n && s.p[i + 1] >= 0xDC00 && s.p[i + 1] <= 0xDFFF) {
        *len = 2;
        return 0x10000 + ((c - 0xD800) << 10) + (s.p[i + 1] - 0xDC00);
    }
    *len = 1;
    return c;
}

us us_lower(us s) {
    sb b = {0};
    for (int32_t i = 0; i < s.n;) {
        int32_t l;
        uint32_t c = us_cp_at(s, i, &l);
        if (c < 0x80) sb_ch(&b, (uint16_t)(c >= 'A' && c <= 'Z' ? c + 32 : c));
        else if (c >= 0xD800 && c <= 0xDFFF) sb_ch(&b, (uint16_t)c);
        else if (c == 0x130) sb_us(&b, US("i\u0307"));  // the one unconditional full mapping in toLowerCase
        else {
            if (c == 0x3A3) UNSUPPORTED("capital sigma, lowered by context");
            sb_cp(&b, (uint32_t)utf8proc_tolower((utf8proc_int32_t)c));
        }
        i += l;
    }
    return sb_done(&b);
}

// ---- builder ----

static void sb_grow(sb *b, int32_t extra) {
    if (b->n + extra <= b->cap) return;
    int32_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < b->n + extra) cap *= 2;
    uint16_t *p = arena_alloc((size_t)cap * 2);
    if (b->n) memcpy(p, b->p, (size_t)b->n * 2);
    b->p = p, b->cap = cap;
}

void sb_ch(sb *b, uint16_t c) {
    sb_grow(b, 1);
    b->p[b->n++] = c;
}

void sb_cp(sb *b, uint32_t c) {
    if (c >= 0x10000) {
        c -= 0x10000;
        sb_ch(b, (uint16_t)(0xD800 + (c >> 10)));
        sb_ch(b, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
        sb_ch(b, (uint16_t)c);
    }
}

void sb_us(sb *b, us s) {
    if (s.n <= 0) return;
    sb_grow(b, s.n);
    memcpy(b->p + b->n, s.p, (size_t)s.n * 2);
    b->n += s.n;
}

void sb_c(sb *b, const char *s) { sb_us(b, us_c(s)); }

void sb_rep(sb *b, us s, int32_t times) {
    for (int32_t i = 0; i < times; i++) sb_us(b, s);
}

char *put_int(char *p, int64_t v, int width) {
    char t[20];
    int k = 0;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do t[k++] = (char)('0' + u % 10);
    while (u /= 10);
    while (k < width) t[k++] = '0';
    if (v < 0) *p++ = '-';
    while (k) *p++ = t[--k];
    return p;
}

void sb_pad(sb *b, int64_t v, int width) {
    char t[24];
    char *e = put_int(t, v, width);
    sb_grow(b, (int32_t)(e - t));
    for (char *q = t; q < e; q++) b->p[b->n++] = (uint8_t)*q;
}

void sb_int(sb *b, int64_t v) { sb_pad(b, v, 1); }

us us_int(int64_t v) {
    sb b = {0};
    sb_int(&b, v);
    return sb_done(&b);
}

void sb_num(sb *b, double v) {
    if (isnan(v)) return sb_us(b, US("NaN"));
    if (v == 0) return sb_ch(b, '0');
    if (isinf(v)) return sb_us(b, v < 0 ? US("-Infinity") : US("Infinity"));
    if (v < 0) sb_ch(b, '-'), v = -v;
    // Most numbers shown are integers, and below 2^53 an integer is its own shortest form.
    if (v < 9007199254740992.0 && v == trunc(v)) return sb_int(b, (int64_t)v);
    // yyjson writes the shortest round-trip digits in the layout of Number.prototype.toString,
    // except for a ".0" kept on an integer and no "+" in a positive exponent.
    yyjson_val num = {.tag = YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL, .uni.f64 = v};
    char buf[40];
    char *end = yyjson_write_number(&num, buf);
    bool exp = false;
    for (char *q = buf; q < end; q++) {
        sb_ch(b, (uint8_t)*q);
        if (*q == 'e') {
            exp = true;
            if (q[1] != '-') sb_ch(b, '+');
        }
    }
    if (!exp && end - buf >= 2 && end[-2] == '.' && end[-1] == '0') b->n -= 2;
}

// toFixed: the integer nearest x * 10^f, the larger one on a tie, taken from x's exact binary
// value m * 2^e. Up to 17 digits every step fits in 128 bits; the callers ask for 4 at most.
void sb_fixed(sb *b, double x, int f) {
    if (isnan(x)) return sb_us(b, US("NaN"));
    if (fabs(x) >= 1e21) return sb_num(b, x);
    bool neg = x < 0;
    if (neg) x = -x;
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    int ex = (int)(bits >> 52);
    unsigned __int128 m = bits & ((1ull << 52) - 1);
    if (ex) m |= 1ull << 52;
    else ex = 1;
    int e = ex - 1075;
    for (int i = 0; i < f; i++) m *= 10;
    // Below 2^-128 even the largest m * 10^f is less than half.
    unsigned __int128 n = e >= 0 ? m << e : -e >= 128 ? 0 : (m + ((unsigned __int128)1 << (-e - 1))) >> -e;
    char d[48];
    int k = 0;
    do d[k++] = (char)('0' + (int)(n % 10));
    while ((n /= 10));
    while (k <= f) d[k++] = '0';
    if (neg) sb_ch(b, '-');
    while (k > f) sb_ch(b, (uint8_t)d[--k]);
    if (f) {
        sb_ch(b, '.');
        while (k) sb_ch(b, (uint8_t)d[--k]);
    }
}

us sb_done(sb *b) { return (us){b->p, b->n}; }

us us_cat_n(int count, const us *parts) {
    sb b = {0};
    for (int i = 0; i < count; i++) sb_us(&b, parts[i]);
    return sb_done(&b);
}

// ---- JSON ----

jv jget(jv o, const char *key) {
    if (!jis_obj(o)) return nullptr;
    size_t kl = strlen(key);
    jv found = nullptr;
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(o, idx, max, k, v) {
        if (yyjson_get_len(k) == kl && memcmp(yyjson_get_str(k), key, kl) == 0) found = v;
    }
    return found;
}

bool jtruthy(jv v) {
    if (!v) return false;
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: return false;
    case YYJSON_TYPE_BOOL: return yyjson_get_bool(v);
    case YYJSON_TYPE_NUM: {
        double d = jnum(v);
        return d != 0 && !isnan(d);
    }
    case YYJSON_TYPE_STR: return yyjson_get_len(v) > 0;
    default: return true;
    }
}

double jnum(jv v) {
    if (!jis_num(v)) return NAN;
    if (yyjson_is_real(v)) return yyjson_get_real(v);
    if (yyjson_is_sint(v)) return (double)yyjson_get_sint(v);
    return (double)yyjson_get_uint(v);
}

us jstr(jv v) { return jis_str(v) ? us_utf8(yyjson_get_str(v), yyjson_get_len(v)) : US_EMPTY; }

bool jstr_eq(jv v, const char *s) {
    if (!jis_str(v)) return false;
    size_t n = strlen(s);
    return yyjson_get_len(v) == n && memcmp(yyjson_get_str(v), s, n) == 0;
}

void json_str(sb *b, us s) {
    static const char hex[] = "0123456789abcdef";
    sb_ch(b, '"');
    for (int32_t i = 0; i < s.n; i++) {
        uint16_t c = s.p[i];
        bool lone = false;
        if (c >= 0xD800 && c <= 0xDBFF) lone = !(i + 1 < s.n && s.p[i + 1] >= 0xDC00 && s.p[i + 1] <= 0xDFFF);
        else if (c >= 0xDC00 && c <= 0xDFFF) lone = !(i > 0 && s.p[i - 1] >= 0xD800 && s.p[i - 1] <= 0xDBFF);
        if (c == '"') sb_us(b, US("\\\""));
        else if (c == '\\') sb_us(b, US("\\\\"));
        else if (c == '\b') sb_us(b, US("\\b"));
        else if (c == '\f') sb_us(b, US("\\f"));
        else if (c == '\n') sb_us(b, US("\\n"));
        else if (c == '\r') sb_us(b, US("\\r"));
        else if (c == '\t') sb_us(b, US("\\t"));
        else if (c < 0x20 || lone) {
            sb_us(b, US("\\u"));
            for (int sh = 12; sh >= 0; sh -= 4) sb_ch(b, (uint16_t)hex[(c >> sh) & 15]);
        } else sb_ch(b, c);
    }
    sb_ch(b, '"');
}

static bool is_index_key(jv k) {
    const char *s = yyjson_get_str(k);
    size_t n = yyjson_get_len(k);
    if (n == 0 || n > 10) return false;
    if (n > 1 && s[0] == '0') return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

void json_stringify(sb *b, jv v) {
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: return sb_us(b, US("null"));
    case YYJSON_TYPE_BOOL: return sb_us(b, yyjson_get_bool(v) ? US("true") : US("false"));
    case YYJSON_TYPE_NUM: {
        double d = jnum(v);
        return isfinite(d) ? sb_num(b, d) : sb_us(b, US("null"));
    }
    case YYJSON_TYPE_STR: return json_str(b, jstr(v));
    case YYJSON_TYPE_ARR: {
        sb_ch(b, '[');
        size_t idx, max;
        jv x;
        yyjson_arr_foreach(v, idx, max, x) {
            if (idx) sb_ch(b, ',');
            json_stringify(b, x);
        }
        return sb_ch(b, ']');
    }
    case YYJSON_TYPE_OBJ: {
        sb_ch(b, '{');
        bool first = true;
        size_t idx, max;
        jv k, x;
        yyjson_obj_foreach(v, idx, max, k, x) {
            // JS keeps a repeated key at its first position with its last value; array-index keys
            // are reordered. Neither occurs in Claude Code's output.
            if (is_index_key(k)) UNSUPPORTED("index-like object key");
            bool seen = false;
            size_t i2, m2;
            jv k2, x2;
            yyjson_obj_foreach(v, i2, m2, k2, x2) {
                if (i2 >= idx) break;
                if (yyjson_equals(k, k2)) seen = true;
            }
            if (seen) continue;
            if (!first) sb_ch(b, ',');
            first = false;
            json_str(b, jstr(k));
            sb_ch(b, ':');
            const char *ks = yyjson_get_str(k);
            json_stringify(b, jget(v, ks));
        }
        return sb_ch(b, '}');
    }
    default: UNSUPPORTED("unexpected JSON type");
    }
}

#if defined(HUD_C_TEST)
bool number_of(const char *s, size_t n, double *v) {
    char t[64];
    if (n >= sizeof t) return false;
    memcpy(t, s, n);
    t[n] = 0;
    yyjson_val x;
    if (yyjson_read_number(t, &x, 0, nullptr, nullptr) != t + n) return false;
    *v = jnum(&x);
    return true;
}
#endif

// ---- time ----

double now_ms(void) {
#if defined(HUD_C_TEST)
    // Test build only: the comparison scripts freeze the clock at the instant they also give the
    // upstream HUD (test/compare/fakeclock.mjs), so time-dependent text matches byte for byte.
    static double fixed = -1;
    if (fixed < 0) {
        size_t n;
        char *t = us_to_utf8(env_get("HUD_FAKE_NOW_MS"), &n);
        if (!(n > 0 && number_of(t, n, &fixed))) fixed = 0;
    }
    if (fixed > 0) return fixed;
#endif
    return os_now_ms();
}

static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int dig(us s, int32_t *i, int count, bool *ok) {
    int v = 0;
    for (int k = 0; k < count; k++, (*i)++) {
        if (*i >= s.n || s.p[*i] < '0' || s.p[*i] > '9') {
            *ok = false;
            return 0;
        }
        v = v * 10 + (s.p[*i] - '0');
    }
    return v;
}

// V8 accepts more than the ES date-time format (legacy formats, local-time forms); none of those
// appear in Claude Code's data, so anything outside the UTC forms below is reported unsupported.
double parse_date(us s) {
    int32_t i = 0;
    bool ok = true;
    int y = dig(s, &i, 4, &ok), mo = 1, d = 1, h = 0, mi = 0, se = 0, ms = 0;
    if (!ok) goto bad;
    if (i < s.n && s.p[i] == '-') {
        i++;
        mo = dig(s, &i, 2, &ok);
        if (i < s.n && s.p[i] == '-') i++, d = dig(s, &i, 2, &ok);
    }
    if (!ok) goto bad;
    double off = 0;
    if (i < s.n) {
        if (s.p[i] != 'T') goto bad;
        i++;
        h = dig(s, &i, 2, &ok);
        if (i >= s.n || s.p[i] != ':') goto bad;
        i++;
        mi = dig(s, &i, 2, &ok);
        if (i < s.n && s.p[i] == ':') {
            i++;
            se = dig(s, &i, 2, &ok);
            if (i < s.n && s.p[i] == '.') i++, ms = dig(s, &i, 3, &ok);
        }
        if (!ok || i >= s.n) goto bad;
        if (s.p[i] == 'Z') i++;
        else if (s.p[i] == '+' || s.p[i] == '-') {
            int sign = s.p[i] == '-' ? -1 : 1;
            i++;
            int oh = dig(s, &i, 2, &ok);
            if (i >= s.n || s.p[i] != ':') goto bad;
            i++;
            int om = dig(s, &i, 2, &ok);
            if (!ok || oh > 23 || om > 59) goto bad;
            off = sign * (oh * 60 + om) * 60000.0;
        } else goto bad;
    }
    if (!ok || i != s.n) goto bad;
    static const int mdays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mo < 1 || mo > 12 || d < 1 || d > mdays[mo - 1] || mi > 59 || se > 59) goto bad;
    if (h > 24 || (h == 24 && (mi || se || ms))) goto bad;
    if (mo == 2 && d == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) goto bad;
    return (double)days_from_civil(y, mo, d) * 86400000.0 + h * 3600000.0 + mi * 60000.0 + se * 1000.0 + ms - off;
bad:
    UNSUPPORTED("non-ISO date string");
    return NAN;
}

double date_from_json(jv v) {
    if (jis_str(v)) return parse_date(jstr(v));
    if (jis_num(v)) {
        double d = jnum(v);
        return isfinite(d) && fabs(d) <= 8.64e15 ? trunc(d) + 0.0 : NAN;
    }
    if (v && yyjson_is_bool(v)) return yyjson_get_bool(v) ? 1 : 0;
    UNSUPPORTED("non-scalar date value");
    return NAN;
}

// ---- files ----

// The same on every platform; only the separator and path_resolve differ.
us path_join_n(int count, const us *parts) {
    sb b = {0};
    for (int i = 0; i < count; i++) {
        if (i && b.n && !is_sep(b.p[b.n - 1])) sb_ch(&b, OS_SEP);
        sb_us(&b, parts[i]);
    }
    return path_resolve(sb_done(&b));
}

char *read_file(us path, size_t *n) {
    void *h = open_read(path);
    if (!h) return nullptr;
    char *p = read_rest(h, 0, n);
    os_close(h);
    return p;
}

bool write_file(us path, us body, double mtime, int mode) {
    size_t n;
    char *u8 = us_to_utf8(body, &n);
    return write_bytes(path, u8, n, mtime, mode);
}

// ---- JS helpers ----

static bool display_drops(uint16_t c) {  // control and bidi characters
    return c <= 0x1F || (c >= 0x7F && c <= 0x9F) || c == 0x061C || c == 0x200E || c == 0x200F ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x206F);
}

// The first three of sanitizeDisplayText's global replaces, each over the previous result: CSI,
// OSC, ESC Fe.
static us strip_escapes(us s) {
    sb b = {0};
    for (int32_t i = 0; i < s.n;) {
        if (s.p[i] == 0x1B && i + 1 < s.n && s.p[i + 1] == '[') {
            int32_t j = i + 2;
            while (j < s.n && s.p[j] >= 0x30 && s.p[j] <= 0x3F) j++;
            while (j < s.n && s.p[j] >= 0x20 && s.p[j] <= 0x2F) j++;
            if (j < s.n && s.p[j] >= 0x40 && s.p[j] <= 0x7E) {
                i = j + 1;
                continue;
            }
        }
        sb_ch(&b, s.p[i++]);
    }
    s = sb_done(&b);
    b = (sb){0};
    for (int32_t i = 0; i < s.n;) {
        if (s.p[i] == 0x1B && i + 1 < s.n && s.p[i + 1] == ']') {
            int32_t j = i + 2;
            while (j < s.n && s.p[j] != 0x07 && s.p[j] != 0x1B) j++;
            if (j < s.n && s.p[j] == 0x07) {
                i = j + 1;
                continue;
            }
            if (j + 1 < s.n && s.p[j] == 0x1B && s.p[j + 1] == '\\') {
                i = j + 2;
                continue;
            }
        }
        sb_ch(&b, s.p[i++]);
    }
    s = sb_done(&b);
    b = (sb){0};
    for (int32_t i = 0; i < s.n;) {
        if (s.p[i] == 0x1B && i + 1 < s.n && ((s.p[i + 1] >= 0x40 && s.p[i + 1] <= 0x5A) || (s.p[i + 1] >= 0x5C && s.p[i + 1] <= 0x5F))) {
            i += 2;
            continue;
        }
        sb_ch(&b, s.p[i++]);
    }
    return sb_done(&b);
}

us sanitize_display(us s) {
    // Plain text, the usual case, comes back as it is: every replace needs a character the last
    // one drops, and the escape replaces need ESC, which is one of those.
    int32_t first = 0;
    while (first < s.n && !display_drops(s.p[first])) first++;
    if (first == s.n) return s;
    for (int32_t i = first; i < s.n; i++)
        if (s.p[i] == 0x1B) {
            s = strip_escapes(s);
            break;
        }
    sb b = {0};
    for (int32_t i = 0; i < s.n; i++)
        if (!display_drops(s.p[i])) sb_ch(&b, s.p[i]);
    return sb_done(&b);
}

void js_string_of(sb *b, jv v) {
    if (!v) return sb_us(b, US("undefined"));
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: return sb_us(b, US("null"));
    case YYJSON_TYPE_BOOL: return sb_us(b, yyjson_get_bool(v) ? US("true") : US("false"));
    case YYJSON_TYPE_NUM: return sb_num(b, jnum(v));
    case YYJSON_TYPE_STR: return sb_us(b, jstr(v));
    case YYJSON_TYPE_ARR: {
        size_t idx, max;
        jv x;
        yyjson_arr_foreach(v, idx, max, x) {
            if (idx) sb_ch(b, ',');
            if (!jis_nullish(x)) js_string_of(b, x);
        }
        return;
    }
    default: return sb_us(b, US("[object Object]"));
    }
}

static bool hex4(const char *s, unsigned *v) {
    *v = 0;
    for (int j = 0; j < 4; j++) {
        char c = s[j];
        *v <<= 4;
        if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0');
        else if ((c | 32) >= 'a' && (c | 32) <= 'f') *v |= (unsigned)((c | 32) - 'a' + 10);
        else return false;
    }
    return true;
}

// yyjson rejects an escaped lone surrogate, JSON.parse keeps it. Rewrite such escapes as the
// three-byte (WTF-8) form, which us_utf8() turns back into the lone code unit.
char *json_fix_surrogates(const char *p, size_t n, size_t *outn) {
    char *o = xmalloc(n + YYJSON_PADDING_SIZE + 1);
    size_t k = 0;
    bool changed = false;
    for (size_t i = 0; i < n;) {
        if (p[i] == '\\' && i + 1 < n) {
            if (p[i + 1] == 'u' && i + 5 < n) {
                unsigned v;
                if (hex4(p + i + 2, &v) && v >= 0xD800 && v <= 0xDFFF) {
                    bool pair = false;
                    if (v <= 0xDBFF && i + 11 < n && p[i + 6] == '\\' && p[i + 7] == 'u') {
                        unsigned w;
                        pair = hex4(p + i + 8, &w) && w >= 0xDC00 && w <= 0xDFFF;
                    }
                    if (pair) {
                        memcpy(o + k, p + i, 12);
                        k += 12, i += 12;
                        continue;
                    }
                    o[k++] = (char)(0xE0 | (v >> 12));
                    o[k++] = (char)(0x80 | ((v >> 6) & 0x3F));
                    o[k++] = (char)(0x80 | (v & 0x3F));
                    i += 6;
                    changed = true;
                    continue;
                }
            }
            o[k++] = p[i++];
            o[k++] = p[i++];
            continue;
        }
        o[k++] = p[i++];
    }
    if (!changed) {
        free(o);
        return nullptr;
    }
    memset(o + k, 0, YYJSON_PADDING_SIZE + 1);
    *outn = k;
    return o;
}

jv parse_json(const char *p, size_t n) {
    static const yyjson_read_flag flg = YYJSON_READ_ALLOW_INVALID_UNICODE;
    yyjson_doc *d = yyjson_read_opts((char *)p, n, flg, nullptr, nullptr);
    if (!d) {
        size_t fn;
        char *fixed = json_fix_surrogates(p, n, &fn);
        if (fixed) d = yyjson_read_opts(fixed, fn, flg, nullptr, nullptr);
    }
    return d ? yyjson_doc_get_root(d) : nullptr;
}

jv read_json_file(us path) {
    size_t n;
    char *p = read_file(path, &n);
    return p ? parse_json(p, n) : nullptr;
}

static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

us iso_string(double ms) {
    if (!isfinite(ms) || fabs(ms) > 8.64e15) {
        UNSUPPORTED("toISOString of an invalid date");
        return US_EMPTY;
    }
    int64_t t = (int64_t)ms;
    int64_t days = t >= 0 ? t / 86400000 : -((-t + 86399999) / 86400000);
    int64_t rem = t - days * 86400000;
    int64_t y;
    int mo, d;
    civil_from_days(days, &y, &mo, &d);
    sb b = {0};
    if (y >= 0 && y <= 9999) sb_pad(&b, y, 4);
    else sb_ch(&b, y < 0 ? '-' : '+'), sb_pad(&b, y < 0 ? -y : y, 6);
    sb_ch(&b, '-'), sb_pad(&b, mo, 2);
    sb_ch(&b, '-'), sb_pad(&b, d, 2);
    sb_ch(&b, 'T'), sb_pad(&b, rem / 3600000, 2);
    sb_ch(&b, ':'), sb_pad(&b, rem / 60000 % 60, 2);
    sb_ch(&b, ':'), sb_pad(&b, rem / 1000 % 60, 2);
    sb_ch(&b, '.'), sb_pad(&b, rem % 1000, 3);
    sb_ch(&b, 'Z');
    return sb_done(&b);
}
