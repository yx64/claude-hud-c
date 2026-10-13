// Port of claude-hud dist/render/** and the stdin.js, i18n and utils helpers it uses.
#include <math.h>
#include <string.h>

#include "extpict.h"
#include "hud.h"
#include "utf8proc.h"

// ---- strings (i18n/en.js); one column per language ----

enum {
    T_CONTEXT, T_USAGE, T_WEEKLY, T_APPROX_RAM, T_PROMPT_CACHE, T_CACHE_HIT_RATE, T_RULES, T_HOOKS, T_COST, T_TODAY,
    T_WEEK, T_TOKENS, T_SESSION_STARTED, T_LAST_REPLY, T_ADVISOR, T_COMPACTIONS, T_LIMIT_REACHED, T_ALL_TODOS, T_EXPIRED,
    T_RESETS, T_RESETS_IN, T_ABS_TIME, T_UNTIL_TIME, T_IN, T_CACHE, T_OUT, T_TOK, T_TOK_PER_SEC, T_JUST_NOW,
    T_RELATIVE_TIME, T_ELAPSED, T_COUNT
};

// A column per language, in enum order; the assert catches a key added to the enum only.
static const us EN[] = {
    US_INIT("Context"), US_INIT("Usage"), US_INIT("Weekly"), US_INIT("Approx RAM"), US_INIT("Cache"), US_INIT("Cache hit"),
    US_INIT("rules"), US_INIT("hooks"), US_INIT("Cost"), US_INIT("Today"), US_INIT("Week"), US_INIT("Tokens"), US_INIT("Started"),
    US_INIT("Last reply"), US_INIT("Advisor"), US_INIT("Compactions"), US_INIT("Limit reached"), US_INIT("All todos complete"),
    US_INIT("expired"), US_INIT("resets"), US_INIT("resets in"), US_INIT("at {time}"), US_INIT("until {time}"), US_INIT("in"),
    US_INIT("cache"), US_INIT("out"), US_INIT("tok"), US_INIT("tok/s"), US_INIT("just now"), US_INIT("{value} ago"),
    US_INIT("{value}% elapsed"),
};
static_assert(countof(EN) == T_COUNT);
static const us *const STRINGS[] = {[LANG_EN] = EN};

static Ctx *C;  // the render pass is single-threaded and runs once
static Config *CF;
static us t(int key) { return STRINGS[CF->lang][key]; }

// interpolate(): {name} placeholders; unknown ones render empty
static us interpolate(us pattern, const char *name, us value) {
    sb b = {0};
    for (int32_t i = 0; i < pattern.n; i++) {
        if (pattern.p[i] == '{') {
            int32_t j = us_find_ch(pattern, '}', i);
            if (j > i + 1) {
                us key = us_slice(pattern, i + 1, j);
                bool word = true;
                for (int32_t k = 0; k < key.n; k++) {
                    uint16_t ch = key.p[k];
                    word &= (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
                }
                if (word) {
                    if (us_eq_c(key, name)) sb_us(&b, value);
                    i = j;
                    continue;
                }
            }
        }
        sb_ch(&b, pattern.p[i]);
    }
    return sb_done(&b);
}

double js_round(double x) {
    if (!isfinite(x)) return x;
    double r = floor(x);
    return x - r >= 0.5 ? r + 1 : r;
}

double to_percent(double v) { return fmin(100, fmax(0, js_round(v))); }

static us num(double v) {
    sb b = {0};
    sb_num(&b, v);
    return sb_done(&b);
}

static us fixed(double v, int f) {
    sb b = {0};
    sb_fixed(&b, v, f);
    return sb_done(&b);
}

#define CAT(...) us_cat(__VA_ARGS__)

static us join(us *parts, int n, us sep) {
    sb b = {0};
    for (int i = 0; i < n; i++) {
        if (i) sb_us(&b, sep);
        sb_us(&b, parts[i]);
    }
    return sb_done(&b);
}

static us repeat(us s, double times) {
    sb b = {0};
    sb_rep(&b, s, (int32_t)times);
    return sb_done(&b);
}

static jv sget(const char *a, const char *b) {
    jv v = jget(C->input, a);
    return b ? jget(v, b) : v;
}

// ---- colors.js ----

static const us RESET = US_INIT("\x1b[0m"), DIM = US_INIT("\x1b[2m");

static us colorize(us text, us color) { return CAT(color, text, RESET); }
static us green(us x) { return colorize(x, US("\x1b[32m")); }
static us yellow(us x) { return colorize(x, US("\x1b[33m")); }
static us red(us x) { return colorize(x, US("\x1b[31m")); }
static us cyan(us x) { return colorize(x, US("\x1b[36m")); }
static us magenta(us x) { return colorize(x, US("\x1b[35m")); }
static us dim(us x) { return colorize(x, DIM); }
static us label(us x) { return colorize(x, CF->c.label); }
static us c_model(us x) { return colorize(x, CF->c.model); }
static us c_project(us x) { return colorize(x, CF->c.project); }
static us c_git(us x) { return colorize(x, CF->c.git); }
static us c_branch(us x) { return colorize(x, CF->c.git_branch); }
static us c_custom(us x) { return colorize(x, CF->c.custom); }
static us c_warning(us x) { return colorize(x, CF->c.warning); }
static us c_critical(us x) { return colorize(x, CF->c.critical); }

static us context_color(double pct, double warn, double crit) {
    if (pct >= crit) return CF->c.critical;
    if (pct >= warn) return CF->c.warning;
    return CF->c.context;
}

enum { PACE_NONE, PACE_NORMAL, PACE_WARNING, PACE_CRITICAL };
static bool pace_alert(int pace) { return pace == PACE_WARNING || pace == PACE_CRITICAL; }

static us quota_color(double pct, int pace) {
    if (pct >= 90 || pace == PACE_CRITICAL) return CF->c.critical;
    if (pct >= 75 || pace == PACE_WARNING) return CF->c.usage_warning;
    return CF->c.usage;
}

static us quota_percent(double pct, int pace) {
    if (isnan(pct)) return label(US("--"));
    double shown = CF->d.usage_remaining ? fmax(0, 100 - pct) : pct;
    us marker = pace_alert(pace) ? CAT(US(" "), colorize(US("▲"), quota_color(0, pace))) : US_EMPTY;
    return CAT(quota_color(pct, pace), num(shown), US("%"), RESET, marker);
}

static us bar_of(double pct, double width, us color) {
    double w = isfinite(width) ? fmax(0, js_round(width)) : 0;
    double filled = js_round(pct / 100 * w);
    return CAT(color, repeat(CF->c.bar_filled, filled), DIM, repeat(CF->c.bar_empty, w - filled), RESET);
}

static double safe_pct(double p) { return isfinite(p) ? fmin(100, fmax(0, p)) : 0; }
static us quota_bar(double pct, int pace) { return bar_of(safe_pct(pct), C->bar_width, quota_color(safe_pct(pct), pace)); }
static us colored_bar(double pct) {
    return bar_of(safe_pct(pct), C->bar_width, context_color(safe_pct(pct), CF->d.ctx_warn, CF->d.ctx_crit));
}

// ---- ansi.js: width, slicing and wrapping ----

static bool cjk_ambiguous_wide(void) { return false; }  // only CJK UI languages turn this on

static bool is_wide(uint32_t c) {
    return c >= 0x1100 && (c <= 0x115F || c == 0x2329 || c == 0x232A || (c >= 0x2E80 && c <= 0xA4CF && c != 0x303F) ||
                           (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE10 && c <= 0xFE19) ||
                           (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) ||
                           (c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x20000 && c <= 0x3FFFD));
}

static bool is_ambiguous(uint32_t c) {
    return (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) || (c >= 0x2190 && c <= 0x23FF) ||
           (c >= 0x2460 && c <= 0x24FF) || (c >= 0x2500 && c <= 0x27BF);
}

static bool is_mark(uint32_t c) {
    utf8proc_category_t k = utf8proc_category((utf8proc_int32_t)c);
    return k == UTF8PROC_CATEGORY_MN || k == UTF8PROC_CATEGORY_MC || k == UTF8PROC_CATEGORY_ME;
}

static int32_t grapheme_end(us s, int32_t i) {
    int32_t l;
    utf8proc_int32_t state = 0;
    uint32_t c1 = us_cp_at(s, i, &l);
    int32_t j = i + l;
    while (j < s.n) {
        int32_t l2;
        uint32_t c2 = us_cp_at(s, j, &l2);
        if (utf8proc_grapheme_break_stateful((utf8proc_int32_t)c1, (utf8proc_int32_t)c2, &state)) break;
        c1 = c2, j += l2;
    }
    return j;
}

static int grapheme_width(us g, bool amb) {
    int32_t l;
    uint32_t first = us_cp_at(g, 0, &l);
    if (l == g.n && utf8proc_category((utf8proc_int32_t)first) == UTF8PROC_CATEGORY_CC) return 0;
    for (int32_t i = 0; i < g.n; i += l)
        if (is_extpict(us_cp_at(g, i, &l))) return 2;
    int w = 0;
    for (int32_t i = 0; i < g.n; i += l) {
        uint32_t c = us_cp_at(g, i, &l);
        if (is_mark(c) || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF)) continue;
        int cw = is_wide(c) || (amb && is_ambiguous(c)) ? 2 : 1;
        if (cw > w) w = cw;
    }
    return w;
}

// The grapheme at i: where it ends, and its width in *w.
static int32_t grapheme_at(us s, int32_t i, bool amb, int *w) {
    // Printable ASCII followed by anything below U+0300 stands alone, one column wide: no
    // character that extends or joins lies below that, and none of ASCII is pictographic.
    uint16_t c = s.p[i];
    if (c >= 0x20 && c < 0x7F && (i + 1 == s.n || s.p[i + 1] < 0x300)) {
        *w = 1;
        return i + 1;
    }
    int32_t e = grapheme_end(s, i);
    *w = grapheme_width(us_slice(s, i, e), amb);
    return e;
}

static int text_width(us s) {
    bool amb = cjk_ambiguous_wide();
    int w = 0;
    for (int32_t g = 0; g < s.n;) {
        int gw;
        g = grapheme_at(s, g, amb, &gw);
        w += gw;
    }
    return w;
}

// ESCAPE_AT: a CSI sequence, or an OSC sequence ended by BEL or ESC \. Its length at i, else 0.
static int32_t escape_len(us s, int32_t i) {
    if (i + 1 >= s.n || s.p[i] != 0x1B) return 0;
    int32_t j = i + 2;
    if (s.p[i + 1] == '[') {
        while (j < s.n && s.p[j] >= 0x30 && s.p[j] <= 0x3F) j++;
        while (j < s.n && s.p[j] >= 0x20 && s.p[j] <= 0x2F) j++;
        return j < s.n && s.p[j] >= 0x40 && s.p[j] <= 0x7E ? j + 1 - i : 0;
    }
    if (s.p[i + 1] == ']') {
        while (j < s.n && s.p[j] != 0x07 && s.p[j] != 0x1B) j++;
        if (j < s.n && s.p[j] == 0x07) return j + 1 - i;
        if (j + 1 < s.n && s.p[j] == 0x1B && s.p[j + 1] == '\\') return j + 2 - i;
    }
    return 0;
}

