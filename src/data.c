// The data claude-hud's main() gathers before rendering: config-reader.js, auth.js,
// daily-cost.js, stdin.js getUsageFromStdin, external-usage.js, memory.js and speed.js.
#include <math.h>
#include <string.h>

#include "hud.h"
#include "utf8proc.h"

static jv sget(Ctx *ctx, const char *a, const char *b) {
    jv v = jget(ctx->input, a);
    return b ? jget(v, b) : v;
}

// getClaudeConfigJsonPath: inside CLAUDE_CONFIG_DIR when it is set, else in the home directory.
static us claude_json(Ctx *ctx) {
    return us_trim(env_get("CLAUDE_CONFIG_DIR")).n ? path_join(ctx->claude_dir, US(".claude.json"))
                                                   : path_join(ctx->home, US(".claude.json"));
}

// .claude.json is read once: the config counts and the auth line both look into it.
static jv claude_json_doc(Ctx *ctx) {
    static bool read;
    static jv doc;
    if (!read) doc = read_json_file(claude_json(ctx)), read = true;
    return doc;
}

static void key_of(us s, const char **k, uint32_t *kn) {
    *k = (const char *)s.p, *kn = (uint32_t)s.n * 2;
}

// ---- config-reader.js ----

static jv plain_object_file(us path) {  // readJson: null unless a non-array object
    jv v = read_json_file(path);
    return jis_obj(v) ? v : nullptr;
}

static void add_keys(omap *set, jv obj) {  // keysOf: own keys of a non-array object
    if (!jis_obj(obj)) return;
    size_t idx, max;
    jv k, v;
    yyjson_obj_foreach(obj, idx, max, k, v) {
        const char *kp;
        uint32_t kn;
        key_of(jstr(k), &kp, &kn);
        om_put(set, kp, kn, nullptr);
    }
}

static int key_count(jv obj) {
    omap m = {0};
    add_keys(&m, obj);
    return m.alive;
}

static void del_strings(omap *set, jv arr) {  // for each string of an array, Set.delete
    if (!jis_arr(arr)) return;
    size_t idx, max;
    jv v;
    yyjson_arr_foreach(arr, idx, max, v) {
        if (!jis_str(v)) continue;
        const char *kp;
        uint32_t kn;
        key_of(jstr(v), &kp, &kn);
        om_ent *e = om_get(set, kp, kn);
        if (e && e->alive) om_del(set, e);
    }
}

typedef struct { omap visited; int entries, dirs; } RuleState;

static bool rule_visit(RuleState *s, us real, bool add) {
    const char *kp;
    uint32_t kn;
    key_of(real, &kp, &kn);
    if (om_get(&s->visited, kp, kn)) return true;
    if (add) om_put(&s->visited, kp, kn, nullptr);
    return false;
}

// countRules: .md files, following links once each, bounded. A failure inside the listing ends
// it, as the try around the loop does upstream. `dir` is already resolved below the top level.
static int count_rules(us dir, bool resolved, RuleState *s) {
    us root = dir;
    if ((!resolved && !real_path(dir, &root)) || rule_visit(s, root, false) || s->dirs >= 1000) return 0;
    rule_visit(s, root, true);
    s->dirs++;
    int count = 0;
    DirEntry *e;
    // Listing past what the entry bound lets through would only be thrown away.
    int n = os_list_dir(root, &e, 10001 - s->entries);
    for (int i = 0; i < n; i++) {
        us name = e[i].name;
        if (++s->entries > 10000) break;
        // Under a resolved root, anything but a link resolves to its own path, and the listing
        // already says what it is; only a link needs realpath and stat.
        us target;
        fstat_t st;
        if (e[i].kind == DE_OTHER) {
            if (!real_path(path_join(root, name), &target) || rule_visit(s, target, false)) continue;
            st = stat_path(target);
            if (!st.ok) break;
        } else {
            target = path_join(root, name);
            if (rule_visit(s, target, false)) continue;
            bool is_dir = e[i].kind == DE_DIR;
            st = (fstat_t){.ok = true, .is_dir = is_dir, .is_file = !is_dir};
        }
        if (st.is_dir) count += count_rules(target, true, s);
        else if (st.is_file && us_ends_c(name, ".md")) {
            rule_visit(s, target, true);
            count++;
        }
    }
    return count;
}

