// Port of claude-hud config.ts loadConfigFiles(): the two config files, merged, migrated and
// normalized against the defaults, with what they hold that is ignored or changed, plus the color
// values resolved the way render/colors.ts does.
#include <math.h>
#include <string.h>

#include "hud.h"
#include "utf8proc.h"

static jv BASE, OVR;

// hasSafeConfigShape: any value nested deeper than 8 fails, primitives included.
static bool safe_shape(jv v, int depth) {
    if (depth > 8) return false;
    size_t idx, max;
    jv k, c;
    if (jis_arr(v)) {
        yyjson_arr_foreach(v, idx, max, c) if (!safe_shape(c, depth + 1)) return false;
    } else if (jis_obj(v)) {
        yyjson_obj_foreach(v, idx, max, k, c) {
            if (jstr_eq(k, "__proto__") || jstr_eq(k, "prototype") || jstr_eq(k, "constructor")) return false;
            if (!safe_shape(c, depth + 1)) return false;
        }
    }
    return true;
}

static void problem(Config *c, us text) {
    if (!c->nproblems++) c->problem = text;
}

// readConfigFile: null when the file is missing; why, as a problem, when it cannot be used. Only a regular
// file is opened: a FIFO would wait for a writer.
static jv read_config_file(Config *c, us path, const char *name) {
    fstat_t st = stat_path(path);
    const char *why = nullptr;
    jv v = nullptr;
    void *h = nullptr;
    if (!st.ok) return nullptr;
    if (!st.is_file) why = "is not a regular file";
    else if (!(h = open_read(path))) why = "can't be read";
    else if (st.size > 64 * 1024) why = "is larger than 64 KB";
    else {
        size_t n;
        char *p = read_rest(h, 0, &n);
        if (!p) why = "can't be read";
        else if (!(v = parse_json(p, n))) why = "is not valid JSON";
        else if (!jis_obj(v) || !safe_shape(v, 0)) why = "is not a JSON object (or nests too deep, or uses __proto__)", v = nullptr;
    }
    if (h) os_close(h);
    if (why) problem(c, us_cat(us_c(name), US(" "), us_c(why)));
    return v;
}

// mergeOverrides: sections merge key by key, anything else in the override replaces.
static jv top(const char *a) {
    jv o = jget(OVR, a);
    return o ? o : jget(BASE, a);
}

// A section's two sides, looked up once rather than for every key; base is null when the
// override replaces the section.
typedef struct { jv over, base; } Section;

static Section section(const char *a) {
    jv oa = jget(OVR, a), ba = jget(BASE, a);
    return oa && !(jis_obj(oa) && jis_obj(ba)) ? (Section){oa, nullptr} : (Section){oa, ba};
}

static jv get(Section s, const char *b) {
    jv v = jget(s.over, b);
    return v ? v : jget(s.base, b);
}

static bool is_integer(jv v) { return is_finite_num(v) && jnum(v) == trunc(jnum(v)); }

static bool boolean(jv v, bool def) { return v && yyjson_is_bool(v) ? yyjson_get_bool(v) : def; }

static int index_of(jv v, const char *const *names, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (jstr_eq(v, names[i])) return (int)i;
    return -1;
}

// The position of v among the names of an array, else def.
#define ONE_OF(v, names, def) one_of((v), (names), countof(names), (def))
static int one_of(jv v, const char *const *names, size_t count, int def) {
    int i = index_of(v, names, count);
    return i >= 0 ? i : def;
}

static double clamp100(jv v, double def) { return is_finite_num(v) ? fmax(0, fmin(100, jnum(v))) : def; }
static double floor0(jv v, double def) { return is_finite_num(v) ? fmax(0, floor(jnum(v))) : def; }
static double count(jv v, double def) { return is_integer(v) && jnum(v) >= 0 ? jnum(v) : def; }
static us text(jv v, int max) { return jis_str(v) ? truncate_string(sanitize_display(jstr(v)), max, US_EMPTY) : US_EMPTY; }