static int32_t text_end(us s, int32_t i) {
    int32_t j = i;
    while (j < s.n && !escape_len(s, j)) j++;
    return j;
}

static int visible_width(us s) {
    int w = 0;
    for (int32_t i = 0; i < s.n;) {
        int32_t a = escape_len(s, i);
        if (a) {
            i += a;
            continue;
        }
        int32_t e = text_end(s, i);
        w += text_width(us_slice(s, i, e));
        i = e;
    }
    return w;
}

static us strip_ansi(us s) {
    sb b = {0};
    for (int32_t i = 0; i < s.n;) {
        int32_t a = escape_len(s, i);
        if (a) {
            i += a;
            continue;
        }
        int32_t e = text_end(s, i);
        sb_us(&b, us_slice(s, i, e));
        i = e;
    }
    return sb_done(&b);
}

static us slice_to_width(us s, int max) {
    if (max <= 0) return US_EMPTY;
    bool amb = cjk_ambiguous_wide();
    sb b = {0};
    int used = 0;
    for (int32_t i = 0; i < s.n;) {
        int32_t a = escape_len(s, i);
        if (a) {
            sb_us(&b, us_slice(s, i, i + a));
            i += a;
            continue;
        }
        us run = us_slice(s, i, text_end(s, i));
        for (int32_t g = 0; g < run.n;) {
            int gw;
            int32_t e = grapheme_at(run, g, amb, &gw);
            used += gw;
            if (used > max) return sb_done(&b);
            sb_us(&b, us_slice(run, g, e));
            g = e;
        }
        i += run.n;
    }
    return sb_done(&b);
}

// OSC8 matched anywhere: the close sequence when the last one opened a link
static us close_open_hyperlink(us s) {
    bool open = false;
    for (int32_t i = 0; i + 4 < s.n; i++) {
        if (!(s.p[i] == 0x1B && s.p[i + 1] == ']' && s.p[i + 2] == '8' && s.p[i + 3] == ';' && s.p[i + 4] == ';')) continue;
        int32_t j = i + 5;
        while (j < s.n && s.p[j] != 0x07 && s.p[j] != 0x1B) j++;
        int32_t end = 0;
        if (j < s.n && s.p[j] == 0x07) end = j + 1;
        else if (j + 1 < s.n && s.p[j] == 0x1B && s.p[j + 1] == '\\') end = j + 2;
        if (!end) continue;
        open = j > i + 5;
        i = end - 1;
    }
    return open ? US("\x1b]8;;\x1b\\") : US_EMPTY;
}

static us truncate_to_width(us s, int width) {
    if (width <= 0 || visible_width(s) <= width) return s;
    us suffix = width >= 3 ? US("...") : us_slice(US("..."), 0, width);
    us kept = slice_to_width(s, width - suffix.n);
    return CAT(kept, close_open_hyperlink(kept), suffix, RESET);
}

typedef struct { us sep, text; } Part;

// splitAtSeparators: " | " and " │ " inside text, never inside a link
static int split_at_separators(us line, Part *parts) {
    int n = 0;
    int32_t start = 0;
    us sep = US_EMPTY;
    bool in_link = false;
    for (int32_t i = 0; i < line.n;) {
        int32_t a = escape_len(line, i);
        if (a) {
            if (a >= 5 && line.p[i + 1] == ']' && line.p[i + 2] == '8' && line.p[i + 3] == ';' && line.p[i + 4] == ';') {
                us tok = us_slice(line, i, i + a);
                int32_t u = 5;
                while (u < tok.n && tok.p[u] != 0x07 && tok.p[u] != 0x1B) u++;
                in_link = u > 5;
            }
            i += a;
            continue;
        }
        int32_t e = text_end(line, i);
        if (!in_link) {
            for (int32_t k = i; k + 3 <= e;) {
                bool pipe = line.p[k] == ' ' && line.p[k + 1] == '|' && line.p[k + 2] == ' ';
                bool box = line.p[k] == ' ' && line.p[k + 1] == 0x2502 && line.p[k + 2] == ' ';
                if (pipe || box) {
                    parts[n++] = (Part){sep, us_slice(line, start, k)};
                    sep = pipe ? US(" | ") : US(" │ ");
                    k += 3;
                    start = k;
                } else k++;
            }
        }
        i = e;
    }
    parts[n++] = (Part){sep, us_slice(line, start, line.n)};
    // A leading "[model | provider]" badge contains a separator but must not wrap.
    us first = strip_ansi(parts[0].text);
    us lead = first;
    while (lead.n && js_is_space(lead.p[0])) lead.p++, lead.n--;
    if (us_starts_c(lead, "[") && us_find_ch(first, ']', 0) < 0) {
        int end = 1;
        while (end < n && us_find_ch(strip_ansi(parts[end - 1].text), ']', 0) < 0) end++;
        sb b = {0};
        for (int k = 0; k < end; k++) sb_us(&b, parts[k].sep), sb_us(&b, parts[k].text);
        parts[0] = (Part){US_EMPTY, sb_done(&b)};
        memmove(parts + 1, parts + end, sizeof(Part) * (size_t)(n - end));
        n = n - end + 1;
    }
    return n;
}

static void wrap_to_width(us line, int width, us *out, int *nout) {
    if (width <= 0 || visible_width(line) <= width) {
        out[(*nout)++] = line;
        return;
    }
    Part *parts = arena_alloc(sizeof(Part) * (size_t)(line.n + 2));
    int n = split_at_separators(line, parts);
    if (n <= 1) {
        out[(*nout)++] = truncate_to_width(line, width);
        return;
    }
    us cur = parts[0].text;
    for (int k = 1; k < n; k++) {
        us cand = CAT(cur, parts[k].sep, parts[k].text);
        if (visible_width(cand) <= width) {
            cur = cand;
            continue;
        }
        out[(*nout)++] = truncate_to_width(cur, width);
        cur = parts[k].text;
    }
    if (cur.n) out[(*nout)++] = truncate_to_width(cur, width);
}

static us separator_line(int width) {
    int reps = width / (cjk_ambiguous_wide() ? 2 : 1);
    return dim(repeat(US("─"), reps > 1 ? reps : 1));
}

// ---- labels.js ----

typedef struct { bool align, memory; } Align;

static us bar_label(int key, Align a) {
    us text = t(key);
    if (!a.align) return label(text);
    int max = 0;
    static const int keys[] = {T_CONTEXT, T_USAGE, T_WEEKLY, T_APPROX_RAM};  // the memory label last
    for (size_t i = 0; i < countof(keys) - !a.memory; i++) {
        int w = text_width(t(keys[i]));
        if (w > max) max = w;
    }
    int pad = max - text_width(text);
    return label(pad > 0 ? CAT(text, repeat(US(" "), pad)) : text);
}

// ---- utils/format.js, stdin.js ----

static us format_tokens(double n) {
    if (n >= 1000000) return CAT(fixed(n / 1000000, 1), US("M"));
    if (n >= 1000) return CAT(fixed(n / 1000, 0), US("k"));
    return num(n);
}

static double usage_field(const char *name) {  // context_window.current_usage[name] ?? 0
    jv f = jget(sget("context_window", "current_usage"), name);
    if (jis_nullish(f)) return 0;
    if (!jis_num(f)) {
        UNSUPPORTED("non-number token count");
        return 0;
    }
    return jnum(f);
}

static double total_tokens(void) {
    return usage_field("input_tokens") + usage_field("cache_creation_input_tokens") + usage_field("cache_read_input_tokens");
}

typedef struct { double percent, tokens, size; } ContextUse;

// getContextUsage: the native percentage when Claude Code reports one, else from token counts
static ContextUse context_usage(void) {
    double live = total_tokens();
    Transcript *tr = C->t;
    double tokens = live > 0 ? live : tr->context_tokens;
    double acw = CF->d.auto_compact_window;
    if (acw > 0) return (ContextUse){to_percent(tokens / acw * 100), tokens, acw};
    jv sz = sget("context_window", "context_window_size");
    double size = 0;
    if (!jis_nullish(sz)) {
        if (jis_num(sz)) size = jnum(sz);
        else UNSUPPORTED("non-number context window size");
    }
    jv native = sget("context_window", "used_percentage");
    if (is_finite_num(native) && jnum(native) > 0) return (ContextUse){to_percent(jnum(native)), tokens, size};
    return (ContextUse){size > 0 ? to_percent(tokens / size * 100) : 0, tokens, size};
}

static us format_context_value(ContextUse u, int mode) {
    us ratio = u.size > 0 ? CAT(format_tokens(u.tokens), US("/"), format_tokens(u.size)) : format_tokens(u.tokens);
    if (mode == CV_TOKENS) return ratio;
    if (mode == CV_BOTH) return u.size > 0 ? CAT(num(u.percent), US("% ("), ratio, US(")")) : CAT(num(u.percent), US("%"));
    if (mode == CV_REMAINING) return CAT(num(fmax(0, 100 - u.percent)), US("%"));
    return CAT(num(u.percent), US("%"));
}

// stdinText: sanitized, trimmed, bounded; false for undefined
static bool stdin_text(jv v, int max, us *out) {
    if (!jis_str(v)) return false;
    us s = us_slice(us_trim(sanitize_display(jstr(v))), 0, max);
    if (!s.n) return false;
    *out = s;
    return true;
}

// optional?.trim() on a value that must be a string when present
static bool opt_trim(jv v, us *out) {
    if (jis_nullish(v)) return false;
    if (!jis_str(v)) {
        UNSUPPORTED("non-string where upstream calls trim()");
        return false;
    }
    *out = us_trim(jstr(v));
    return true;
}

bool is_bedrock_id(jv id) { return jis_str(id) && us_find_c(us_lower(jstr(id)), "anthropic.claude-", 0) >= 0; }
bool is_vertex_id(jv id) { return jis_str(id) && us_find_ch(jstr(id), '@', 0) >= 0; }

static us model_name(void) {
    us name;
    if (opt_trim(sget("model", "display_name"), &name) && name.n) return name;
    if (opt_trim(sget("model", "id"), &name) && name.n) return name;
    return US("Unknown");
}

static us resolve_model_name(void) {
    us stdin_model = model_name();
    Transcript *tr = C->t;
    if (CF->d.model_source == MS_STDIN || !tr->last_model.n) return stdin_model;
    if (CF->d.model_source == MS_TRANSCRIPT) return tr->last_model;
    us low = us_lower(tr->last_model);
    return us_starts_c(low, "claude-") || us_starts_c(low, "anthropic.") ? stdin_model : tr->last_model;
}