static int rules_in(us dir) {
    RuleState *s = arena_alloc(sizeof *s);
    *s = (RuleState){0};
    return count_rules(dir, false, s);
}

static us location(us p) {  // sameLocation's normalize
    us r = path_resolve(p);
    while (r.n && (r.p[r.n - 1] == '\\' || r.p[r.n - 1] == '/')) r.n--;
    return path_key(r.n ? r : path_resolve(p));
}

static bool same_location(us a, us b) {
    if (us_eq(location(a), location(b))) return true;
    us ra, rb;
    return real_path(a, &ra) && real_path(b, &rb) && us_eq(location(ra), location(rb));
}

static bool exists(us p) { return file_exists(p); }

void count_configs(Ctx *ctx) {
    us cd = ctx->claude_dir;
    jv user = plain_object_file(path_join(cd, US("settings.json")));
    jv cj = claude_json_doc(ctx);
    if (!jis_obj(cj)) cj = nullptr;
    omap user_mcp = {0}, project_mcp = {0};
    add_keys(&user_mcp, jget(user, "mcpServers"));
    add_keys(&user_mcp, jget(cj, "mcpServers"));
    del_strings(&user_mcp, jget(cj, "disabledMcpServers"));
    int md = exists(path_join(cd, US("CLAUDE.md")));
    int rules = rules_in(path_join(cd, US("rules")));
    int hooks = key_count(jget(user, "hooks"));
    jv cwdv = sget(ctx, "cwd", nullptr);
    if (jtruthy(cwdv) && !jis_str(cwdv)) UNSUPPORTED("non-string cwd");
    if (jtruthy(cwdv) && jis_str(cwdv)) {
        us cwd = jstr(cwdv);
        us pd = path_join(cwd, US(".claude"));
        // When the project's .claude is the user config dir, its files were counted above.
        bool user_dir = same_location(pd, cd);
        md += exists(path_join(cwd, US("CLAUDE.md")));
        md += exists(path_join(cwd, US("CLAUDE.local.md")));
        md += !user_dir && exists(path_join(pd, US("CLAUDE.md")));
        md += exists(path_join(pd, US("CLAUDE.local.md")));
        if (!user_dir) {
            rules += rules_in(path_join(pd, US("rules")));
            jv ps = plain_object_file(path_join(pd, US("settings.json")));
            add_keys(&project_mcp, jget(ps, "mcpServers"));
            hooks += key_count(jget(ps, "hooks"));
        }
        jv ls = plain_object_file(path_join(pd, US("settings.local.json")));
        add_keys(&project_mcp, jget(ls, "mcpServers"));
        hooks += key_count(jget(ls, "hooks"));
        omap mcpjson = {0};
        add_keys(&mcpjson, jget(plain_object_file(path_join(cwd, US(".mcp.json"))), "mcpServers"));
        del_strings(&mcpjson, jget(ls, "disabledMcpjsonServers"));
        for (int32_t i = 0; i < mcpjson.n; i++)
            if (mcpjson.e[i].alive) om_put(&project_mcp, mcpjson.e[i].k, mcpjson.e[i].kn, nullptr);
    }
    ctx->claude_md = md, ctx->rules = rules, ctx->hooks = hooks;
    // A server named in both scopes counts twice: they are separate configs.
    ctx->mcp = user_mcp.alive + project_mcp.alive;
}

// ---- auth.js ----

static bool auth_value(jv obj, const char *key, us *out) {
    jv v = jget(obj, key);
    if (!jis_str(v)) return false;
    us s = us_slice(us_trim(sanitize_display(jstr(v))), 0, 128);
    if (!s.n) return false;
    *out = s;
    return true;
}