// usagePath: a leading ~ and ${VAR} expanded; unset variables are left as written.
static us usage_path(jv v, us home) {
    if (!jis_str(v)) return US_EMPTY;
    us s = us_trim(jstr(v));
    if (us_eq_c(s, "~")) s = home;
    else if (us_starts_c(s, "~/") || us_starts_c(s, "~\\")) s = path_join(home, us_slice(s, 2, s.n));
    sb b = {0};
    for (int32_t i = 0; i < s.n; i++) {
        if (s.p[i] == '$' && i + 2 < s.n && s.p[i + 1] == '{') {
            uint16_t c0 = s.p[i + 2];
            bool start = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_';
            int32_t j = i + 2;
            while (start && j < s.n && ((s.p[j] >= 'A' && s.p[j] <= 'Z') || (s.p[j] >= 'a' && s.p[j] <= 'z') || (s.p[j] >= '0' && s.p[j] <= '9') || s.p[j] == '_')) j++;
            if (start && j < s.n && s.p[j] == '}') {
                size_t n;
                char *name = us_to_utf8(us_slice(s, i + 2, j), &n);
                if (env_has(name)) {
                    sb_us(&b, env_get(name));
                    i = j;
                    continue;
                }
            }
        }
        sb_ch(&b, s.p[i]);
    }
    return sb_done(&b);
}

// In enum order; the asserts catch a name missing from either side.
static const char *const EL_NAMES[] = {"project", "addedDirs", "context", "usage", "promptCache", "cacheHitRate", "memory",
                                       "environment", "tools", "skills", "mcp", "agents", "todos", "sessionTime", "sessionTokens", "cost"};
static const char *const SEG_NAMES[] = {"model", "project", "advisor", "sessionName", "version",
                                        "extra", "duration", "cost", "speed", "auth"};
static_assert(countof(EL_NAMES) == EL_COUNT);
static_assert(countof(SEG_NAMES) == SEG_COUNT);

// names(): known names once each, in order. `known` is one of the two tables above.
static int names(jv v, const char *const *known, size_t nknown, int *out) {
    int n = 0;
    bool seen[EL_COUNT > SEG_COUNT ? EL_COUNT : SEG_COUNT] = {0};
    size_t idx, max;
    jv x;
    yyjson_arr_foreach(v, idx, max, x) {
        int i = index_of(x, known, nknown);
        if (i >= 0 && !seen[i]) seen[i] = true, out[n++] = i;
    }
    return n;
}

static int grapheme_count(us s) {
    int n = 0;
    utf8proc_int32_t state = 0;
    int32_t l;
    uint32_t prev = 0;
    for (int32_t i = 0; i < s.n; i += l) {
        uint32_t c = us_cp_at(s, i, &l);
        if (i == 0 || utf8proc_grapheme_break_stateful((utf8proc_int32_t)prev, (utf8proc_int32_t)c, &state)) n++;
        prev = c;
    }
    return n;
}

// barChar: exactly one visible grapheme
static us bar_char(jv v, us def) {
    if (!jis_str(v) || !yyjson_get_len(v)) return def;
    us s = jstr(v);
    if (grapheme_count(s) != 1) return def;
    int32_t l;
    for (int32_t i = 0; i < s.n; i += l) {
        uint32_t c = us_cp_at(s, i, &l);
        utf8proc_category_t k = utf8proc_category((utf8proc_int32_t)c);
        bool vs = (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) || (c >= 0x180B && c <= 0x180D) || c == 0x180F;
        if (k == UTF8PROC_CATEGORY_CC || k == UTF8PROC_CATEGORY_CF || k == UTF8PROC_CATEGORY_ZL || k == UTF8PROC_CATEGORY_ZP ||
            k == UTF8PROC_CATEGORY_CN || vs)
            return def;
    }
    return s;
}

static const char *const COLOR_NAMES[] = {"dim", "red", "green", "yellow", "magenta", "cyan", "brightBlue", "brightMagenta"};

// The color rule, then resolveAnsi: a name, a 256-color index, or #rrggbb.
static us color_of(jv v, us def) {
    static const us ansi[] = {US_INIT("\x1b[2m"),  US_INIT("\x1b[31m"), US_INIT("\x1b[32m"), US_INIT("\x1b[33m"),
                              US_INIT("\x1b[35m"), US_INIT("\x1b[36m"), US_INIT("\x1b[94m"), US_INIT("\x1b[95m")};
    static_assert(countof(COLOR_NAMES) == countof(ansi));
    int i = index_of(v, COLOR_NAMES, countof(COLOR_NAMES));
    if (i >= 0) return ansi[i];
    sb b = {0};
    if (is_integer(v) && jnum(v) >= 0 && jnum(v) <= 255) {
        sb_us(&b, US("\x1b[38;5;"));
        sb_int(&b, (int64_t)jnum(v));
        sb_ch(&b, 'm');
        return sb_done(&b);
    }
    if (jis_str(v) && yyjson_get_len(v) == 7) {
        const char *s = yyjson_get_str(v);
        int x[6];
        bool ok = s[0] == '#';
        for (int i = 0; i < 6 && ok; i++) {
            char c = s[i + 1];
            x[i] = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
            ok = x[i] >= 0;
        }
        if (ok) {
            sb_us(&b, US("\x1b[38;2;"));
            for (int i = 0; i < 6; i += 2) {
                if (i) sb_ch(&b, ';');
                sb_int(&b, x[i] * 16 + x[i + 1]);
            }
            sb_ch(&b, 'm');
            return sb_done(&b);
        }
    }
    return def;
}