static bool word_ch(uint16_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// /\s*\([^)]*\bcontext\b[^)]*\)/i, first match removed, then trim
static us strip_context_suffix(us name) {
    for (int32_t q = 0; q < name.n; q++) {
        if (name.p[q] != '(') continue;
        int32_t close = us_find_ch(name, ')', q + 1);
        if (close < 0) break;
        us inner = us_slice(name, q + 1, close);
        bool hit = false;
        for (int32_t i = 0; i + 7 <= inner.n && !hit; i++) {
            bool m = true;
            for (int k = 0; k < 7 && m; k++) m = (inner.p[i + k] | 32) == (uint16_t)"context"[k];
            if (!m) continue;
            bool before = i == 0 ? !word_ch(name.p[q]) : !word_ch(inner.p[i - 1]);
            bool after = i + 7 == inner.n ? true : !word_ch(inner.p[i + 7]);
            hit = before && after;
        }
        if (!hit) continue;
        int32_t start = q;
        while (start > 0 && js_is_space(name.p[start - 1])) start--;
        return us_trim(CAT(us_slice(name, 0, start), us_slice(name, close + 1, name.n)));
    }
    return us_trim(name);
}

static us format_model_name(us name) {
    if (CF->d.model_override.n) return CF->d.model_override;
    if (CF->d.model_format == MF_FULL) return name;
    us r = strip_context_suffix(name);
    if (CF->d.model_format == MF_SHORT && r.n >= 6) {  // .replace(/^Claude\s+/i, '')
        bool m = true;
        for (int k = 0; k < 6 && m; k++) m = (r.p[k] | 32) == (uint16_t)"claude"[k];
        int32_t j = 6;
        if (m && j < r.n && js_is_space(r.p[j])) {
            while (j < r.n && js_is_space(r.p[j])) j++;
            r = us_slice(r, j, r.n);
        }
    }
    return r;
}

static us provider_label(void) {
    if (us_eq_c(env_get("CLAUDE_CODE_USE_BEDROCK"), "1")) return US("Bedrock");
    if (us_eq_c(env_get("CLAUDE_CODE_USE_VERTEX"), "1")) return US("Vertex");
    // isMiniMaxAnthropicEndpoint compares new URL(base).origin + pathname without trailing slashes.
    // A URL can only normalize to those two endpoints if it already spells one of them, give or
    // take letter case and trailing slashes.
    us base = us_trim(env_get("ANTHROPIC_BASE_URL"));
    if (!base.n) base = us_trim(env_get("ANTHROPIC_API_BASE_URL"));
    if (base.n) {
        us low = us_lower(base);
        static const char *const ends[] = {"https://api.minimax.io/anthropic", "https://api.minimaxi.com/anthropic"};
        for (size_t i = 0; i < countof(ends); i++) {
            if (!us_starts_c(low, ends[i])) continue;
            us rest = us_slice(base, (int32_t)strlen(ends[i]), base.n);
            bool slashes = true;
            for (int32_t k = 0; k < rest.n; k++) slashes &= rest.p[k] == '/';
            if (slashes && !us_starts_c(base, ends[i])) UNSUPPORTED("MiniMax URL in mixed case");
            if (slashes) return US("MiniMax");
        }
        if (us_find_c(low, "minimax", 0) >= 0 || us_find_ch(low, '%', 0) >= 0 || us_find_ch(low, '\\', 0) >= 0)
            UNSUPPORTED("ANTHROPIC_BASE_URL needing URL normalization");
    }
    jv id = sget("model", "id");
    if (jis_str(id)) {
        us low = us_lower(jstr(id));
        if (us_eq_c(low, "opusplan") || us_eq_c(low, "sonnetplan") || us_eq_c(low, "haikuplan")) return US("Enterprise");
    }
    return US_EMPTY;
}

// ---- effort.js ----

static bool effort(us *level, us *symbol) {
    jv e = sget("effort", nullptr);
    us l;
    if (!stdin_text(jget(e, "level"), 32, &l)) return false;
    l = us_lower(l);
    static const char *const names[] = {"low", "medium", "high", "xhigh", "max"};
    static const us syms[] = {US_INIT("○"), US_INIT("◔"), US_INIT("◑"), US_INIT("◕"), US_INIT("●")};
    static_assert(countof(names) == countof(syms));
    *symbol = US_EMPTY;
    for (size_t i = 0; i < countof(names); i++)
        if (us_eq_c(l, names[i])) *symbol = syms[i];
    // SYMBOLS is a plain object: these names reach Object.prototype upstream.
    static const char *const proto[] = {"constructor", "tostring", "valueof", "hasownproperty", "isprototypeof", "__proto__",
                                        "propertyisenumerable", "tolocalestring", "__definegetter__", "__definesetter__",
                                        "__lookupgetter__", "__lookupsetter__"};
    for (size_t i = 0; i < countof(proto); i++)
        if (us_eq_c(l, proto[i])) UNSUPPORTED("effort level named like an Object.prototype member");
    *level = C->t->ultracode ? CAT(US("ultracode("), l, US(")")) : l;
    return true;
}

// ---- utils/hyperlinks.js: pathToFileURL(path.resolve(p)), then new URL() ----

static us file_href(us path) {
    sb b = {0};
    sb_us(&b, US("file://"));
    static const char hex[] = "0123456789ABCDEF";
    size_t n;
    char *u8 = us_to_utf8(file_url_path(path_resolve(path)), &n);
    // A backslash is left only where it is a file name character, which pathToFileURL encodes.
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)u8[i];
        if (c <= 0x20 || c >= 0x7F || strchr("\"#%<>?[\\]^`{|}~", c)) {
            sb_ch(&b, '%');
            sb_ch(&b, (uint16_t)hex[c >> 4]);
            sb_ch(&b, (uint16_t)hex[c & 15]);
        } else sb_ch(&b, c);
    }
    return sb_done(&b);
}

static us hyperlink(us uri, us text) {
    if (!uri.n) return text;
    return CAT(US("\x1b]8;;"), uri, US("\x1b\\"), text, US("\x1b]8;;\x1b\\"));
}

// ---- render/parts.js ----

static us effort_suffix(void) {
    us level, symbol;
    if (!CF->d.show_effort || !effort(&level, &symbol)) return US_EMPTY;
    // The symbol alone can't carry the ultracode marker, so symbol mode keeps the full form.
    if (CF->d.effort_format == EF_SYMBOL && symbol.n && !us_starts_c(level, "ultracode(")) return CAT(US(" "), symbol);
    return CF->d.effort_format == EF_TEXT || !symbol.n ? CAT(US(" "), level) : CAT(US(" "), symbol, US(" "), level);
}

static us model_badge(void) {
    us name = sanitize_display(format_model_name(resolve_model_name()));
    us core = CAT(name, effort_suffix());
    us provider = provider_label();
    us text = provider.n ? CAT(core, US(" | "), provider) : core;
    if (CF->d.show_provider) {
        us shown = us_trim(CF->d.provider_name);
        if (!shown.n) shown = provider;
        text = shown.n ? CAT(shown, US(" | "), core) : core;
    }
    return c_model(CAT(US("["), text, US("]")));
}

static int split_segments(us s, us *segs, int max) {
    int n = 0;
    for (int32_t i = 0; i <= s.n && n < max;) {
        int32_t j = i;
        while (j < s.n && s.p[j] != '/' && s.p[j] != '\\') j++;
        if (j > i) segs[n++] = us_slice(s, i, j);
        i = j + 1;
    }
    return n;
}

static us format_project_path(us cwd, int levels) {
    us safe = sanitize_display(cwd);
    us segs[256];
    int n = split_segments(safe, segs, 256);
    bool lead = safe.n && (safe.p[0] == '/' || safe.p[0] == '\\');
    if (levels != 0) {
        int from = n - levels > 0 ? n - levels : 0;
        us shown = join(segs + from, n - from, US("/"));
        return shown.n ? shown : lead ? US("/") : safe;
    }
    if (safe.n >= 2 && lead && (safe.p[1] == '/' || safe.p[1] == '\\')) return n ? CAT(US("//"), join(segs, n, US("/"))) : US("//");
    if (safe.n >= 3 && ((safe.p[0] | 32) >= 'a' && (safe.p[0] | 32) <= 'z') && safe.p[1] == ':' && (safe.p[2] == '/' || safe.p[2] == '\\')) {
        us j = join(segs, n, US("/"));
        return n == 1 ? CAT(j, US("/")) : j;
    }
    if (lead) return n ? CAT(US("/"), join(segs, n, US("/"))) : US("/");
    us j = join(segs, n, US("/"));
    return j.n ? j : safe;
}

// Linked added-dir names, capped at five; prefix is the inline "+".
static us added_dirs(us prefix, us joiner) {
    jv arr = sget("workspace", "added_dirs");
    if (!jis_arr(arr)) return US_EMPTY;
    us dirs[256], names[256];
    int n = 0;
    size_t idx, max;
    jv v;
    yyjson_arr_foreach(arr, idx, max, v) {
        if (!jis_str(v) || !yyjson_get_len(v) || n >= 256) continue;
        us dir = jstr(v), segs[256];
        int ns = split_segments(dir, segs, 256);
        us name = sanitize_display(ns ? segs[ns - 1] : dir);
        if (name.n > 24) name = CAT(us_slice(name, 0, 23), US("…"));
        if (name.n) dirs[n] = dir, names[n++] = name;
    }
    if (!n) return US_EMPTY;
    us shown[6];
    int k = 0;
    for (; k < n && k < 5; k++) shown[k] = hyperlink(file_href(dirs[k]), dim(CAT(prefix, names[k])));
    if (n > 5) shown[k++] = dim(CAT(US("+"), num(n - 5), US(" more")));
    return join(shown, k, joiner);
}

// ---- render/vcs.js ----

static us vcs_part(bool expanded) {
    VcsStatus *st = C->vcs;
    if (!st || !(st->jj ? CF->jj.enabled : CF->git.enabled)) return US_EMPTY;
    bool dirty = (st->jj ? CF->jj.show_dirty : CF->git.show_dirty) && st->dirty;
    us inner[8];
    int n = 0;
    inner[n++] = hyperlink(st->jj ? US_EMPTY : st->branch_url, c_branch(CAT(sanitize_display(st->branch), dirty ? US("*") : US_EMPTY)));
    if (!st->jj && CF->git.show_ahead_behind) {
        if (st->ahead > 0) {
            us v = CAT(US("↑"), num(st->ahead));
            inner[n++] = CF->git.push_crit > 0 && st->ahead >= CF->git.push_crit   ? c_critical(v)
                         : CF->git.push_warn > 0 && st->ahead >= CF->git.push_warn ? c_warning(v)
                                                                                    : c_branch(v);
        }
        if (st->behind > 0) inner[n++] = c_branch(CAT(US("↓"), num(st->behind)));
    }
    if (!st->jj && CF->git.show_file_stats) {
        if (expanded && st->has_line_diff) {
            us diff[2];
            int nd = 0;
            if (st->line_added > 0) diff[nd++] = green(CAT(US("+"), num(st->line_added)));
            if (st->line_deleted > 0) diff[nd++] = red(CAT(US("-"), num(st->line_deleted)));
            if (nd) inner[n++] = CAT(US("["), join(diff, nd, US(" ")), US("]"));
        }
        if (!expanded && st->has_file_stats) {
            double cnt[] = {st->modified, st->added, st->deleted, st->untracked};
            static const us sym[] = {US_INIT("!"), US_INIT("+"), US_INIT("✘"), US_INIT("?")};
            static_assert(countof(cnt) == countof(sym));
            us parts[countof(sym)];
            int np = 0;
            for (size_t i = 0; i < countof(sym); i++)
                if (cnt[i] > 0) parts[np++] = CAT(sym[i], num(cnt[i]));
            if (np) inner[n++] = c_branch(join(parts, np, US(" ")));
        }
    }
    if (st->jj && CF->jj.show_conflicts && st->conflict) inner[n++] = c_critical(US("!conflict"));
    us worktree = US_EMPTY;
    if (!st->jj && CF->git.show_worktree) {
        jv w = sget("workspace", "git_worktree");
        if (!jis_nullish(w) && !jis_str(w)) UNSUPPORTED("non-string git_worktree");
        us name = jis_str(w) ? us_trim(sanitize_display(jstr(w))) : US_EMPTY;
        if (name.n) worktree = CAT(US(" "), c_git(CAT(US("⎇ "), name)));
    }
    return CAT(c_git(st->jj ? US("jj:(") : US("git:(")), join(inner, n, US(" ")), c_git(US(")")), worktree);
}