static bool special_upper(uint16_t c) {  // toUpperCase maps these to more than one character
    return c == 0xDF || c == 0x149 || c == 0x1F0 || c == 0x390 || c == 0x3B0 || c == 0x587 || (c >= 0x1E96 && c <= 0x1E9A) ||
           (c >= 0x1F50 && c <= 0x1FFF) || (c >= 0xFB00 && c <= 0xFB17);
}

void read_auth(Ctx *ctx) {
    if (us_trim(env_get("ANTHROPIC_API_KEY")).n) {
        ctx->has_auth_method = true, ctx->auth_method = US("API Key");
        return;
    }
    jv root = claude_json_doc(ctx);
    jv acct = jget(root, "oauthAccount");
    if (!jtruthy(acct) || !(jis_obj(acct) || jis_arr(acct))) return;
    us org;
    if (auth_value(acct, "organizationType", &org)) {
        // "claude_max" -> "Claude Max"
        sb b = {0};
        bool first = true;
        for (int32_t i = 0; i <= org.n;) {
            int32_t j = us_find_ch(org, '_', i);
            if (j < 0) j = org.n;
            if (j > i) {
                if (!first) sb_ch(&b, ' ');
                first = false;
                uint16_t c0 = org.p[i];
                if (special_upper(c0)) UNSUPPORTED("organization type with a multi-character uppercase");
                sb_ch(&b, (uint16_t)(c0 >= 0xD800 && c0 <= 0xDFFF ? c0 : utf8proc_toupper(c0)));
                sb_us(&b, us_slice(org, i + 1, j));
            }
            i = j + 1;
        }
        us method = sb_done(&b);
        us tier;
        if (auth_value(acct, "organizationRateLimitTier", &tier) && tier.n >= 3 && (tier.p[tier.n - 1] | 32) == 'x') {
            // /_(\d+x)$/i
            int32_t k = tier.n - 1;
            while (k > 0 && tier.p[k - 1] >= '0' && tier.p[k - 1] <= '9') k--;
            if (k < tier.n - 1 && k > 0 && tier.p[k - 1] == '_') {
                us suffix = us_slice(tier, k, tier.n);
                us ml = us_lower(method), sl = us_lower(suffix);
                bool has = false;
                for (int32_t i = 0; i + sl.n <= ml.n && !has; i++) has = us_eq(us_slice(ml, i, i + sl.n), sl);
                if (!has) method = us_cat(method, US(" "), suffix);
            }
        }
        ctx->has_auth_method = true, ctx->auth_method = method;
    }
    us email, name;
    if (auth_value(acct, "emailAddress", &email)) {
        int32_t at = us_find_ch(email, '@', 0);
        ctx->auth_user = at < 0 ? email : us_slice(email, 0, at);
        ctx->has_auth_user = ctx->auth_user.n > 0;
    } else if (auth_value(acct, "displayName", &name)) {
        ctx->has_auth_user = true, ctx->auth_user = name;
    }
}

// ---- stdin.js getUsageFromStdin, cost.js getNativeCostUsd ----

static double date_ms(double ms) {  // new Date(ms).getTime(), NaN standing for null
    if (!isfinite(ms)) return NAN;
    if (fabs(ms) > 8.64e15) {
        UNSUPPORTED("reset time out of range");
        return NAN;
    }
    return trunc(ms) + 0.0;
}

static double reset_at(jv v) { return jis_num(v) && isfinite(jnum(v)) && jnum(v) > 0 ? date_ms(jnum(v) * 1000) : NAN; }

static bool stdin_usage(Ctx *ctx, Usage *u) {
    jv rl = sget(ctx, "rate_limits", nullptr);
    jv f = jget(jget(rl, "five_hour"), "used_percentage"), s = jget(jget(rl, "seven_day"), "used_percentage");
    *u = (Usage){.five = is_finite_num(f) ? to_percent(jnum(f)) : NAN, .seven = is_finite_num(s) ? to_percent(jnum(s)) : NAN,
                 .five_reset = NAN, .seven_reset = NAN};
    if (isnan(u->five) && isnan(u->seven)) return false;
    u->five_reset = reset_at(jget(jget(rl, "five_hour"), "resets_at"));
    u->seven_reset = reset_at(jget(jget(rl, "seven_day"), "resets_at"));
    return true;
}