static us color(Section colors, const char *key, us def) { return color_of(get(colors, key), def); }

static const char *const LANGS[] = {"en", "zh", "zh-Hans", "zh-Hant", "zh-TW"};
static const char *const LAYOUTS[] = {"compact", "expanded"};
static const char *const BRANCH_OVERFLOW[] = {"truncate", "wrap"};
static const char *const INLINE_LINE[] = {"inline", "line"};
static const char *const CONTEXT_VALUES[] = {"percent", "tokens", "remaining", "both"};
static const char *const USAGE_VALUES[] = {"percent", "remaining"};
static const char *const EFFORT_FORMATS[] = {"full", "symbol", "text"};
static const char *const MODEL_FORMATS[] = {"full", "compact", "short"};
static const char *const MODEL_SOURCES[] = {"auto", "stdin", "transcript"};
static const char *const TIME_FORMATS[] = {"relative", "absolute", "both", "elapsed", "elapsedAndAbsolute"};
static const char *const HOUR_CYCLES[] = {"auto", "h11", "h12", "h23", "h24"};
static const char *const POSITIONS[] = {"first", "last"};

// ---- configProblems: each key a file sets whose value a rule would ignore or change ----

enum { R_BOOL, R_ONE_OF, R_LEVELS, R_MAX_WIDTH, R_POSITIVE, R_WHOLE, R_PERCENT, R_PATH, R_TEXT, R_COLOR, R_BAR,
       R_ELEMENTS, R_ELEMENTS_SOME, R_SEGMENTS, R_GROUPS };
typedef struct { const char *sec, *key; uint8_t rule, n; const char *const *names; } Key;
#define K(sec, key, rule) {sec, key, rule, 0, nullptr}
#define ONE(sec, key, names) {sec, key, R_ONE_OF, countof(names), names}
#define TEXT(sec, key, max) {sec, key, R_TEXT, max, nullptr}