// insideCwd: path.resolve(cwd, candidate) when it stays inside cwd
static bool inside_cwd(us cwd, us candidate, us *out) {
    us base = path_resolve(cwd);
    us resolved = path_join(base, candidate);
    us lb = path_key(base), lr = path_key(resolved);
    us rel;
    if (us_eq(lb, lr)) rel = US_EMPTY;
    else if (lr.n > lb.n && us_eq(us_slice(lr, 0, lb.n), lb) && (lr.p[lb.n] == OS_SEP || lb.p[lb.n - 1] == OS_SEP))
        rel = us_slice(resolved, lb.p[lb.n - 1] == OS_SEP ? lb.n : lb.n + 1, resolved.n);
    else return false;
    if (us_starts_c(rel, "..")) return false;
    *out = resolved;
    return true;
}

static us git_files_line(void) {
    VcsStatus *st = C->vcs;
    if (!CF->git.show_file_stats || !st || !st->has_file_stats) return US_EMPTY;
    if (st->nfiles == 0 && st->untracked == 0) return US_EMPTY;
    if (C->width >= 0 && C->width < 60) return US_EMPTY;
    jv cwdv = sget("cwd", nullptr);
    bool has_cwd = jtruthy(cwdv) && jis_str(cwdv);
    us cwd = has_cwd ? jstr(cwdv) : US_EMPTY;
    int n = st->nfiles;
    double *mt = arena_alloc(sizeof(double) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        us r;
        mt[i] = 0;
        if (has_cwd && inside_cwd(cwd, st->files[i].full, &r)) {
            fstat_t fs = stat_path(r);
            mt[i] = fs.ok ? fs.mtime_ms : NAN;  // NaN: statSync threw, the comparator answers 0
        }
    }
    // Stable insertion sort, newest first, like the comparator upstream.
    int *ord = arena_alloc(sizeof(int) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) ord[i] = i;
    for (int i = 1; i < n; i++) {
        int x = ord[i], j = i - 1;
        while (j >= 0) {
            double a = mt[ord[j]], b = mt[x];
            if (isnan(a) || isnan(b) || !(b - a > 0)) break;
            ord[j + 1] = ord[j], j--;
        }
        ord[j + 1] = x;
    }
    us entries[9];
    int ne = 0;
    for (int k = 0; k < n && k < 6; k++) {
        GitFile *f = &st->files[ord[k]];
        us (*color)(us) = f->type == TF_ADDED ? green : f->type == TF_DELETED ? red : yellow;
        static const us prefix[] = {[TF_ADDED] = US_INIT("+"), [TF_DELETED] = US_INIT("-"), [TF_MODIFIED] = US_INIT("~")};
        us r;
        bool in = has_cwd && inside_cwd(cwd, f->full, &r);
        us name = color(sanitize_display(f->basename));
        us entry = CAT(color(prefix[f->type]), in ? hyperlink(file_href(r), name) : name);
        if (f->has_diff) {
            us dp[2];
            int nd = 0;
            if (f->added > 0) dp[nd++] = green(CAT(US("+"), num(f->added)));
            if (f->deleted > 0) dp[nd++] = red(CAT(US("-"), num(f->deleted)));
            if (nd) entry = CAT(entry, dim(CAT(US("("), join(dp, nd, US(" ")), US(")"))));
        }
        entries[ne++] = entry;
    }
    if (n > 6) entries[ne++] = dim(CAT(US("+"), num(n - 6), US(" more")));
    if (st->untracked > 0) entries[ne++] = dim(CAT(US("?"), num(st->untracked)));
    return join(entries, ne, US("  "));
}

// projectParts: the path with its VCS segment, as one part or, with branchOverflow "wrap", two
static int project_parts(bool expanded, us *out) {
    us project = US_EMPTY;
    jv cwd = sget("cwd", nullptr);
    if (CF->d.show_project && jtruthy(cwd)) {
        if (!jis_str(cwd)) UNSUPPORTED("non-string cwd");
        else {
            us text = c_project(format_project_path(jstr(cwd), CF->path_levels));
            project = expanded ? hyperlink(file_href(jstr(cwd)), text) : text;
        }
    }
    if (expanded && CF->d.show_added_dirs && !CF->d.added_dirs_line) {
        us dirs = added_dirs(US("+"), US(" "));
        if (dirs.n) project = project.n ? CAT(project, US(" "), dirs) : dirs;
    }
    us vcs = vcs_part(expanded);
    if (project.n && vcs.n) {
        if (CF->git.wrap) {
            out[0] = project, out[1] = vcs;
            return 2;
        }
        out[0] = CAT(project, US(" "), vcs);
        return 1;
    }
    out[0] = project.n ? project : vcs;
    return out[0].n ? 1 : 0;
}

static us advisor_clean(us s) { return us_slice(sanitize_display(us_trim(s)), 0, 64); }

// The model families in ids, for prettify_advisor and short_model.
static const us FAMILIES[] = {US_INIT("opus"), US_INIT("sonnet"), US_INIT("haiku")};

static us capitalize(us word) {  // lower-case ASCII
    sb x = {0};
    sb_ch(&x, (uint16_t)(word.p[0] - 32));
    sb_us(&x, us_slice(word, 1, word.n));
    return sb_done(&x);
}

// `claude-opus-4-7` -> `Opus 4.7`; aliases like `opus` -> `Opus`
static us prettify_advisor(us id) {
    id = us_trim(id);
    us low = us_lower(id);
    us rest = us_starts_c(low, "claude-") ? us_slice(id, 7, id.n) : id;
    us rlow = us_lower(rest);
    for (size_t f = 0; f < countof(FAMILIES); f++) {  // ^(?:claude-)?(fam)-(\d+)-(\d+)
        int32_t fl = FAMILIES[f].n;
        if (!us_starts(rlow, FAMILIES[f]) || rest.n <= fl || rest.p[fl] != '-') continue;
        int32_t a = fl + 1, b = a;
        while (b < rest.n && rest.p[b] >= '0' && rest.p[b] <= '9') b++;
        if (b == a || b >= rest.n || rest.p[b] != '-') continue;
        int32_t c = b + 1, d = c;
        while (d < rest.n && rest.p[d] >= '0' && rest.p[d] <= '9') d++;
        if (d == c) continue;
        return CAT(capitalize(FAMILIES[f]), US(" "), us_slice(rest, a, b), US("."), us_slice(rest, c, d));
    }
    for (size_t f = 0; f < countof(FAMILIES); f++)
        if (us_eq(low, FAMILIES[f])) return capitalize(FAMILIES[f]);
    return us_starts_c(low, "claude-") ? us_slice(id, 7, id.n) : id;
}

static us advisor_part(void) {
    if (!CF->d.show_advisor) return US_EMPTY;
    us name = advisor_clean(CF->d.advisor_override);
    if (!name.n) name = advisor_clean(prettify_advisor(advisor_clean(C->t->advisor)));
    return name.n ? CAT(label(CAT(t(T_ADVISOR), US(":"))), US(" "), name) : US_EMPTY;
}

static us labeled(us text) { return text.n ? label(text) : US_EMPTY; }

static us session_name_part(void) {
    us s;
    return CF->d.show_session_name && stdin_text(sget("session_name", nullptr), 80, &s) ? label(s) : US_EMPTY;
}

static us version_part(void) {
    us v;
    return CF->d.show_cc_version && stdin_text(sget("version", nullptr), 32, &v) ? label(CAT(US("CC v"), v)) : US_EMPTY;
}

static us duration_part(void) {  // formatSessionDuration(cost.total_duration_ms)
    if (!CF->d.show_duration) return US_EMPTY;
    jv d = sget("cost", "total_duration_ms");
    if (!is_finite_num(d) || jnum(d) < 0) return US_EMPTY;
    double mins = floor(jnum(d) / 60000);
    us text = mins < 1 ? US("<1m") : mins < 60 ? CAT(num(mins), US("m")) : CAT(num(floor(mins / 60)), US("h "), num(fmod(mins, 60)), US("m"));
    return label(CAT(US("⏱️  "), text));
}

static us format_usd(double v) {
    if (v >= 1) return CAT(US("$"), fixed(v, 2));
    if (v >= 0.1) return CAT(US("$"), fixed(v, 3));
    return CAT(US("$"), fixed(v, 4));
}

static us cost_part(void) {
    us parts[3];
    int n = 0;
    double usd;
    if (CF->d.show_cost && native_cost(C, &usd)) parts[n++] = CAT(t(T_COST), US(" "), format_usd(usd));
    if (CF->d.show_daily_cost && C->has_costs) parts[n++] = CAT(t(T_TODAY), US(" "), format_usd(C->today));
    if (CF->d.show_weekly_cost && C->has_costs && C->has_week) parts[n++] = CAT(t(T_WEEK), US(" "), format_usd(C->week));
    return labeled(join(parts, n, US(" | ")));
}

static us speed_part(void) {
    if (!CF->d.show_speed || !C->has_speed) return US_EMPTY;
    return label(CAT(t(T_OUT), US(": "), fixed(C->speed, 1), US(" "), t(T_TOK_PER_SEC)));
}

static us auth_part(void) {
    us parts[2];
    int n = 0;
    if (CF->d.show_auth && C->has_auth_method) parts[n++] = C->auth_method;
    if (CF->d.show_auth_user && C->has_auth_user) {
        us u = C->auth_user;
        double max = CF->d.auth_user_length;
        if (max > 0 && u.n > max) u = CAT(us_slice(u, 0, (int32_t)max), US("…"));
        parts[n++] = u;
    }
    return labeled(join(parts, n, US(" · ")));
}

static us custom_line_part(bool first) {
    if (!CF->d.custom_line.n || CF->d.custom_first != first) return US_EMPTY;
    return c_custom(CF->d.custom_line);
}

static int config_count_parts(us mcp_suffix, us *out) {
    int total = C->claude_md + C->rules + C->mcp + C->hooks;
    if (!CF->d.show_config_counts || total == 0 || total < CF->d.env_threshold) return 0;
    int n = 0;
    if (C->claude_md > 0) out[n++] = label(CAT(num(C->claude_md), US(" CLAUDE.md")));
    if (C->rules > 0) out[n++] = label(CAT(num(C->rules), US(" "), t(T_RULES)));
    if (C->mcp > 0) out[n++] = CAT(label(CAT(num(C->mcp), US(" MCPs"))), mcp_suffix);
    if (C->hooks > 0) out[n++] = label(CAT(num(C->hooks), US(" "), t(T_HOOKS)));
    return n;
}