bool native_cost(Ctx *ctx, double *out) {
    jv c = sget(ctx, "cost", "total_cost_usd");
    if (!is_finite_num(c) || jnum(c) < 0) return false;
    jv id = sget(ctx, "model", "id");
    if ((is_bedrock_id(id) || is_vertex_id(id)) && (!ctx->cfg->d.show_routed_cost || jnum(c) == 0)) return false;
    *out = jnum(c);
    return true;
}

// ---- external-usage.js ----

// Epoch seconds or milliseconds, or a date string; NaN for null.
static double snapshot_date(jv v) {
    if (is_finite_num(v) && jnum(v) > 0) return date_ms(jnum(v) > 1e12 ? jnum(v) : jnum(v) * 1000);
    if (jis_str(v) && us_trim(jstr(v)).n) return parse_date(jstr(v));
    return NAN;
}

static double snapshot_percent(jv v) { return is_finite_num(v) ? js_round(fmin(100, fmax(0, jnum(v)))) : NAN; }

static bool external_usage(Ctx *ctx, Usage *u) {
    us p = ctx->cfg->d.external_usage_path;
    if (!p.n || !path_is_absolute(p)) return false;
    jv s = read_json_file(p);
    if (!s || yyjson_is_null(s)) return false;
    double updated = snapshot_date(jget(s, "updated_at"));
    if (isnan(updated) || ctx->now - updated > ctx->cfg->d.external_usage_freshness) return false;
    *u = (Usage){.five = snapshot_percent(jget(jget(s, "five_hour"), "used_percentage")),
                 .seven = snapshot_percent(jget(jget(s, "seven_day"), "used_percentage"))};
    jv bl = jget(s, "balance_label");
    if (jis_str(bl)) {
        us l = us_trim(sanitize_display(jstr(bl)));
        if (l.n) u->has_balance = true, u->balance = truncate_string(l, 50, US("..."));
    }
    jv ms = jget(s, "model_scoped");
    if (jis_arr(ms)) {
        u->scoped = arena_alloc(sizeof(Scoped) * 8);
        size_t idx, max;
        jv w;
        yyjson_arr_foreach(ms, idx, max, w) {
            if (u->nscoped >= 8) break;
            jv dn = jget(w, "display_name"), ut = jget(w, "utilization"), ra = jget(w, "resets_at");
            us lab = jis_str(dn) ? us_slice(us_trim(sanitize_display(jstr(dn))), 0, 64) : US_EMPTY;
            bool null_util = ut && yyjson_is_null(ut);
            double pct = null_util ? NAN : snapshot_percent(ut);
            if (!lab.n || (!null_util && isnan(pct))) continue;
            double reset = NAN;
            if (jis_str(ra)) {
                reset = parse_date(jstr(ra));
            }
            u->scoped[u->nscoped++] = (Scoped){lab, pct, reset};
        }
    }
    if (isnan(u->five) && isnan(u->seven) && !u->has_balance && !u->nscoped) return false;
    // A reset time that is present but unreadable means a broken snapshot, not a missing one.
    jv fr = jget(jget(s, "five_hour"), "resets_at"), sr = jget(jget(s, "seven_day"), "resets_at");
    u->five_reset = snapshot_date(fr), u->seven_reset = snapshot_date(sr);
    if ((!jis_nullish(fr) && isnan(u->five_reset)) || (!jis_nullish(sr) && isnan(u->seven_reset))) return false;
    return true;
}

