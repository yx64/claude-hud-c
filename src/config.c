// Port of claude-hud dist/config.js loadConfig(): the two config files, merged, migrated and
// normalized against the defaults, plus the color values resolved the way render/colors.js does.
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

static jv read_config_file(us path) {
    fstat_t st = stat_path(path);
    if (!st.ok || !st.is_file || st.size > 64 * 1024) return nullptr;
    jv v = read_json_file(path);
    return jis_obj(v) && safe_shape(v, 0) ? v : nullptr;
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
static us text(jv v, int max) { return jis_str(v) ? us_slice(sanitize_display(jstr(v)), 0, max) : US_EMPTY; }

// usagePath: a leading ~ and ${VAR} expanded; unset variables are left as written.
static us usage_path(jv v, us home) {
    if (!jis_str(v)) return US_EMPTY;
    us s = us_trim(jstr(v));
    if (us_eq_c(s, "~")) s = home;
    else if (us_starts_c(s, "~/") || us_starts_c(s, "~\\")) s = path_join(2, home, us_slice(s, 2, s.n));
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
                                       "environment", "tools", "skills", "mcp", "agents", "todos", "sessionTime"};
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

// The color rule, then resolveAnsi: a name, a 256-color index, or #rrggbb.
static us color(Section colors, const char *key, us def) {
    static const char *const named[] = {"dim", "red", "green", "yellow", "magenta", "cyan", "brightBlue", "brightMagenta"};
    static const us ansi[] = {US_INIT("\x1b[2m"),  US_INIT("\x1b[31m"), US_INIT("\x1b[32m"), US_INIT("\x1b[33m"),
                              US_INIT("\x1b[35m"), US_INIT("\x1b[36m"), US_INIT("\x1b[94m"), US_INIT("\x1b[95m")};
    static_assert(countof(named) == countof(ansi));
    jv v = get(colors, key);
    int i = index_of(v, named, countof(named));
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

void load_config(Config *c, us claude_dir, us home) {
    BASE = read_config_file(path_join(4, claude_dir, US("plugins"), US("claude-hud"), US("config.json")));
    OVR = read_config_file(path_join(2, claude_dir, US("claude-hud.json")));
    *c = (Config){0};

    // migrateLegacyLayout: v0.0.x's `layout`, a string or an object, when lineLayout is absent
    jv line_layout = top("lineLayout"), separators = top("showSeparators"), levels = top("pathLevels");
    jv layout = top("layout");
    bool legacy_compact = false;
    if (layout && !line_layout) {
        if (jis_str(layout)) {
            legacy_compact = true;
            separators = nullptr;
            c->show_separators = jstr_eq(layout, "separators");
        } else if (jis_obj(layout)) {
            jv ll = jget(layout, "lineLayout"), ss = jget(layout, "showSeparators"), pl = jget(layout, "pathLevels");
            if (jis_str(ll)) line_layout = ll;
            if (ss && yyjson_is_bool(ss)) separators = ss;
            if (jis_num(pl) || jstr_eq(pl, "full")) levels = pl;
        }
    }

    static const char *const langs[] = {"en", "zh", "zh-Hans", "zh-Hant", "zh-TW"};
    if (ONE_OF(top("language"), langs, 0) != 0) UNSUPPORTED("non-English UI language");
    c->lang = LANG_EN;
    static const char *const layouts[] = {"compact", "expanded"};
    c->compact = legacy_compact || ONE_OF(line_layout, layouts, 1) == 0;
    if (!legacy_compact) c->show_separators = boolean(separators, false);
    double pld = jnum(levels);
    c->path_levels = jstr_eq(levels, "full") ? 0 : (jis_num(levels) && (pld == 1 || pld == 2 || pld == 3)) ? (int)pld : 1;
    jv mw = top("maxWidth");
    c->max_width = is_finite_num(mw) && jnum(mw) > 0 ? (int)fmin(floor(jnum(mw)), 1000) : -1;
    c->force_max_width = boolean(top("forceMaxWidth"), false);

    jv eo = top("elementOrder");
    c->nelem = jis_arr(eo) ? names(eo, EL_NAMES, EL_COUNT, c->elem) : 0;
    if (!c->nelem)
        for (int i = 0; i < EL_COUNT; i++) c->elem[c->nelem++] = i;
    jv po = top("projectLineOrder");
    c->nplo = jis_arr(po) ? names(po, SEG_NAMES, SEG_COUNT, c->plo) : 0;

    Section git = section("gitStatus"), jj = section("jjStatus"), display = section("display"), colors = section("colors");
    static const char *const overflow[] = {"truncate", "wrap"};
    c->git.enabled = boolean(get(git, "enabled"), true);
    c->git.show_dirty = boolean(get(git, "showDirty"), true);
    c->git.show_ahead_behind = boolean(get(git, "showAheadBehind"), false);
    c->git.show_file_stats = boolean(get(git, "showFileStats"), false);
    c->git.show_worktree = boolean(get(git, "showWorktree"), false);
    c->git.wrap = ONE_OF(get(git, "branchOverflow"), overflow, 0) == 1;
    c->git.push_warn = floor0(get(git, "pushWarningThreshold"), 0);
    c->git.push_crit = floor0(get(git, "pushCriticalThreshold"), 0);
    c->jj.enabled = boolean(get(jj, "enabled"), false);
    c->jj.show_dirty = boolean(get(jj, "showDirty"), true);
    c->jj.show_conflicts = boolean(get(jj, "showConflicts"), true);

    static const char *const inline_line[] = {"inline", "line"};
    static const char *const cv[] = {"percent", "tokens", "remaining", "both"};
    static const char *const uv[] = {"percent", "remaining"};
    static const char *const ef[] = {"full", "symbol", "text"};
    static const char *const mf[] = {"full", "compact", "short"};
    static const char *const ms[] = {"auto", "stdin", "transcript"};
    static const char *const tf[] = {"relative", "absolute", "both", "elapsed", "elapsedAndAbsolute"};
    static const char *const hc[] = {"auto", "h11", "h12", "h23", "h24"};
    static const char *const pos[] = {"first", "last"};
    auto d = &c->d;
#define B(field, key, def) d->field = boolean(get(display, key), def)
    B(show_model, "showModel", true);
    B(show_project, "showProject", true);
    B(show_added_dirs, "showAddedDirs", true);
    d->added_dirs_line = ONE_OF(get(display, "addedDirsLayout"), inline_line, 0) == 1;
    B(show_context_bar, "showContextBar", true);
    d->context_value = ONE_OF(get(display, "contextValue"), cv, CV_PERCENT);
    B(show_config_counts, "showConfigCounts", false);
    B(show_cost, "showCost", false);
    B(show_routed_cost, "showRoutedCost", false);
    B(show_daily_cost, "showDailyCost", false);
    B(show_weekly_cost, "showWeeklyCost", false);
    B(show_duration, "showDuration", false);
    B(show_speed, "showSpeed", false);
    B(show_token_breakdown, "showTokenBreakdown", true);
    B(show_usage, "showUsage", true);
    d->usage_remaining = ONE_OF(get(display, "usageValue"), uv, 0) == 1;
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
    d->effort_format = ONE_OF(get(display, "effortFormat"), ef, EF_FULL);
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
    d->model_format = ONE_OF(get(display, "modelFormat"), mf, MF_FULL);
    d->model_override = text(get(display, "modelOverride"), 80);
    d->model_source = ONE_OF(get(display, "modelSource"), ms, MS_STDIN);
    B(show_provider, "showProvider", false);
    d->provider_name = text(get(display, "providerName"), 40);
    d->custom_line = text(get(display, "customLine"), 80);
    d->custom_first = ONE_OF(get(display, "customLinePosition"), pos, 1) == 0;
    d->time_format = ONE_OF(get(display, "timeFormat"), tf, TF_RELATIVE);
    d->hour_cycle = ONE_OF(get(display, "hourCycle"), hc, HC_AUTO);
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