// Every key of DEFAULT_CONFIG, in its order; null sec is the top level.
static const Key KEYS[] = {
    ONE(nullptr, "language", LANGS), ONE(nullptr, "lineLayout", LAYOUTS), K(nullptr, "showSeparators", R_BOOL),
    K(nullptr, "pathLevels", R_LEVELS), K(nullptr, "maxWidth", R_MAX_WIDTH), K(nullptr, "forceMaxWidth", R_BOOL),
    K(nullptr, "elementOrder", R_ELEMENTS_SOME), K(nullptr, "projectLineOrder", R_SEGMENTS),
    K("gitStatus", "enabled", R_BOOL), K("gitStatus", "showDirty", R_BOOL), K("gitStatus", "showAheadBehind", R_BOOL),
    K("gitStatus", "showFileStats", R_BOOL), K("gitStatus", "showWorktree", R_BOOL), ONE("gitStatus", "branchOverflow", BRANCH_OVERFLOW),
    K("gitStatus", "pushWarningThreshold", R_WHOLE), K("gitStatus", "pushCriticalThreshold", R_WHOLE),
    K("jjStatus", "enabled", R_BOOL), K("jjStatus", "showDirty", R_BOOL), K("jjStatus", "showConflicts", R_BOOL),
    K("display", "showModel", R_BOOL), K("display", "showProject", R_BOOL), K("display", "showLaunchDir", R_BOOL),
    K("display", "showAddedDirs", R_BOOL), ONE("display", "addedDirsLayout", INLINE_LINE), K("display", "showContextBar", R_BOOL),
    ONE("display", "contextValue", CONTEXT_VALUES), K("display", "showConfigCounts", R_BOOL), K("display", "showCost", R_BOOL),
    K("display", "showRoutedCost", R_BOOL), K("display", "showDailyCost", R_BOOL), K("display", "showWeeklyCost", R_BOOL),
    K("display", "showDuration", R_BOOL), K("display", "showSpeed", R_BOOL), K("display", "showTokenBreakdown", R_BOOL),
    K("display", "showUsage", R_BOOL), ONE("display", "usageValue", USAGE_VALUES), K("display", "usageBarEnabled", R_BOOL),
    K("display", "showResetLabel", R_BOOL), K("display", "usageCompact", R_BOOL), K("display", "showModelScopedUsage", R_BOOL),
    K("display", "usagePace", R_BOOL), K("display", "showTools", R_BOOL), K("display", "showSkills", R_BOOL),
    K("display", "showMcp", R_BOOL), K("display", "toolNameMaxLength", R_WHOLE), K("display", "toolsMaxVisible", R_WHOLE),
    K("display", "skillsMaxVisible", R_WHOLE), K("display", "showAgents", R_BOOL), K("display", "showTodos", R_BOOL),
    K("display", "showSessionName", R_BOOL), K("display", "showAuth", R_BOOL), K("display", "showAuthUser", R_BOOL),
    K("display", "authUserLength", R_WHOLE), K("display", "showClaudeCodeVersion", R_BOOL), K("display", "showEffortLevel", R_BOOL),
    ONE("display", "effortFormat", EFFORT_FORMATS), K("display", "showAgentEffort", R_BOOL), K("display", "mergeGroups", R_GROUPS),
    K("display", "rightAlign", R_ELEMENTS), K("display", "showMemoryUsage", R_BOOL), K("display", "showPromptCache", R_BOOL),
    K("display", "showCacheHitRate", R_BOOL), K("display", "showSessionTokens", R_BOOL), K("display", "showOutputStyle", R_BOOL),
    K("display", "showSessionStartDate", R_BOOL), K("display", "showLastResponseAt", R_BOOL), K("display", "showCompactions", R_BOOL),
    K("display", "contextWarningThreshold", R_PERCENT), K("display", "contextCriticalThreshold", R_PERCENT),
    K("display", "usageThreshold", R_PERCENT), K("display", "sevenDayThreshold", R_PERCENT), K("display", "environmentThreshold", R_PERCENT),
    K("display", "externalUsagePath", R_PATH), K("display", "externalUsageWritePath", R_PATH),
    K("display", "externalUsageFreshnessMs", R_WHOLE), ONE("display", "modelFormat", MODEL_FORMATS), TEXT("display", "modelOverride", 80),
    ONE("display", "modelSource", MODEL_SOURCES), K("display", "showProvider", R_BOOL), TEXT("display", "providerName", 40),
    TEXT("display", "customLine", 80), ONE("display", "customLinePosition", POSITIONS), ONE("display", "timeFormat", TIME_FORMATS),
    ONE("display", "hourCycle", HOUR_CYCLES), K("display", "showClockSeconds", R_BOOL), K("display", "showAdvisor", R_BOOL),
    TEXT("display", "advisorOverride", 80), K("display", "autoCompactWindow", R_POSITIVE),
    K("colors", "context", R_COLOR), K("colors", "usage", R_COLOR), K("colors", "warning", R_COLOR), K("colors", "usageWarning", R_COLOR),
    K("colors", "critical", R_COLOR), K("colors", "model", R_COLOR), K("colors", "project", R_COLOR), K("colors", "git", R_COLOR),
    K("colors", "gitBranch", R_COLOR), K("colors", "label", R_COLOR), K("colors", "custom", R_COLOR), K("colors", "barFilled", R_BAR),
    K("colors", "barEmpty", R_BAR),
};
#undef K
#undef ONE
#undef TEXT
static const char *const SECTIONS[] = {"gitStatus", "jjStatus", "display", "colors"};

static const Key *key_in(const char *sec, const char *s, size_t n) {
    for (size_t i = 0; i < countof(KEYS); i++)
        if ((KEYS[i].sec == sec || (sec && KEYS[i].sec && !strcmp(KEYS[i].sec, sec))) && strlen(KEYS[i].key) == n && !memcmp(KEYS[i].key, s, n))
            return &KEYS[i];
    return nullptr;
}

static const Key *key_of(const char *sec, jv k) { return key_in(sec, yyjson_get_str(k), yyjson_get_len(k)); }

// keyEntry(prefix + key) at the top level: a key spelled <Section>.<Key> names that key as well.
static const Key *dotted_key(jv k) {
    const char *s = yyjson_get_str(k), *dot = memchr(s, '.', yyjson_get_len(k));
    if (!dot) return nullptr;
    for (size_t i = 0; i < countof(SECTIONS); i++)
        if ((size_t)(dot - s) == strlen(SECTIONS[i]) && !memcmp(s, SECTIONS[i], (size_t)(dot - s)))
            return key_in(SECTIONS[i], dot + 1, yyjson_get_len(k) - (size_t)(dot - s) - 1);
    return nullptr;
}