static void write_snapshot(Ctx *ctx, Usage *u) {
    us target = ctx->cfg->d.external_usage_write_path;
    if (!target.n || !path_is_absolute(target) || !us_ends_c(us_lower(target), ".json")) return;
    us path = path_resolve(target);
    us dir = dir_of(path);
    sb b = {0};
    sb_us(&b, US(",\n  \"five_hour\": {\n    \"used_percentage\": "));
    if (isnan(u->five)) sb_us(&b, US("null"));
    else sb_num(&b, u->five);
    sb_us(&b, US(",\n    \"resets_at\": "));
    if (isnan(u->five_reset)) sb_us(&b, US("null"));
    else json_str(&b, iso_string(u->five_reset));
    sb_us(&b, US("\n  },\n  \"seven_day\": {\n    \"used_percentage\": "));
    if (isnan(u->seven)) sb_us(&b, US("null"));
    else sb_num(&b, u->seven);
    sb_us(&b, US(",\n    \"resets_at\": "));
    if (isnan(u->seven_reset)) sb_us(&b, US("null"));
    else json_str(&b, iso_string(u->seven_reset));
    sb_us(&b, US("\n  }\n}\n"));
    us rest = sb_done(&b);
    fstat_t ds = stat_path(dir);
    if (!ds.ok || !ds.is_dir) return;
    // shouldWrite: the values changed, or the snapshot is older than the throttle
    fstat_t st = stat_path(path);
    bool write = !st.ok || ctx->now - st.mtime_ms > 30000;
    if (!write) {
        jv cur = read_json_file(path);
        if (!jis_obj(cur)) write = true;
        else {
            sb c = {0}, n = {0};
            sb_ch(&c, '{');
            bool first = true;
            size_t idx, max;
            jv k, v;
            yyjson_obj_foreach(cur, idx, max, k, v) {
                if (jstr_eq(k, "updated_at")) continue;
                if (!first) sb_ch(&c, ',');
                first = false;
                json_str(&c, jstr(k));
                sb_ch(&c, ':');
                json_stringify(&c, v);
            }
            sb_ch(&c, '}');
            sb_us(&n, US("{\"five_hour\":{\"used_percentage\":"));
            if (isnan(u->five)) sb_us(&n, US("null"));
            else sb_num(&n, u->five);
            sb_us(&n, US(",\"resets_at\":"));
            if (isnan(u->five_reset)) sb_us(&n, US("null"));
            else json_str(&n, iso_string(u->five_reset));
            sb_us(&n, US("},\"seven_day\":{\"used_percentage\":"));
            if (isnan(u->seven)) sb_us(&n, US("null"));
            else sb_num(&n, u->seven);
            sb_us(&n, US(",\"resets_at\":"));
            if (isnan(u->seven_reset)) sb_us(&n, US("null"));
            else json_str(&n, iso_string(u->seven_reset));
            sb_us(&n, US("}}"));
            write = !us_eq(sb_done(&c), sb_done(&n));
        }
    }
    if (!write) return;
    sb body = {0};
    sb_us(&body, US("{\n  \"updated_at\": "));
    json_str(&body, iso_string(ctx->now));
    sb_us(&body, rest);
    int32_t base = path.n;
    while (base > 0 && path.p[base - 1] != OS_SEP) base--;
    us name = us_cat(US("."), us_slice(path, base, path.n), US("."), us_int(os_pid()), US("."), us_int((int64_t)ctx->now), US(".tmp"));
    us tmp = path_join(dir, name);
    if (!write_file(tmp, sb_done(&body), 0, WRITE_NEW)) return;
    os_rename(tmp, path);
}

// main(): stdin's usage, the snapshot written from it, and resolveUsage on top.
void gather_usage(Ctx *ctx) {
    Usage su;
    bool have = stdin_usage(ctx, &su);
    if (ctx->cfg->d.external_usage_write_path.n && have) write_snapshot(ctx, &su);
    if (!ctx->cfg->d.show_usage) return;
    Usage ex;
    bool hex = ctx->cfg->d.external_usage_path.n && external_usage(ctx, &ex);
    if (!have || !hex) {
        if (have) ctx->usage = su, ctx->has_usage = true;
        else if (hex) ctx->usage = ex, ctx->has_usage = true;
        return;
    }
    // Stdin wins; the snapshot fills in what it lacks.
    ctx->usage = su, ctx->has_usage = true;
    if (ex.has_balance) ctx->usage.has_balance = true, ctx->usage.balance = ex.balance;
    if (isnan(su.seven) && !isnan(ex.seven)) ctx->usage.seven = ex.seven, ctx->usage.seven_reset = ex.seven_reset;
    if (ex.scoped) ctx->usage.scoped = ex.scoped, ctx->usage.nscoped = ex.nscoped;
}