static us session_tokens_summary(us prefix) {
    Transcript *tr = C->t;
    double total = tr->tok_in + tr->tok_out + tr->tok_cc + tr->tok_cr;
    if (total == 0) return US_EMPTY;
    us parts = CAT(t(T_IN), US(": "), format_tokens(tr->tok_in), US(", "), t(T_OUT), US(": "), format_tokens(tr->tok_out));
    double cache = tr->tok_cc + tr->tok_cr;
    if (cache > 0) parts = CAT(parts, US(", "), t(T_CACHE), US(": "), format_tokens(cache));
    return label(CAT(prefix, US(" "), format_tokens(total), US(" ("), parts, US(")")));
}

static us compactions_part(void) {
    int n = C->t->compactions;
    return CF->d.show_compactions && n > 0 ? label(CAT(t(T_COMPACTIONS), US(": "), num(n))) : US_EMPTY;
}

// ---- render/order.js ----

typedef struct { int key; us text; } KPart;  // key -1: unkeyed, else a SEG_ value

// Room for every part a line can have: the compact line adds at most 31.
enum { MAX_PARTS = 48 };

// Unkeyed parts keep their slots, a key's parts stay together, keys missing from the order
// follow in their original order.
static us order_parts(KPart *parts, int n, us sep) {
    int keys[SEG_COUNT], nk = 0;
    for (int i = 0; i < n; i++) {
        if (parts[i].key < 0) continue;
        bool seen = false;
        for (int k = 0; k < nk; k++) seen |= keys[k] == parts[i].key;
        if (!seen) keys[nk++] = parts[i].key;
    }
    int order[SEG_COUNT], no = 0;
    bool used[SEG_COUNT] = {0};
    for (int o = 0; o < CF->nplo; o++)
        for (int k = 0; k < nk; k++)
            if (keys[k] == CF->plo[o] && !used[keys[k]]) order[no++] = keys[k], used[keys[k]] = true;
    for (int k = 0; k < nk; k++)
        if (!used[keys[k]]) order[no++] = keys[k], used[keys[k]] = true;
    us reordered[MAX_PARTS];
    int nr = 0;
    for (int o = 0; o < no; o++)
        for (int i = 0; i < n; i++)
            if (parts[i].key == order[o]) reordered[nr++] = parts[i].text;
    us result[MAX_PARTS];
    for (int i = 0, k = 0; i < n; i++) result[i] = parts[i].key >= 0 ? reordered[k++] : parts[i].text;
    return join(result, n, sep);
}

// ---- render/time.js ----

static int g_locale = -1;  // 0 zh-CN, 1 en-US; the node default locale decides the shape

bool init_locale(void) {
    int l = os_locale();
    g_locale = l < 0 ? 1 : l;
    return l >= 0;
}

static us time_str(ltime lt) {
    int hc = CF->d.hour_cycle, h = lt.h;
    bool twelve = g_locale == 1 ? (hc == HC_AUTO || hc == HC_H11 || hc == HC_H12) : (hc == HC_H11 || hc == HC_H12);
    int shown = h;
    if (twelve) shown = hc == HC_H11 ? h % 12 : (h % 12 == 0 ? 12 : h % 12);
    else if (hc == HC_H24 && h == 0) shown = 24;
    sb b = {0};
    sb_pad(&b, shown, 2);
    sb_ch(&b, ':'), sb_pad(&b, lt.mi, 2);
    if (CF->d.show_clock_seconds) sb_ch(&b, ':'), sb_pad(&b, lt.s, 2);
    us core = sb_done(&b);
    if (!twelve) return core;
    if (g_locale == 0) return CAT(h < 12 ? US("上午") : US("下午"), core);
    return CAT(core, h < 12 ? US(" AM") : US(" PM"));
}

static us date_str(ltime lt) {
    static const us mon[] = {US_INIT("Jan"), US_INIT("Feb"), US_INIT("Mar"), US_INIT("Apr"), US_INIT("May"), US_INIT("Jun"),
                             US_INIT("Jul"), US_INIT("Aug"), US_INIT("Sep"), US_INIT("Oct"), US_INIT("Nov"), US_INIT("Dec")};
    return g_locale == 0 ? CAT(us_int(lt.mo), US("月"), us_int(lt.d), US("日")) : CAT(mon[lt.mo - 1], US(" "), us_int(lt.d));
}

// Wall-clock time such as `at 14:30`, with the date when it isn't today.
static us format_absolute(double at, int pattern) {
    ltime a, b;
    if (!local_time(at, &a)) {
        UNSUPPORTED("unrepresentable time");
        return US_EMPTY;
    }
    us ts = time_str(a);
    if (local_time(C->now, &b) && a.y == b.y && a.mo == b.mo && a.d == b.d) return interpolate(t(pattern), "time", ts);
    return interpolate(t(pattern), "time", CAT(date_str(a), US(" "), ts));
}

static us format_duration(double ms) {
    double mins = ceil(ms / 60000);
    if (mins < 60) return CAT(num(mins), US("m"));
    double hours = floor(mins / 60), rem = fmod(mins, 60);
    if (hours >= 24) {
        double days = floor(hours / 24), rh = fmod(hours, 24);
        return rh > 0 ? CAT(num(days), US("d "), num(rh), US("h")) : CAT(num(days), US("d"));
    }
    return rem > 0 ? CAT(num(hours), US("h "), num(rem), US("m")) : CAT(num(hours), US("h"));
}

// `2h 30m`, `at 14:30`, or both; empty when the reset is unknown or already past
static us format_reset_time(double at, int mode) {
    if (isnan(at)) return US_EMPTY;
    double remaining = at - C->now;
    if (remaining <= 0) return US_EMPTY;
    if (mode == TF_RELATIVE) return format_duration(remaining);
    us abs = format_absolute(at, T_ABS_TIME);
    return mode == TF_ABSOLUTE ? abs : CAT(format_duration(remaining), US(", "), abs);
}

static us format_elapsed(double at, double window) {
    if (isnan(at)) return US_EMPTY;
    double e = js_round((C->now - (at - window)) / window * 100);
    return interpolate(t(T_ELAPSED), "value", num(fmax(0, fmin(100, e))));
}

static us format_window_time(double at, double window, int tf) {
    if (tf == TF_ELAPSED) return format_elapsed(at, window);
    if (tf == TF_ELAPSED_ABSOLUTE) {
        us e = format_elapsed(at, window), a = format_reset_time(at, TF_ABSOLUTE);
        return e.n && a.n ? CAT(e, US(", "), a) : e.n ? e : a;
    }
    return format_reset_time(at, tf);
}

static int limit_time_format(int tf) { return tf == TF_ELAPSED_ABSOLUTE ? TF_ABSOLUTE : tf == TF_ELAPSED ? TF_RELATIVE : tf; }

// `45s ago`, `2h 5m ago`, `3d 4h ago`
static us format_ago(double ms) {
    if (ms < 0) return t(T_JUST_NOW);
    double s = floor(ms / 1000), m = floor(s / 60), h = floor(m / 60), d = floor(h / 24);
    us x = s < 60   ? CAT(num(s), US("s"))
           : m < 60 ? CAT(num(m), US("m"))
           : h < 24 ? (fmod(m, 60) > 0 ? CAT(num(h), US("h "), num(fmod(m, 60)), US("m")) : CAT(num(h), US("h")))
                    : (fmod(h, 24) > 0 ? CAT(num(d), US("d "), num(fmod(h, 24)), US("h")) : CAT(num(d), US("d")));
    return interpolate(t(T_RELATIVE_TIME), "value", x);
}

// ---- usage-pace.js ----

static const double FIVE_HOUR = 5 * 3600000.0, SEVEN_DAY = 7 * 86400000.0;

static int usage_pace(double pct, double reset, double window) {
    if (!CF->d.usage_pace || isnan(pct) || isnan(reset)) return PACE_NONE;
    double remaining = reset - C->now;
    if (!isfinite(remaining) || remaining <= 0 || remaining >= window) return PACE_NONE;
    if (pct < 10) return PACE_NORMAL;
    double projected = pct / ((window - remaining) / window);
    return projected > 100 ? PACE_CRITICAL : projected >= 90 ? PACE_WARNING : PACE_NORMAL;
}

// ---- render/usage.js ----

typedef struct { us label; int label_key; double percent, reset, window; int pace; bool force_label; us duration_label; } Window;

static us format_window(bool compact, Window w, Align a) {
    int tf = CF->d.time_format;
    us percent = quota_percent(w.percent, w.pace), reset = format_window_time(w.reset, w.window, tf);
    if (CF->d.usage_compact) {
        us lab = label(CAT(w.label, US(":")));
        return reset.n ? CAT(lab, US(" "), percent, US(" "), label(CAT(US("("), reset, US(")")))) : CAT(lab, US(" "), percent);
    }
    bool wording = CF->d.show_reset_label && tf != TF_ELAPSED && tf != TF_ELAPSED_ABSOLUTE;
    us reset_text = reset.n && wording ? CAT(t(tf == TF_ABSOLUTE ? T_RESETS : T_RESETS_IN), US(" "), reset) : reset;
    us styled = w.label_key >= 0 ? bar_label(w.label_key, a) : label(w.label);
    if (CF->d.usage_bar) {
        us bar_reset = compact && tf == TF_RELATIVE && reset.n ? CAT(reset, US(" / "), w.duration_label.n ? w.duration_label : w.label) : reset_text;
        us body = CAT(quota_bar(isnan(w.percent) ? 0 : w.percent, w.pace), US(" "), percent,
                      bar_reset.n ? CAT(US(" ("), bar_reset, US(")")) : US_EMPTY);
        return w.force_label ? CAT(styled, US(" "), body) : body;
    }
    return CAT(styled, US(" "), percent, reset_text.n ? CAT(US(" ("), reset_text, US(")")) : US_EMPTY);
}

static us limit_notice(void) {
    Usage *u = &C->usage;
    int format = limit_time_format(CF->d.time_format);
    us reset = format_reset_time(u->five == 100 ? u->five_reset : u->seven_reset, format);
    if (CF->d.usage_compact) return c_critical(CAT(US("⚠ Limit"), reset.n ? CAT(US(" ("), reset, US(")")) : US_EMPTY));
    us suffix = !reset.n                 ? US_EMPTY
                : CF->d.show_reset_label ? CAT(US(" ("), t(format == TF_ABSOLUTE ? T_RESETS : T_RESETS_IN), US(" "), reset, US(")"))
                                         : CAT(US(" ("), reset, US(")"));
    return c_critical(CAT(US("⚠ "), t(T_LIMIT_REACHED), suffix));
}