// Each element once, all of them known; at least one where the rule falls back on an empty list.
static bool known_once(jv v, const char *const *known, size_t nknown, bool some) {
    if (!jis_arr(v) || (some && !yyjson_arr_size(v))) return false;
    bool seen[EL_COUNT > SEG_COUNT ? EL_COUNT : SEG_COUNT] = {0};
    size_t idx, max;
    jv x;
    yyjson_arr_foreach(v, idx, max, x) {
        int i = index_of(x, known, nknown);
        if (i < 0 || seen[i]) return false;
        seen[i] = true;
    }
    return true;
}

// mergeGroups keeps every group: each a list of two or more known elements, none in an earlier group.
static bool groups_kept(jv v) {
    if (!jis_arr(v)) return false;
    bool used[EL_COUNT] = {0};
    size_t gi, gmax;
    jv group;
    yyjson_arr_foreach(v, gi, gmax, group) {
        if (!jis_arr(group) || yyjson_arr_size(group) < 2) return false;
        size_t idx, max;
        jv x;
        yyjson_arr_foreach(group, idx, max, x) {
            int e = index_of(x, EL_NAMES, EL_COUNT);
            if (e < 0 || used[e]) return false;
            used[e] = true;
        }
    }
    return true;
}

// checkValue: the rule gives the value back unchanged (usagePath: any string).
static bool valid(const Key *k, jv v) {
    switch (k->rule) {
    case R_BOOL: return yyjson_is_bool(v);
    case R_ONE_OF: return index_of(v, k->names, k->n) >= 0;
    case R_LEVELS: return jstr_eq(v, "full") || (jis_num(v) && (jnum(v) == 1 || jnum(v) == 2 || jnum(v) == 3));
    case R_MAX_WIDTH: return yyjson_is_null(v) || (is_integer(v) && jnum(v) > 0 && jnum(v) <= 1000);
    case R_POSITIVE: return yyjson_is_null(v) || (is_integer(v) && jnum(v) > 0);
    case R_WHOLE: return is_integer(v) && jnum(v) >= 0;
    case R_PERCENT: return is_finite_num(v) && jnum(v) >= 0 && jnum(v) <= 100;
    case R_PATH: return jis_str(v);
    case R_TEXT: return jis_str(v) && us_eq(text(v, k->n), jstr(v));
    case R_COLOR: return color_of(v, US_EMPTY).n > 0;
    case R_BAR: return bar_char(v, US_EMPTY).n > 0;
    case R_ELEMENTS: return known_once(v, EL_NAMES, EL_COUNT, false);
    case R_ELEMENTS_SOME: return known_once(v, EL_NAMES, EL_COUNT, true);
    case R_SEGMENTS: return known_once(v, SEG_NAMES, SEG_COUNT, false);
    default: return groups_kept(v);
    }
}

static void list_of(sb *b, const char *const *names, size_t n) {  // each as JSON, joined by " | "
    for (size_t i = 0; i < n; i++) {
        if (i) sb_us(b, US(" | "));
        sb_ch(b, '"'), sb_c(b, names[i]), sb_ch(b, '"');
    }
}

// The rule's `accepts`: what it takes, in words.
static void accepts(sb *b, const Key *k) {
    switch (k->rule) {
    case R_BOOL: return sb_us(b, US("true | false"));
    case R_ONE_OF: return list_of(b, k->names, k->n);
    case R_LEVELS: return sb_us(b, US("1 | 2 | 3 | \"full\""));
    case R_MAX_WIDTH: return sb_us(b, US("a positive number, or null"));
    case R_POSITIVE: return sb_us(b, US("a positive whole number, or null"));
    case R_WHOLE: return sb_us(b, US("a whole number, 0 or more"));
    case R_PERCENT: return sb_us(b, US("a number from 0 to 100"));
    case R_PATH: return sb_us(b, US("an absolute path (a leading ~ and ${VAR} are expanded)"));
    case R_TEXT: return sb_us(b, US("text, up to ")), sb_int(b, k->n), sb_us(b, US(" characters"));
    case R_COLOR: return list_of(b, COLOR_NAMES, countof(COLOR_NAMES)), sb_us(b, US(", a 256-color number 0-255, or \"#rrggbb\""));
    case R_BAR: return sb_us(b, US("one visible character"));
    case R_SEGMENTS: return sb_us(b, US("a list of ")), list_of(b, SEG_NAMES, SEG_COUNT);
    case R_GROUPS: sb_us(b, US("a list of groups, each 2 or more of ")), list_of(b, EL_NAMES, EL_COUNT);
        return sb_us(b, US(", each element in one group"));
    default: return sb_us(b, US("a list of ")), list_of(b, EL_NAMES, EL_COUNT);
    }
}