// ---- daily-cost.js ----

typedef struct { us id; double baseline, week_baseline, total, ts; } Session;
enum { MAX_SESSIONS = 4096 };  // upstream has no limit; a ledger past it is reported, not cut short

static bool amount(jv v) { return is_finite_num(v) && jnum(v) >= 0; }

static void local_date_key(double ms, char out[16]) {
    ltime lt = {0};  // within Date's range a year has at most 6 digits, so the key always fits
    local_time(ms, &lt);
    char *p = put_int(out, lt.y, 1);
    p = put_int(p, lt.mo, 2);
    *put_int(p, lt.d, 2) = 0;
}

// Renders run in separate processes that share one ledger. The rename keeps the file whole, but two
// overlapping read-modify-write cycles still drop the earlier writer's changes, a whole new session and
// its baseline included; a lock file makes each cycle run alone. An update takes a few milliseconds, so a
// lock older than two seconds belongs to a crashed process. The clock here is the real one, not the
// refresh's. Without the lock, after half a second or when it cannot be made, the update goes ahead.
static bool ledger_lock(us lock) {
    double deadline = os_now_ms() + 500;
    for (;;) {
        int r = os_lock_file(lock);
        if (r != LOCK_HELD) return r == LOCK_TAKEN;
        fstat_t st = stat_path(lock);
        if (st.ok && os_now_ms() - st.mtime_ms > 2000) os_remove(lock);
        if (os_now_ms() >= deadline) return false;
        os_sleep_ms(10);
    }
}