// The usage windows as separator-joined parts; 0 for null.
static int usage_parts(bool compact, Align a, us *out) {
    if (!CF->d.show_usage || !C->has_usage) return 0;
    Usage *u = &C->usage;
    us usage_label = bar_label(T_USAGE, a);
    int nsc = CF->d.show_model_scoped ? u->nscoped : 0;
    bool has_data = !isnan(u->five) || !isnan(u->seven) || nsc > 0;
    int n = 0;
#define WITH_LABEL(x) CAT(usage_label, US(" "), (x))
#define BALANCE() \
    if (u->has_balance) out[n++] = u->balance
    if (u->has_balance && !has_data) {
        out[0] = WITH_LABEL(u->balance);
        return 1;
    }
    int five_pace = usage_pace(u->five, u->five_reset, FIVE_HOUR), seven_pace = usage_pace(u->seven, u->seven_reset, SEVEN_DAY);
    int *scoped_pace = arena_alloc(sizeof(int) * (size_t)(nsc + 1));
    bool alert = pace_alert(five_pace) || pace_alert(seven_pace);
    for (int i = 0; i < nsc; i++) {
        scoped_pace[i] = usage_pace(u->scoped[i].percent, u->scoped[i].reset, SEVEN_DAY);
        alert |= pace_alert(scoped_pace[i]);
    }
    bool show_seven = !isnan(u->seven) && (u->seven >= CF->d.seven_day_threshold || pace_alert(seven_pace));
    us *scoped = arena_alloc(sizeof(us) * (size_t)(nsc + 1));
    for (int i = 0; i < nsc; i++)
        scoped[i] = format_window(compact, (Window){u->scoped[i].label, -1, u->scoped[i].percent, u->scoped[i].reset, SEVEN_DAY,
                                                    scoped_pace[i], true, US("7d")}, a);
    if (u->five == 100 || u->seven == 100) {
        us notice = limit_notice();
        out[n++] = compact || CF->d.usage_compact ? notice : WITH_LABEL(notice);
        for (int i = 0; i < nsc; i++) out[n++] = scoped[i];
        BALANCE();
        return n;
    }
    double eff = fmax(isnan(u->five) ? 0 : u->five, isnan(u->seven) ? 0 : u->seven);
    for (int i = 0; i < nsc; i++) eff = fmax(eff, isnan(u->scoped[i].percent) ? 0 : u->scoped[i].percent);
    if (eff < CF->d.usage_threshold && !alert) {
        if (!u->has_balance) return 0;
        out[0] = compact ? u->balance : WITH_LABEL(u->balance);
        return 1;
    }
    Window five = {US("5h"), -1, u->five, u->five_reset, FIVE_HOUR, five_pace, false, US_EMPTY};
    Window seven = {CF->d.usage_compact ? US("7d") : t(T_WEEKLY), T_WEEKLY, u->seven, u->seven_reset, SEVEN_DAY, seven_pace, true, US_EMPTY};
    if (CF->d.usage_compact) {
        if (!isnan(u->five)) out[n++] = format_window(compact, five, a);
        if (!isnan(u->seven) && (isnan(u->five) || show_seven)) out[n++] = format_window(compact, seven, a);
        for (int i = 0; i < nsc; i++) out[n++] = scoped[i];
        if (!n) return 0;
        BALANCE();
        return n;
    }
    if (isnan(u->five) && isnan(u->seven)) {
        if (nsc) {
            out[n++] = WITH_LABEL(scoped[0]);
            for (int i = 1; i < nsc; i++) out[n++] = scoped[i];
            BALANCE();
            return n;
        }
        if (!u->has_balance) return 0;
        out[0] = WITH_LABEL(u->balance);
        return 1;
    }
    if (isnan(u->five)) {
        us weekly = format_window(compact, seven, a);
        out[n++] = compact ? weekly : WITH_LABEL(weekly);
    } else {
        out[n++] = WITH_LABEL(format_window(compact, five, a));
        if (show_seven) out[n++] = format_window(compact, seven, a);
    }
    for (int i = 0; i < nsc; i++) out[n++] = scoped[i];
    BALANCE();
    return n;
#undef WITH_LABEL
#undef BALANCE
}

// ---- render/context.js ----

static void context_bar_and_value(us *bar, us *value) {
    ContextUse u = context_usage();
    *value = CAT(context_color(u.percent, CF->d.ctx_warn, CF->d.ctx_crit), format_context_value(u, CF->d.context_value), RESET);
    *bar = CF->d.show_context_bar ? colored_bar(u.percent) : US_EMPTY;
}

// ` (in: 12k, cache: 180k)` once context reaches the critical threshold
static us token_breakdown(void) {
    jv usage = sget("context_window", "current_usage");
    if (!CF->d.show_token_breakdown || !jtruthy(usage)) return US_EMPTY;
    if (context_usage().percent < CF->d.ctx_crit) return US_EMPTY;
    us in = format_tokens(usage_field("input_tokens"));
    us cache = format_tokens(usage_field("cache_creation_input_tokens") + usage_field("cache_read_input_tokens"));
    return label(CAT(US(" ("), t(T_IN), US(": "), in, US(", "), t(T_CACHE), US(": "), cache, US(")")));
}

static us context_line(Align a) {
    us bar, value;
    context_bar_and_value(&bar, &value);
    return CAT(bar_label(T_CONTEXT, a), US(" "), bar.n ? CAT(bar, US(" ")) : US_EMPTY, value, token_breakdown());
}

// ---- render/lines.js ----

static us environment_line(void) {
    bool show_errors = CF->d.show_config_counts || CF->d.show_mcp;
    us errors = US_EMPTY;
    Transcript *tr = C->t;
    if (show_errors && tr->nmcp_err > 0) {
        us safe[64];
        int n = 0;
        for (int i = 0; i < tr->nmcp_err && n < 64; i++) {
            us s = us_slice(us_trim(sanitize_display(tr->mcp_errors[i])), 0, 64);
            if (s.n) safe[n++] = s;
        }
        errors = c_critical(CAT(US("⚠ "), join(safe, n < 3 ? n : 3, US(", ")), n > 3 ? CAT(US(" +"), num(n - 3)) : US_EMPTY));
    }
    us parts[8];
    int n = config_count_parts(errors.n ? CAT(US(" "), errors) : US_EMPTY, parts);
    bool counted_mcp = n > 0 && C->mcp > 0;
    us style;
    if (CF->d.show_output_style && stdin_text(sget("output_style", "name"), 40, &style)) parts[n++] = label(CAT(US("style: "), style));
    if (errors.n && !counted_mcp) parts[n++] = errors;
    return join(parts, n, US(" | "));
}

// The expiry time rather than a countdown: between turns the statusline isn't repainted.
static us prompt_cache_line(void) {
    jv cache = sget("prompt_cache", nullptr);
    if (!CF->d.show_prompt_cache || !jtruthy(jget(cache, "caching_observed"))) return US_EMPTY;
    jv ex = jget(cache, "expires_at");
    double expires = jis_num(ex) ? jnum(ex) * 1000 : 0;
    double remaining = jtruthy(jget(cache, "warm")) ? expires - C->now : 0;
    us value;
    if (!(remaining > 0)) {
        value = label(CAT(US("⏱ "), t(T_EXPIRED)));
    } else {
        jv ttl = jget(cache, "ttl");
        double secs = jstr_eq(ttl, "1h") ? 3600 : 300;
        if (jis_str(ttl) && !jstr_eq(ttl, "5m") && !jstr_eq(ttl, "1h")) {
            static const char *const proto[] = {"constructor", "toString", "valueOf", "hasOwnProperty", "isPrototypeOf", "__proto__",
                                                "propertyIsEnumerable", "toLocaleString"};
            for (size_t i = 0; i < countof(proto); i++)
                if (jstr_eq(ttl, proto[i])) UNSUPPORTED("prompt cache ttl named like an Object.prototype member");
        }
        if (fabs(expires) > 8.64e15) UNSUPPORTED("prompt cache expiry out of range");
        double warn = fmax(60, floor(secs / 5)) * 1000;
        us until = CAT(US("⏱ "), format_absolute(trunc(expires), T_UNTIL_TIME));
        value = remaining <= warn ? c_warning(until) : CAT(context_color(0, 70, 85), until, RESET);
    }
    return CAT(label(t(T_PROMPT_CACHE)), US(" "), value);
}

static us cache_hit_rate_line(void) {
    jv ratio = jget(sget("prompt_cache", nullptr), "hit_ratio");
    if (!CF->d.show_cache_hit_rate || !is_finite_num(ratio)) return US_EMPTY;
    return CAT(label(t(T_CACHE_HIT_RATE)), US(" "), fixed(fmin(100, fmax(0, jnum(ratio) * 100)), 1), US("%"));
}

// `Started: 2026-10-01 11:00 │ Last reply: 1m ago`
static us session_time_line(void) {
    Transcript *tr = C->t;
    us parts[2];
    int n = 0;
    ltime lt;
    bool started = CF->d.show_session_start && !isnan(tr->session_start);
    if (started && !local_time(tr->session_start, &lt)) UNSUPPORTED("unrepresentable time");
    else if (started) {
        sb b = {0};
        sb_int(&b, lt.y);
        sb_ch(&b, '-'), sb_pad(&b, lt.mo, 2);
        sb_ch(&b, '-'), sb_pad(&b, lt.d, 2);
        sb_ch(&b, ' '), sb_pad(&b, lt.h, 2);
        sb_ch(&b, ':'), sb_pad(&b, lt.mi, 2);
        parts[n++] = CAT(label(CAT(t(T_SESSION_STARTED), US(":"))), US(" "), sb_done(&b));
    }
    if (CF->d.show_last_response && !isnan(tr->last_assistant_at))
        parts[n++] = CAT(label(CAT(t(T_LAST_REPLY), US(":"))), US(" "), format_ago(C->now - tr->last_assistant_at));
    return join(parts, n, CAT(US(" "), label(US("│")), US(" ")));
}

static us format_bytes(double bytes) {
    if (!isfinite(bytes) || bytes <= 0) return US("0 B");
    static const us units[] = {US_INIT("B"), US_INIT("KB"), US_INIT("MB"), US_INIT("GB"), US_INIT("TB")};
    double v = bytes;
    size_t u = 0;
    while (v >= 1024 && u + 1 < countof(units)) v /= 1024, u++;
    return CAT(fixed(v, v >= 10 || u == 0 ? 0 : 1), US(" "), units[u]);
}

static us memory_line(Align a) {
    if (CF->compact || !CF->d.show_memory || !C->has_memory) return US_EMPTY;
    us prefix = bar_label(T_APPROX_RAM, (Align){a.align, true});
    us percent = CAT(quota_color(C->mem_percent, PACE_NONE), num(C->mem_percent), US("%"), RESET);
    return CAT(prefix, US(" "), quota_bar(C->mem_percent, PACE_NONE), US(" "), format_bytes(C->mem_used), US(" / "), format_bytes(C->mem_total),
               US(" ("), percent, US(")"));
}

static us added_dirs_line(void) {
    if (!CF->d.show_added_dirs || !CF->d.added_dirs_line) return US_EMPTY;
    us dirs = added_dirs(US_EMPTY, dim(US(", ")));
    return dirs.n ? CAT(label(US("Added dirs:")), US(" "), dirs) : US_EMPTY;
}

// ---- render/activity.js ----

