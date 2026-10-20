#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "yyjson.h"

// The compiler's own classification, whatever math.h says: older Windows SDKs expand these to
// calls into the C runtime (_dclass and its kin), which a build that links no C runtime cannot resolve.
#undef isnan
#undef isinf
#undef isfinite
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)

// Anything this port does not reproduce exactly sets this. The release build renders on
// regardless; the test build reports it instead of output, the trace build logs it.
extern const char *g_unsupported;
#define UNSUPPORTED(why) (g_unsupported ? (void)0 : (void)(g_unsupported = (why)))

// To stderr as it is. Lines end in \r\n, which is what the CRT's text-mode stderr wrote before
// the CRT was dropped and what the test scripts' references hold.
void write_err(const char *s);
#if defined(HUD_C_TEST)
bool number_of(const char *s, size_t n, double *v);  // s whole as a JSON number, the form the test scripts pass
#endif

// malloc and realloc that end the process, loudly, when memory runs out. Never null, which lets
// the compiler drop the checks behind them.
[[gnu::returns_nonnull]] void *xmalloc(size_t n);
[[gnu::returns_nonnull]] void *xrealloc(void *p, size_t n);
[[gnu::returns_nonnull]] void *arena_alloc(size_t n);
void *arena_dup(const void *p, size_t n);

// A JS string: UTF-16 code units, possibly holding lone surrogates.
typedef struct { const uint16_t *p; int32_t n; } us;
typedef struct { uint16_t *p; int32_t n, cap; } sb;

#define US_EMPTY ((us){nullptr, 0})
// A string literal as a us, encoded by the compiler: nothing is decoded at run time and the
// length comes from sizeof. Literals only; write a non-ASCII character as itself or as \u,
// never as UTF-8 \x bytes, which would become one code unit each. US_INIT is the same for a
// static initializer.
#define US(lit) ((us)US_INIT(lit))
#define US_INIT(lit) {(const uint16_t *)u"" lit, (int32_t)(sizeof(u"" lit) / 2 - 1)}
#define countof(a) (sizeof(a) / sizeof *(a))
us us_utf8(const char *s, size_t n);   // WTF-8 tolerant; invalid bytes become U+FFFD
int32_t us_utf8_len(const char *s, size_t n);  // the length us_utf8 would give, without the copy
us us_c(const char *s);                // NUL-terminated UTF-8 built at run time
char *us_to_utf8(us s, size_t *n);     // lone surrogates become U+FFFD, like node's stdout
bool us_eq(us a, us b);
bool us_eq_c(us a, const char *ascii);
us us_slice(us s, int32_t a, int32_t b);  // String.prototype.slice
us us_trim(us s);
us us_trim_end(us s);
bool us_starts(us s, us prefix);
bool us_starts_c(us s, const char *ascii);
bool us_ends_c(us s, const char *ascii);
int32_t us_find_c(us s, const char *ascii, int32_t from);
int32_t us_find_ch(us s, uint16_t ch, int32_t from);
us us_lower(us s);
bool js_is_space(uint32_t c);
uint32_t us_cp_at(us s, int32_t i, int32_t *len);  // code point at i (lone surrogate as itself)

void sb_ch(sb *b, uint16_t c);
void sb_cp(sb *b, uint32_t cp);
void sb_us(sb *b, us s);
void sb_c(sb *b, const char *utf8);
void sb_rep(sb *b, us s, int32_t times);
char *put_int(char *p, int64_t v, int width);  // decimal, zero-padded to width digits; returns the end
void sb_pad(sb *b, int64_t v, int width);     // the same into a builder
void sb_int(sb *b, int64_t v);
us us_int(int64_t v);
void sb_num(sb *b, double v);           // Number.prototype.toString()
void sb_fixed(sb *b, double v, int f);  // Number.prototype.toFixed(f)
us sb_done(sb *b);
// The parts go in as an array rather than through "...": clang on Windows ARM64 passes a 16-byte struct
// that no longer fits in x0-x7 wholly on the stack, while va_arg reads it split between x7 and the stack
// as Microsoft's convention has it, so every part from the one that straddles on came out wrong.
us us_cat_n(int count, const us *parts);
#define us_cat(...) us_cat_n(countof(((us[]){__VA_ARGS__})), (us[]){__VA_ARGS__})

// JSON access with JS semantics. A null jv means "undefined"; JSON null is a yyjson null value.
typedef yyjson_val *jv;
jv jget(jv obj, const char *key);       // duplicate keys: last one wins, as in JSON.parse
bool jtruthy(jv v);
static inline bool jis_str(jv v) { return v && yyjson_is_str(v); }
static inline bool jis_num(jv v) { return v && yyjson_is_num(v); }
static inline bool jis_obj(jv v) { return v && yyjson_is_obj(v); }
static inline bool jis_arr(jv v) { return v && yyjson_is_arr(v); }
static inline bool jis_nullish(jv v) { return !v || yyjson_is_null(v); }
static inline bool jis_true(jv v) { return v && yyjson_is_true(v); }
double jnum(jv v);
static inline bool is_finite_num(jv v) { return jis_num(v) && __builtin_isfinite(jnum(v)); }
us jstr(jv v);
bool jstr_eq(jv v, const char *ascii);
void json_stringify(sb *b, jv v);       // JSON.stringify for parsed values
void json_str(sb *b, us s);             // JSON.stringify for a string
// Rewrites escaped lone surrogates as WTF-8 bytes, which yyjson accepts and JSON.parse keeps;
// returns a malloc'd padded copy, or null when the text has none.
char *json_fix_surrogates(const char *p, size_t n, size_t *outn);
jv parse_json(const char *p, size_t n);  // JSON.parse; the document lives until exit
jv read_json_file(us path);              // JSON.parse(fs.readFileSync(path, 'utf8')), null on failure

// Time. Milliseconds since the epoch as a double, NaN for an invalid date.
double now_ms(void);
typedef struct { int y, mo, d, h, mi, s; } ltime;
double parse_date(us s);  // ISO 8601 as V8 reads it; anything else is unsupported
double date_from_json(jv v);  // new Date(value)
us iso_string(double ms);     // Date.prototype.toISOString

us sanitize_display(us s);    // utils/sanitize.js sanitizeDisplayText
void js_string_of(sb *b, jv v);  // String(value)

// Files, on top of the platform layer's write_bytes and handles.
char *read_file(us path, size_t *n);  // the whole file, padded for yyjson; null on failure
bool write_file(us path, us body, double mtime, int mode);  // UTF-8, as write_bytes

// The platform layer: system calls and path rules.
#include "os.h"