static void update_ledger(Ctx *ctx, us path) {
    const double DAY = 86400000.0, WEEK = 7 * DAY;
    double now = ctx->now;
    char today[16];
    local_date_key(now, today);
    Session *e = arena_alloc(sizeof(Session) * MAX_SESSIONS);
    int n = 0;
    double week_start = now, carry = 0;
    char date[16];
    memcpy(date, today, 16);
    bool changed = true;
    jv root = read_json_file(path);
    jv d = jget(root, "date"), sess = jget(root, "sessions");
    bool digits = jis_str(d) && yyjson_get_len(d) == 8;
    for (int i = 0; digits && i < 8; i++) digits = yyjson_get_str(d)[i] >= '0' && yyjson_get_str(d)[i] <= '9';
    if (jis_obj(root) && digits && jis_obj(sess)) {
        changed = false;
        memcpy(date, yyjson_get_str(d), 8);
        date[8] = 0;
        size_t idx, max;
        jv k, v;
        yyjson_obj_foreach(sess, idx, max, k, v) {
            // Object.entries order: index-like keys would come first; session ids never are.
            const char *ks = yyjson_get_str(k);
            size_t kl = yyjson_get_len(k);
            bool index_like = kl > 0 && kl <= 10 && !(kl > 1 && ks[0] == '0');
            for (size_t i = 0; i < kl && index_like; i++) index_like = ks[i] >= '0' && ks[i] <= '9';
            if (index_like || jstr_eq(k, "__proto__")) UNSUPPORTED("session id with special meaning as a key");
            us id = jstr(k);
            for (int i = 0; i < n; i++)
                if (us_eq(e[i].id, id)) UNSUPPORTED("duplicate session id in ledger");
            jv b = jget(v, "baseline"), t = jget(v, "total"), ts = jget(v, "ts"), wb = jget(v, "weekBaseline");
            if (!jis_obj(v) || !amount(b) || !amount(t) || !amount(ts) || jnum(ts) == 0) continue;
            if (n == MAX_SESSIONS) {
                UNSUPPORTED("more sessions in the ledger than this port keeps");
                continue;
            }
            e[n++] = (Session){id, jnum(b), amount(wb) ? jnum(wb) : jnum(b), jnum(t), jnum(ts)};
        }
        jv wk = jget(root, "week");
        jv ws = jget(wk, "start"), wc = jget(wk, "carry");
        week_start = jis_obj(wk) && amount(ws) ? jnum(ws) : 0;
        carry = jis_obj(wk) && amount(wc) ? jnum(wc) : 0;
    }
    // Sessions unseen for a day leave the ledger, but their spend stays inside the week.
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (now - e[i].ts > DAY) {
            carry += fmax(0, e[i].total - e[i].week_baseline);
            changed = true;
        } else e[k++] = e[i];
    }
    n = k;
    // At midnight, active sessions carry over so that only today's part of their spend counts.
    if (strcmp(date, today) != 0) {
        memcpy(date, today, 16);
        for (int i = 0; i < n; i++) e[i].baseline = e[i].total;
        changed = true;
    }
    // Restart the week when the quota window behind the Weekly bar opened after accumulation began.
    double reset = ctx->has_usage ? ctx->usage.seven_reset : NAN;
    bool has_window = !isnan(reset);
    double window_start = reset - WEEK;
    if (has_window && week_start < window_start) {
        for (int i = 0; i < n; i++) e[i].week_baseline = e[i].total;
        week_start = now, carry = 0;
        changed = true;
    }
    jv sid = sget(ctx, "session_id", nullptr);
    us id = jis_str(sid) ? us_trim(jstr(sid)) : US_EMPTY;
    double cost;
    if (id.n && native_cost(ctx, &cost)) {
        int at = -1;
        for (int i = 0; i < n; i++)
            if (us_eq(e[i].id, id)) at = i;
        if (at < 0) {
            if (n < MAX_SESSIONS) e[n++] = (Session){id, cost, cost, cost, now};
            else UNSUPPORTED("more sessions in the ledger than this port keeps");
            changed = true;
        } else {
            if (cost > e[at].total) e[at].total = cost, changed = true;
            e[at].ts = now;
        }
    }
    if (!n && carry == 0) return;
    fstat_t st = stat_path(path);
    if (changed || !st.ok || now - st.mtime_ms > 30000) {
        sb b = {0};
        sb_us(&b, US("{\n  \"date\": \""));
        sb_c(&b, date);
        sb_us(&b, n ? US("\",\n  \"sessions\": {") : US("\",\n  \"sessions\": {}"));
        for (int i = 0; i < n; i++) {
            sb_us(&b, i ? US(",\n    ") : US("\n    "));
            json_str(&b, e[i].id);
            sb_us(&b, US(": {\n      \"baseline\": "));
            sb_num(&b, e[i].baseline);
            sb_us(&b, US(",\n      \"weekBaseline\": "));
            sb_num(&b, e[i].week_baseline);
            sb_us(&b, US(",\n      \"total\": "));
            sb_num(&b, e[i].total);
            sb_us(&b, US(",\n      \"ts\": "));
            sb_num(&b, e[i].ts);
            sb_us(&b, US("\n    }"));
        }
        if (n) sb_us(&b, US("\n  }"));
        sb_us(&b, US(",\n  \"week\": {\n    \"start\": "));
        sb_num(&b, week_start);
        sb_us(&b, US(",\n    \"carry\": "));
        sb_num(&b, carry);
        sb_us(&b, US("\n  }\n}\n"));
        us name = us_cat(US("daily-cost.json."), us_int(os_pid()), US("."), us_int((int64_t)now), US(".tmp"));
        us tmp = path_join(dir_of(path), name);
        // Write-then-rename, so concurrent sessions never read a torn ledger. The rename keeps the
        // stamped time.
        if (write_file(tmp, sb_done(&b), now, WRITE_NEW)) os_rename(tmp, path);
    }
    double today_usd = 0, week_usd = 0;
    for (int i = 0; i < n; i++) today_usd += fmax(0, e[i].total - e[i].baseline);
    for (int i = 0; i < n; i++) week_usd += fmax(0, e[i].total - e[i].week_baseline);
    ctx->has_costs = true, ctx->today = today_usd;
    ctx->has_week = has_window, ctx->week = carry + week_usd;
}

void cost_totals(Ctx *ctx) {
    us path = path_join(ctx->claude_dir, US("plugins"), US("claude-hud"), US("daily-cost.json"));
    us lock = us_cat(path, US(".lock"));
    bool held = ledger_lock(lock);
    update_ledger(ctx, path);
    if (held) os_remove(lock);
}