static void check(Config *c, const char *file, const Key *k, jv v) {
    if (valid(k, v)) return;
    sb b = {0};
    sb_c(&b, file), sb_us(&b, US(": "));
    if (k->sec) sb_c(&b, k->sec), sb_ch(&b, '.');
    sb_c(&b, k->key), sb_us(&b, US(" must be "));
    accepts(&b, k);
    problem(c, sb_done(&b));
}

static const Key *top_key(const char *key) {
    for (size_t i = 0; i < countof(KEYS); i++)
        if (!KEYS[i].sec && !strcmp(KEYS[i].key, key)) return &KEYS[i];
    return nullptr;
}

static bool key_is(jv k, const char *name) { return yyjson_get_len(k) == strlen(name) && !memcmp(yyjson_get_str(k), name, strlen(name)); }

// configProblems over the file as migrateLegacyLayout leaves it: `layout` gone, the boolean it implies for
// showSeparators always valid, and lineLayout and pathLevels from a layout object checked where they land.
// Keys of a section that are not config keys, and other unknown keys, are left alone.
static void file_problems(Config *c, const char *file, jv raw) {
    if (!raw) return;
    jv layout = jget(raw, "layout");
    bool legacy = layout && !jget(raw, "lineLayout");
    jv lobj = legacy && jis_obj(layout) ? layout : nullptr;
    jv lsep = jget(lobj, "showSeparators"), lpl = jget(lobj, "pathLevels"), lll = jget(lobj, "lineLayout");
    bool sep_set = legacy && (jis_str(layout) || (lsep && yyjson_is_bool(lsep)));
    bool pl_set = lobj && (jis_num(lpl) || jstr_eq(lpl, "full")), pl_seen = false;
    size_t idx, max;
    jv k, v;
    yyjson_obj_foreach(raw, idx, max, k, v) {
        if ((legacy && key_is(k, "layout")) || (sep_set && key_is(k, "showSeparators"))) continue;
        if (pl_set && key_is(k, "pathLevels")) v = lpl, pl_seen = true;
        const Key *key = key_of(nullptr, k);
        if (!key) key = dotted_key(k);
        if (key) {
            check(c, file, key, v);
            continue;
        }
        for (size_t i = 0; i < countof(SECTIONS); i++) {
            if (!key_is(k, SECTIONS[i])) continue;
            if (!jis_obj(v)) {
                problem(c, us_cat(us_c(file), US(": "), us_c(SECTIONS[i]), US(" must be an object")));
                break;
            }
            size_t si, smax;
            jv sk, sv;
            yyjson_obj_foreach(v, si, smax, sk, sv) {
                const Key *skey = key_of(SECTIONS[i], sk);
                if (skey) check(c, file, skey, sv);
            }
        }
    }
    if (lobj && jis_str(lll)) check(c, file, top_key("lineLayout"), lll);
    if (pl_set && !pl_seen) check(c, file, top_key("pathLevels"), lpl);
}