static us shorten_tool_name(us raw, double max) {
    us name = sanitize_display(raw);
    if (max == 0) return name;
    us shown = name;
    // /^mcp__.+__.+$/ then split('__').pop(); of the line terminators . does not match, only these
    // two outlive sanitizing
    if (us_starts_c(name, "mcp__")) {
        bool term = false;
        for (int32_t i = 0; i < name.n; i++) term |= name.p[i] == 0x2028 || name.p[i] == 0x2029;
        bool m = false;
        if (!term)
            for (int32_t i = 6; i + 2 < name.n && !m; i++) m = name.p[i] == '_' && name.p[i + 1] == '_';
        if (m) {
            int32_t last = -1;
            for (int32_t i = 0; i + 1 < name.n; i++)
                if (name.p[i] == '_' && name.p[i + 1] == '_') last = i, i++;
            shown = us_slice(name, last + 2, name.n);
        }
    }
    if (shown.n <= max) return shown;
    return CAT(us_slice(shown, 0, max - 1 > 0 ? (int32_t)fmin(max - 1, INT32_MAX) : 0), US("…"));
}

static us shorten_path(us target) {
    sb b = {0};
    us s = sanitize_display(target);
    for (int32_t i = 0; i < s.n; i++) sb_ch(&b, s.p[i] == '\\' ? '/' : s.p[i]);
    us n = sb_done(&b);
    if (n.n <= 20) return n;
    int32_t slash = n.n - 1;
    while (slash >= 0 && n.p[slash] != '/') slash--;
    us file = us_slice(n, slash + 1, n.n);
    if (!file.n) file = n;
    return file.n >= 20 ? CAT(us_slice(file, 0, 17), US("...")) : CAT(US(".../"), file);
}

// `◐ Edit: auth.ts | ✓ Read ×3 | +2 more`: the two newest running tools, then completed counts
static us tools_line(void) {
    Transcript *tr = C->t;
    Tool *tools[32];
    int n = 0;
    for (int i = 0; i < tr->ntools; i++)
        if (!(CF->d.show_skills && us_eq_c(tr->tools[i].name, "Skill"))) tools[n++] = &tr->tools[i];
    us parts[40];
    int np = 0;
    Tool *running[32];
    int nr = 0;
    for (int i = 0; i < n; i++)
        if (tools[i]->status == ST_RUNNING) running[nr++] = tools[i];
    for (int i = nr > 2 ? nr - 2 : 0; i < nr; i++) {
        us target = running[i]->target.n ? label(CAT(US(": "), shorten_path(running[i]->target))) : US_EMPTY;
        parts[np++] = CAT(yellow(US("◐")), US(" "), cyan(shorten_tool_name(running[i]->name, CF->d.tool_name_max)), target);
    }
    us names[32];
    int counts[32], nn = 0;
    for (int i = 0; i < n; i++) {
        if (tools[i]->status == ST_RUNNING) continue;
        int k = 0;
        while (k < nn && !us_eq(names[k], tools[i]->name)) k++;
        if (k == nn) names[nn] = tools[i]->name, counts[nn++] = 0;
        counts[k]++;
    }
    for (int i = 1; i < nn; i++) {  // stable sort by count, descending
        us xn = names[i];
        int xc = counts[i], j = i - 1;
        while (j >= 0 && counts[j] < xc) names[j + 1] = names[j], counts[j + 1] = counts[j], j--;
        names[j + 1] = xn, counts[j + 1] = xc;
    }
    double maxv = CF->d.tools_max_visible;
    int visible = maxv == 0 || nn < maxv ? nn : (int)maxv;
    for (int i = 0; i < visible; i++)
        parts[np++] = CAT(green(US("✓")), US(" "), shorten_tool_name(names[i], CF->d.tool_name_max), US(" "), label(CAT(US("×"), num(counts[i]))));
    if (nn > visible) parts[np++] = label(CAT(US("+"), num(nn - visible), US(" more")));
    return join(parts, np, US(" | "));
}

// `✓ Skills (5): a, b, c, d, +1 more`
static us names_line(us title, us *names, int count, double max_visible) {
    us safe[256];
    int n = 0;
    for (int i = 0; i < count && n < 256; i++) {
        us s = us_trim(sanitize_display(names[i]));
        if (s.n) safe[n++] = s.n <= 64 ? s : CAT(us_slice(s, 0, 63), US("…"));
    }
    if (!n) return US_EMPTY;
    int shown = max_visible == 0 || n < max_visible ? n : (int)max_visible;
    us vis[257];
    for (int i = 0; i < shown; i++) vis[i] = cyan(safe[i]);
    int nv = shown;
    if (n > shown) vis[nv++] = label(CAT(US("+"), num(n - shown), US(" more")));
    return CAT(green(US("✓")), US(" "), title, US(" "), label(CAT(US("("), num(n), US(")"))), US(": "), join(vis, nv, US(", ")));
}

static us truncate_string(us s, int max) {
    if (!s.n) return US_EMPTY;
    return s.n <= max ? s : CAT(us_slice(s, 0, max - 3 > 0 ? max - 3 : 0), US("..."));
}

static bool all_digits(us s) {
    if (!s.n) return false;
    for (int32_t i = 0; i < s.n; i++)
        if (s.p[i] < '0' || s.p[i] > '9') return false;
    return true;
}

// `claude-haiku-4-5-20251001` -> `haiku-4.5`; aliases and unknown ids pass through
static bool short_model(us m, us *out) {
    us cleaned = us_trim(sanitize_display(m));
    if (!cleaned.n) return false;
    // /\[[^\]]*\]$/: the leftmost '[' after the last ']' that precedes the final one.
    us cand = cleaned;
    if (cand.n && cand.p[cand.n - 1] == ']') {
        int32_t prev = cand.n - 2;
        while (prev >= 0 && cand.p[prev] != ']') prev--;
        int32_t open = us_find_ch(cand, '[', prev + 1);
        if (open >= 0 && open < cand.n - 1) cand = us_slice(cand, 0, open);
    }
    us low = us_lower(cand);
    if (us_starts_c(low, "claude-")) {
        us rest = us_slice(low, 7, low.n);
        for (size_t f = 0; f < countof(FAMILIES); f++) {  // ^claude-(fam)-(\d+)(?:-(\d+))?(?:-\d{8})?$
            us fam = FAMILIES[f];
            int32_t fl = fam.n;
            if (!us_starts(rest, fam) || rest.n <= fl || rest.p[fl] != '-') continue;
            int32_t j = fl + 1, k = j;
            while (k < rest.n && rest.p[k] >= '0' && rest.p[k] <= '9') k++;
            if (k == j) continue;
            us major = us_slice(cand, 7 + j, 7 + k), tail = us_slice(rest, k, rest.n);
            bool date_only = tail.n == 0 || (tail.n == 9 && tail.p[0] == '-' && all_digits(us_slice(tail, 1, 9)));
            if (tail.n > 1 && tail.p[0] == '-') {
                int32_t m2 = 1;
                while (m2 < tail.n && tail.p[m2] >= '0' && tail.p[m2] <= '9') m2++;
                us after = us_slice(tail, m2, tail.n);
                if (m2 > 1 && (after.n == 0 || (after.n == 9 && after.p[0] == '-' && all_digits(us_slice(after, 1, 9))))) {
                    *out = CAT(fam, US("-"), major, US("."), us_slice(tail, 1, m2));
                    return true;
                }
            }
            if (date_only) {
                *out = CAT(fam, US("-"), major);
                return true;
            }
        }
        // ^claude-(\d+)(?:-(\d+))?-(fam)(?:-\d{8})?$
        int32_t k = 0;
        while (k < rest.n && rest.p[k] >= '0' && rest.p[k] <= '9') k++;
        if (k > 0) {
            us major = us_slice(rest, 0, k);
            for (int pass = 0; pass < 2; pass++) {
                us minor = US_EMPTY;
                int32_t j = k;
                if (pass == 0) {
                    if (j >= rest.n || rest.p[j] != '-') continue;
                    int32_t m2 = j + 1;
                    while (m2 < rest.n && rest.p[m2] >= '0' && rest.p[m2] <= '9') m2++;
                    if (m2 == j + 1) continue;
                    minor = us_slice(rest, j + 1, m2);
                    j = m2;
                }
                if (j >= rest.n || rest.p[j] != '-') continue;
                us tail = us_slice(rest, j + 1, rest.n);
                for (size_t f = 0; f < countof(FAMILIES); f++) {
                    us fam = FAMILIES[f];
                    if (!us_starts(tail, fam)) continue;
                    us after = us_slice(tail, fam.n, tail.n);
                    if (after.n == 0 || (after.n == 9 && after.p[0] == '-' && all_digits(us_slice(after, 1, 9)))) {
                        *out = minor.n ? CAT(fam, US("-"), major, US("."), minor) : CAT(fam, US("-"), major);
                        return true;
                    }
                }
            }
        }
    }
    if (us_eq_c(low, "claude-")) return false;
    *out = cleaned;
    return true;
}

static us agent_elapsed(Agent *a) {
    double ms = fmax(0, (isnan(a->end) ? C->now : a->end) - a->start);
    if (ms < 1000) return US("<1s");
    if (ms < 60000) return CAT(num(js_round(ms / 1000)), US("s"));
    double secs = floor(ms / 1000), mins = floor(secs / 60);
    if (mins < 60) return CAT(num(mins), US("m "), num(fmod(secs, 60)), US("s"));
    return CAT(num(floor(mins / 60)), US("h "), num(fmod(mins, 60)), US("m"));
}

// Up to three agents: running ones first, then those that finished in the last minute.
static us agents_line(void) {
    Transcript *tr = C->t;
    Agent *shown[3], *run[16], *done[16];
    int ns = 0, nr = 0, nd = 0;
    for (int i = 0; i < tr->nagents; i++) {
        Agent *a = &tr->agents[i];
        if (a->status == ST_RUNNING) run[nr++] = a;
        else if (a->status == ST_COMPLETED && isfinite(a->end) && a->end <= C->now && C->now - a->end <= 60000) done[nd++] = a;
    }
    for (int i = nr > 3 ? nr - 3 : 0; i < nr; i++) shown[ns++] = run[i];
    int slots = 3 - ns < 2 ? 3 - ns : 2;
    for (int i = nd > slots ? nd - slots : 0; i < nd && slots > 0; i++) shown[ns++] = done[i];
    us lines[3];
    for (int i = 0; i < ns; i++) {
        Agent *a = shown[i];
        us icon = a->status == ST_RUNNING ? yellow(US("◐")) : green(US("✓"));
        us type = truncate_string(us_trim(sanitize_display(a->type)), 24);
        us model;
        bool hm = a->model.n && short_model(a->model, &model);
        us desc = truncate_string(us_trim(sanitize_display(a->desc)), 40);
        lines[i] = CAT(icon, US(" "), magenta(type.n ? type : US("agent")), hm ? CAT(US(" "), label(CAT(US("["), model, US("]")))) : US_EMPTY,
                       desc.n ? label(CAT(US(": "), desc)) : US_EMPTY, US(" "), label(CAT(US("("), agent_elapsed(a), US(")"))));
    }
    return join(lines, ns, US("\n"));
}

// `▸ Fix the bug (2/5)`, or `✓ All todos complete (5/5)`
static us todos_line(void) {
    Transcript *tr = C->t;
    if (!tr->ntodos) return US_EMPTY;
    int completed = 0;
    Todo *current = nullptr;
    for (int i = 0; i < tr->ntodos; i++) {
        if (tr->todos[i].status == TD_COMPLETED) completed++;
        if (!current && tr->todos[i].status == TD_IN_PROGRESS) current = &tr->todos[i];
    }
    us progress = label(CAT(US("("), num(completed), US("/"), num(tr->ntodos), US(")")));
    if (current) return CAT(yellow(US("▸")), US(" "), truncate_string(sanitize_display(current->content), 50), US(" "), progress);
    return completed == tr->ntodos ? CAT(green(US("✓")), US(" "), t(T_ALL_TODOS), US(" "), progress) : US_EMPTY;
}