// ---- memory.js ----

void memory_usage(Ctx *ctx) {
    double total, free;
#if defined(HUD_C_TEST)
    // Test build only: the comparison scripts give both sides the same figures (test/compare/fakeclock.mjs).
    size_t got;
    const char *t = us_to_utf8(env_get("HUD_FAKE_MEM"), &got);
    const char *colon = memchr(t, ':', got);
    if (colon && number_of(t, (size_t)(colon - t), &total) && number_of(colon + 1, got - (size_t)(colon + 1 - t), &free)) goto have;
#endif
    if (!os_memory(&total, &free)) return;
#if defined(HUD_C_TEST)
have:
#endif
    if (!isfinite(total) || total <= 0) return;
    free = isfinite(free) ? fmin(fmax(free, 0), total) : 0;
    ctx->has_memory = true;
    ctx->mem_total = total, ctx->mem_used = total - free;
    ctx->mem_percent = js_round(ctx->mem_used / total * 100);
}

// ---- speed.js ----

static bool count_value(jv v) { return is_finite_num(v) && jnum(v) >= 0; }

static void join_part(sb *b, jv v) {  // Array.prototype.join: null and undefined are empty
    if (!jis_nullish(v)) js_string_of(b, v);
}

void output_speed(Ctx *ctx) {
    jv tpv = sget(ctx, "transcript_path", nullptr);
    if (!jis_nullish(tpv) && !jis_str(tpv)) {
        UNSUPPORTED("non-string transcript_path");
        return;
    }
    us tp = us_trim(jstr(tpv));
    jv usage = sget(ctx, "context_window", nullptr);
    usage = jget(usage, "current_usage");
    jv output = jget(usage, "output_tokens"), api = sget(ctx, "cost", "total_api_duration_ms");
    if (!tp.n || !jtruthy(usage) || !count_value(output) || !count_value(api)) return;
    us file = path_join(ctx->claude_dir, US("plugins"), US("claude-hud"), US("speed-cache"),
                        us_cat(sha256_hex(path_resolve(tp)), US(".json")));
    sb kb = {0};
    join_part(&kb, jget(usage, "input_tokens"));
    sb_ch(&kb, ':');
    join_part(&kb, output);
    sb_ch(&kb, ':');
    join_part(&kb, jget(usage, "cache_creation_input_tokens"));
    sb_ch(&kb, ':');
    join_part(&kb, jget(usage, "cache_read_input_tokens"));
    us key = sb_done(&kb);
    jv st = read_json_file(file);
    jv pa = jget(st, "apiDurationMs"), pk = jget(st, "usageKey"), ps = jget(st, "speed");
    bool prev = st && !yyjson_is_null(st) && count_value(pa) && jis_str(pk) && (jis_nullish(ps) || count_value(ps));
    double a = jnum(api), out = jnum(output);
    bool has = false;
    double speed = 0;
    if (prev && a == jnum(pa)) {
        ctx->has_speed = !jis_nullish(ps), ctx->speed = jnum(ps);
        return;
    }
    if (prev && a > jnum(pa)) {
        double delta = a - jnum(pa);
        bool fresh = !us_eq(key, jstr(pk)) && out > 0;
        if (fresh && delta >= 500) has = true, speed = out / (delta / 1000);
        else if (!jis_nullish(ps)) has = true, speed = jnum(ps);
    }
    sb b = {0};
    sb_us(&b, US("{\"apiDurationMs\":"));
    sb_num(&b, a);
    sb_us(&b, US(",\"usageKey\":"));
    json_str(&b, key);
    sb_us(&b, US(",\"speed\":"));
    if (has) sb_num(&b, speed);
    else sb_us(&b, US("null"));
    sb_ch(&b, '}');
    us tmp = us_cat(file, US("."), us_int(os_pid()), US(".tmp"));
    if (write_file(tmp, sb_done(&b), 0, WRITE_ALWAYS)) os_rename(tmp, file);
    ctx->has_speed = has, ctx->speed = speed;
}