void load_config(Config *c, us claude_dir, us home) {
    *c = (Config){0};
    BASE = read_config_file(c, path_join(claude_dir, US("plugins"), US("claude-hud"), US("config.json")), "config.json");
    file_problems(c, "config.json", BASE);
    OVR = read_config_file(c, path_join(claude_dir, US("claude-hud.json")), "claude-hud.json");
    file_problems(c, "claude-hud.json", OVR);

    // migrateLegacyLayout: v0.0.x's `layout`, a string or an object, when lineLayout is absent
    jv line_layout = top("lineLayout"), separators = top("showSeparators"), levels = top("pathLevels");
    Section lay = section("layout");
    jv layout = top("layout");
    bool legacy_compact = false;
    if (layout && !line_layout) {
        if (jis_str(layout)) {
            legacy_compact = true;
            separators = nullptr;
            c->show_separators = jstr_eq(layout, "separators");
        } else if (jis_obj(layout)) {
            jv ll = get(lay, "lineLayout"), ss = get(lay, "showSeparators"), pl = get(lay, "pathLevels");
            if (jis_str(ll)) line_layout = ll;
            if (ss && yyjson_is_bool(ss)) separators = ss;
            if (jis_num(pl) || jstr_eq(pl, "full")) levels = pl;
        }
    }

    if (ONE_OF(top("language"), LANGS, 0) != 0) UNSUPPORTED("non-English UI language");
    c->lang = LANG_EN;
    c->compact = legacy_compact || ONE_OF(line_layout, LAYOUTS, 1) == 0;
    if (!legacy_compact) c->show_separators = boolean(separators, false);
    double pld = jnum(levels);
    c->path_levels = jstr_eq(levels, "full") ? 0 : (jis_num(levels) && (pld == 1 || pld == 2 || pld == 3)) ? (int)pld : 1;
    jv mw = top("maxWidth");
    c->max_width = is_finite_num(mw) && jnum(mw) > 0 ? (int)fmin(floor(jnum(mw)), 1000) : -1;
    c->force_max_width = boolean(top("forceMaxWidth"), false);

    jv eo = top("elementOrder");
    c->nelem = jis_arr(eo) ? names(eo, EL_NAMES, EL_COUNT, c->elem) : 0;
    // sessionTokens and cost are left out of the default, so they keep their built-in place (the tokens
    // summary's last line, cost on the first line) until placed.
    if (!c->nelem)
        for (int i = 0; i < EL_COUNT; i++)
            if (i != EL_SESSION_TOKENS && i != EL_COST) c->elem[c->nelem++] = i;
    jv po = top("projectLineOrder");
    c->nplo = jis_arr(po) ? names(po, SEG_NAMES, SEG_COUNT, c->plo) : 0;

    Section git = section("gitStatus"), jj = section("jjStatus"), display = section("display"), colors = section("colors");
    c->git.enabled = boolean(get(git, "enabled"), true);
    c->git.show_dirty = boolean(get(git, "showDirty"), true);
    c->git.show_ahead_behind = boolean(get(git, "showAheadBehind"), false);
    c->git.show_file_stats = boolean(get(git, "showFileStats"), false);
    c->git.show_worktree = boolean(get(git, "showWorktree"), false);
    c->git.wrap = ONE_OF(get(git, "branchOverflow"), BRANCH_OVERFLOW, 0) == 1;
    c->git.push_warn = floor0(get(git, "pushWarningThreshold"), 0);
    c->git.push_crit = floor0(get(git, "pushCriticalThreshold"), 0);
    c->jj.enabled = boolean(get(jj, "enabled"), false);
    c->jj.show_dirty = boolean(get(jj, "showDirty"), true);
    c->jj.show_conflicts = boolean(get(jj, "showConflicts"), true);

    auto d = &c->d;
#define B(field, key, def) d->field = boolean(get(display, key), def)
    B(show_model, "showModel", true);
    B(show_project, "showProject", true);
    B(show_launch_dir, "showLaunchDir", false);
    B(show_added_dirs, "showAddedDirs", true);
    d->added_dirs_line = ONE_OF(get(display, "addedDirsLayout"), INLINE_LINE, 0) == 1;
    B(show_context_bar, "showContextBar", true);
    d->context_value = ONE_OF(get(display, "contextValue"), CONTEXT_VALUES, CV_PERCENT);
    B(show_config_counts, "showConfigCounts", false);
    B(show_cost, "showCost", false);
    B(show_routed_cost, "showRoutedCost", false);
    B(show_daily_cost, "showDailyCost", false);
    B(show_weekly_cost, "showWeeklyCost", false);
    B(show_duration, "showDuration", false);
    B(show_speed, "showSpeed", false);
    B(show_token_breakdown, "showTokenBreakdown", true);
    B(show_usage, "showUsage", true);
    d->usage_remaining = ONE_OF(get(display, "usageValue"), USAGE_VALUES, 0) == 1;
    B(usage_bar, "usageBarEnabled", true);
    B(show_reset_label, "showResetLabel", true);
    B(usage_compact, "usageCompact", false);
    B(show_model_scoped, "showModelScopedUsage", true);
    B(usage_pace, "usagePace", false);
    B(show_tools, "showTools", false);
    B(show_skills, "showSkills", false);
    B(show_mcp, "showMcp", false);
    d->tool_name_max = count(get(display, "toolNameMaxLength"), 0);
    d->tools_max_visible = count(get(display, "toolsMaxVisible"), 4);
    d->skills_max_visible = count(get(display, "skillsMaxVisible"), 4);
    B(show_agents, "showAgents", false);
    B(show_todos, "showTodos", false);
    B(show_session_name, "showSessionName", false);
    B(show_auth, "showAuth", false);
    B(show_auth_user, "showAuthUser", false);
    d->auth_user_length = count(get(display, "authUserLength"), 8);
    B(show_cc_version, "showClaudeCodeVersion", false);
    B(show_effort, "showEffortLevel", false);
    d->effort_format = ONE_OF(get(display, "effortFormat"), EFFORT_FORMATS, EF_FULL);
    B(show_agent_effort, "showAgentEffort", false);
    B(show_memory, "showMemoryUsage", false);
    B(show_prompt_cache, "showPromptCache", false);
    B(show_cache_hit_rate, "showCacheHitRate", false);
    B(show_session_tokens, "showSessionTokens", false);
    B(show_output_style, "showOutputStyle", false);
    B(show_session_start, "showSessionStartDate", false);
    B(show_last_response, "showLastResponseAt", false);
    B(show_compactions, "showCompactions", false);
    d->ctx_warn = clamp100(get(display, "contextWarningThreshold"), 70);
    d->ctx_crit = clamp100(get(display, "contextCriticalThreshold"), 85);
    d->usage_threshold = clamp100(get(display, "usageThreshold"), 0);
    d->seven_day_threshold = clamp100(get(display, "sevenDayThreshold"), 80);
    d->env_threshold = clamp100(get(display, "environmentThreshold"), 0);
    d->external_usage_path = usage_path(get(display, "externalUsagePath"), home);
    d->external_usage_write_path = usage_path(get(display, "externalUsageWritePath"), home);
    d->external_usage_freshness = floor0(get(display, "externalUsageFreshnessMs"), 300000);
    d->model_format = ONE_OF(get(display, "modelFormat"), MODEL_FORMATS, MF_FULL);
    d->model_override = text(get(display, "modelOverride"), 80);
    d->model_source = ONE_OF(get(display, "modelSource"), MODEL_SOURCES, MS_STDIN);
    B(show_provider, "showProvider", false);
    d->provider_name = text(get(display, "providerName"), 40);
    d->custom_line = text(get(display, "customLine"), 80);
    d->custom_first = ONE_OF(get(display, "customLinePosition"), POSITIONS, 1) == 0;
    d->time_format = ONE_OF(get(display, "timeFormat"), TIME_FORMATS, TF_RELATIVE);
    d->hour_cycle = ONE_OF(get(display, "hourCycle"), HOUR_CYCLES, HC_AUTO);
    B(show_clock_seconds, "showClockSeconds", false);
    B(show_advisor, "showAdvisor", false);
    d->advisor_override = text(get(display, "advisorOverride"), 80);
    jv acw = get(display, "autoCompactWindow");
    d->auto_compact_window = is_integer(acw) && jnum(acw) > 0 ? jnum(acw) : 0;
#undef B

    // mergeGroups: two or more known elements each, an element in one group only
    jv mg = get(display, "mergeGroups");
    bool default_groups = !jis_arr(mg);
    if (jis_arr(mg) && yyjson_arr_size(mg)) {
        bool used[EL_COUNT] = {0};
        size_t gi, gmax;
        jv group;
        yyjson_arr_foreach(mg, gi, gmax, group) {
            if (!jis_arr(group)) continue;
            int tmp[EL_COUNT], nt = 0;
            bool in[EL_COUNT] = {0};
            size_t idx, max;
            jv v;
            yyjson_arr_foreach(group, idx, max, v) {
                int e = index_of(v, EL_NAMES, EL_COUNT);
                if (e >= 0 && !used[e] && !in[e]) in[e] = true, tmp[nt++] = e;
            }
            if (nt < 2) continue;
            for (int k = 0; k < nt; k++) used[tmp[k]] = true, c->group[c->ngroups][k] = tmp[k];
            c->group_len[c->ngroups++] = nt;
        }
        default_groups = c->ngroups == 0;
    }
    if (default_groups) {
        c->ngroups = 1;
        c->group[0][0] = EL_CONTEXT, c->group[0][1] = EL_USAGE, c->group_len[0] = 2;
    }
    jv ra = get(display, "rightAlign");
    int ral[EL_COUNT];
    int nra = jis_arr(ra) ? names(ra, EL_NAMES, EL_COUNT, ral) : 0;
    for (int i = 0; i < nra; i++) c->right_align[ral[i]] = true;

    c->c.context = color(colors, "context", US("\x1b[32m"));
    c->c.usage = color(colors, "usage", US("\x1b[94m"));
    c->c.warning = color(colors, "warning", US("\x1b[33m"));
    c->c.usage_warning = color(colors, "usageWarning", US("\x1b[95m"));
    c->c.critical = color(colors, "critical", US("\x1b[31m"));
    c->c.model = color(colors, "model", US("\x1b[36m"));
    c->c.project = color(colors, "project", US("\x1b[33m"));
    c->c.git = color(colors, "git", US("\x1b[35m"));
    c->c.git_branch = color(colors, "gitBranch", US("\x1b[36m"));
    c->c.label = color(colors, "label", US("\x1b[2m"));
    c->c.custom = color(colors, "custom", US("\x1b[38;5;208m"));
    c->c.bar_filled = bar_char(get(colors, "barFilled"), US("█"));
    c->c.bar_empty = bar_char(get(colors, "barEmpty"), US("░"));
}