static us activity_line(int el) {
    Transcript *tr = C->t;
    switch (el) {
    case EL_TOOLS: return CF->d.show_tools ? tools_line() : US_EMPTY;
    case EL_SKILLS: return CF->d.show_skills ? names_line(US("Skills"), tr->skills, tr->nskills, CF->d.skills_max_visible) : US_EMPTY;
    case EL_MCP: return CF->d.show_mcp ? names_line(US("MCPs"), tr->mcp_servers, tr->nmcp, 4) : US_EMPTY;
    case EL_AGENTS: return CF->d.show_agents ? agents_line() : US_EMPTY;
    case EL_TODOS: return CF->d.show_todos ? todos_line() : US_EMPTY;
    }
    return US_EMPTY;
}

static bool is_activity(int el) { return el == EL_TOOLS || el == EL_SKILLS || el == EL_MCP || el == EL_AGENTS || el == EL_TODOS; }

// ---- render/expanded.js ----

static us project_line(void) {
    KPart parts[MAX_PARTS];
    int n = 0;
#define ADD(text, key)                                  \
    do {                                                \
        us x_ = (text);                                 \
        if (x_.n) parts[n++] = (KPart){(key), x_};      \
    } while (0)
    ADD(custom_line_part(true), -1);
    if (CF->d.show_model) ADD(model_badge(), SEG_MODEL);
    us pp[2];
    int npp = project_parts(true, pp);
    for (int i = 0; i < npp; i++) ADD(pp[i], SEG_PROJECT);
    ADD(advisor_part(), SEG_ADVISOR);
    ADD(session_name_part(), SEG_SESSION_NAME);
    ADD(version_part(), SEG_VERSION);
    ADD(duration_part(), SEG_DURATION);
    ADD(cost_part(), SEG_COST);
    ADD(speed_part(), SEG_SPEED);
    ADD(auth_part(), SEG_AUTH);
    ADD(custom_line_part(false), -1);
#undef ADD
    return n ? order_parts(parts, n, US(" │ ")) : US_EMPTY;
}

static us element_line(int el, Align a) {
    us parts[16];
    switch (el) {
    case EL_PROJECT: return project_line();
    case EL_ADDED_DIRS: return added_dirs_line();
    case EL_CONTEXT: return context_line(a);
    case EL_USAGE: {
        int n = usage_parts(false, a, parts);
        return join(parts, n, US(" | "));
    }
    case EL_PROMPT_CACHE: return prompt_cache_line();
    case EL_CACHE_HIT_RATE: return cache_hit_rate_line();
    case EL_MEMORY: return memory_line(a);
    case EL_ENVIRONMENT: return environment_line();
    case EL_SESSION_TIME: return session_time_line();
    }
    return activity_line(el);
}

typedef struct { us line; bool activity; } Row;

static int expanded_lines(us *lines) {
    int group_of[EL_COUNT];
    for (int e = 0; e < EL_COUNT; e++) group_of[e] = -1;
    for (int g = CF->ngroups - 1; g >= 0; g--)
        for (int k = 0; k < CF->group_len[g]; k++) group_of[CF->group[g][k]] = g;
    bool in_order[EL_COUNT] = {0};
    for (int i = 0; i < CF->nelem; i++) in_order[CF->elem[i]] = true;
    // A visible memory bar widens the label column of the other bars so they line up.
    us tmp[16];
    bool memory_visible = in_order[EL_MEMORY] && CF->d.show_memory && C->has_memory;
    bool other_bar = in_order[EL_CONTEXT] || (in_order[EL_USAGE] && usage_parts(false, (Align){0}, tmp) > 0);
    Align separate = memory_visible && other_bar ? (Align){true, true} : (Align){0};
    Align stacked = {true, memory_visible};
    Row rows[64];
    int nr = 0;
    bool seen[EL_COUNT] = {0};
    for (int idx = 0; idx < CF->nelem; idx++) {
        int el = CF->elem[idx];
        if (seen[el]) continue;
        int g = group_of[el];
        int seq[EL_COUNT], ns = 0;
        for (int j = idx; g >= 0 && j < CF->nelem; j++) {
            int e2 = CF->elem[j];
            if (group_of[e2] != g || seen[e2]) break;
            seq[ns++] = e2;
        }
        if (ns <= 1) {
            seen[el] = true;
            us l = element_line(el, separate);
            if (l.n) rows[nr++] = (Row){l, is_activity(el)};
            continue;
        }
        idx += ns - 1;
        bool has_memory = false;
        for (int k = 0; k < ns; k++) seen[seq[k]] = true, has_memory |= seq[k] == EL_MEMORY;
        // Memory only widens this row's labels when its own bar is on a separate row.
        Align group_align = memory_visible && !has_memory ? separate : (Align){0};
        struct { int el; us line; } en[EL_COUNT];
        int ne = 0;
        for (int k = 0; k < ns; k++) {
            us l = element_line(seq[k], group_align);
            if (l.n) en[ne].el = seq[k], en[ne++].line = l;
        }
        if (ne == 1) {
            us l = element_line(en[0].el, separate);
            rows[nr++] = (Row){l.n ? l : en[0].line, is_activity(en[0].el)};
            continue;
        }
        if (ne == 0) continue;
        us ls[EL_COUNT];
        bool act = false;
        for (int k = 0; k < ne; k++) ls[k] = en[k].line, act |= is_activity(en[k].el);
        us combined = join(ls, ne, US(" │ "));
        if (C->width < 0 || visible_width(combined) <= C->width) {
            us aligned = US_EMPTY;
            int split = -1;
            for (int k = 0; k < ne && split < 0; k++)
                if (CF->right_align[en[k].el]) split = k;
            // Callers only align rows that fit, so the gap is at least 3.
            if (C->width >= 0 && split > 0) {
                us left = join(ls, split, US(" │ ")), right = join(ls + split, ne - split, US(" │ "));
                aligned = CAT(left, repeat(US(" "), C->width - visible_width(left) - visible_width(right)), right);
            }
            rows[nr++] = (Row){aligned.n ? aligned : combined, act};
        } else {
            for (int k = 0; k < ne; k++) {
                us l = element_line(en[k].el, stacked);
                rows[nr++] = (Row){l.n ? l : en[k].line, is_activity(en[k].el)};
            }
        }
    }
    int n = 0;
    for (int i = 0; i < nr; i++) lines[n++] = rows[i].line;
    us gf = git_files_line();
    if (gf.n) lines[n++] = gf;
    if (CF->d.show_session_tokens) {
        us st = session_tokens_summary(t(T_TOKENS));
        if (st.n) lines[n++] = st;
    }
    us cp = compactions_part();
    if (cp.n) lines[n++] = cp;
    int first = -1;
    for (int i = 0; i < nr && first < 0; i++)
        if (rows[i].activity) first = i;
    if (CF->show_separators && first > 0) {
        int widest = 20;
        for (int i = 0; i < first; i++) {
            int w = visible_width(rows[i].line);
            if (w > widest) widest = w;
        }
        memmove(lines + first + 1, lines + first, sizeof(us) * (size_t)(n - first));
        lines[first] = separator_line(C->width > 0 && C->width < widest ? C->width : widest);
        n++;
    }
    return n;
}

// ---- render/compact.js ----

static int compact_lines(us *lines) {
    KPart parts[MAX_PARTS];
    int n = 0;
#define ADD(text, key)                                  \
    do {                                                \
        us x_ = (text);                                 \
        if (x_.n) parts[n++] = (KPart){(key), x_};      \
    } while (0)
    ADD(custom_line_part(true), -1);
    // The context bar rides with the model badge, so the cluster moves as the 'model' segment.
    us bar, value, cluster[3];
    int nc = 0;
    context_bar_and_value(&bar, &value);
    if (CF->d.show_model) cluster[nc++] = model_badge();
    if (bar.n) cluster[nc++] = bar;
    cluster[nc++] = value;
    ADD(join(cluster, nc, US(" ")), SEG_MODEL);
    us pp[2];
    int npp = project_parts(false, pp);
    for (int i = 0; i < npp; i++) ADD(pp[i], SEG_PROJECT);
    ADD(session_name_part(), SEG_SESSION_NAME);
    ADD(version_part(), SEG_VERSION);
    us counts[8];
    int ncnt = config_count_parts(US_EMPTY, counts);
    for (int i = 0; i < ncnt; i++) ADD(counts[i], -1);
    us up[16];
    int nup = usage_parts(true, (Align){0}, up);
    for (int i = 0; i < nup; i++) ADD(up[i], -1);
    if (CF->d.show_session_tokens) ADD(session_tokens_summary(CAT(t(T_TOK), US(":"))), -1);
    ADD(compactions_part(), -1);
    ADD(advisor_part(), SEG_ADVISOR);
    ADD(duration_part(), SEG_DURATION);
    ADD(session_time_line(), -1);
    ADD(prompt_cache_line(), -1);
    ADD(cache_hit_rate_line(), -1);
    ADD(cost_part(), SEG_COST);
    ADD(speed_part(), SEG_SPEED);
    ADD(auth_part(), SEG_AUTH);
    ADD(custom_line_part(false), -1);
#undef ADD
    us header = CAT(order_parts(parts, n, US(" | ")), token_breakdown());
    int nl = 0;
    lines[nl++] = header;
    static const int order[] = {EL_TOOLS, EL_SKILLS, EL_MCP, EL_AGENTS, EL_TODOS};
    us act[countof(order)];
    int na = 0;
    for (size_t i = 0; i < countof(order); i++) {
        us l = activity_line(order[i]);
        if (l.n) act[na++] = l;
    }
    if (CF->show_separators && na > 0) {
        int widest = visible_width(header);
        if (widest < 20) widest = 20;
        lines[nl++] = separator_line(C->width > 0 && C->width < widest ? C->width : widest);
    }
    for (int i = 0; i < na; i++) lines[nl++] = act[i];
    return nl;
}

// ---- render/index.js ----

void render(Ctx *ctx, sb *out) {
    C = ctx, CF = ctx->cfg;
    us lines[128];
    int n = CF->compact ? compact_lines(lines) : expanded_lines(lines);
    int width = ctx->width >= 0 ? ctx->width : 0;
    for (int i = 0; i < n; i++) {
        us l = lines[i];
        for (int32_t a = 0;;) {
            int32_t nl = us_find_ch(l, '\n', a);
            us phys = us_slice(l, a, nl < 0 ? l.n : nl);
            // wrap_to_width gives at most one row per part, and a line has at most its length + 2.
            us *vis = arena_alloc(sizeof(us) * (size_t)(phys.n + 2));
            int nv = 0;
            wrap_to_width(phys, width, vis, &nv);
            for (int k = 0; k < nv; k++) {
                sb_us(out, RESET);
                sb_us(out, vis[k]);
                sb_ch(out, '\n');
            }
            if (nl < 0) break;
            a = nl + 1;
        }
    }
}
