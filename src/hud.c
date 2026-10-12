// claude-hud statusLine in C: dist/index.js main(), plus the COLUMNS adjustment of the launcher
// that /claude-hud:setup installs, since Claude Code runs this program in its place.
#include <math.h>
#include <string.h>

#include "hud.h"

// Build variants, chosen by CMakeLists.txt; the release build defines neither and reads no switch
// at run time. A case this port does not reproduce exactly (g_unsupported) is rendered anyway by
// the release build.
//   HUD_C_TEST   for the comparison scripts: no run time on the last line, such a case exits 3
//                with the reason instead of output, and the clock and memory figures can be
//                frozen (now_ms, memory_usage).
//   HUD_C_TRACE  a temporary observation build for a live session: the release behaviour plus
//                a trace log and a capture directory beside the executable.
#if defined(HUD_C_TEST) && defined(HUD_C_TRACE)
#error "HUD_C_TEST and HUD_C_TRACE are separate builds"
#endif

// ---- claude-config-dir.js ----

static us claude_dir_of(us home) {
    us env = us_trim(env_get("CLAUDE_CONFIG_DIR"));
    if (!env.n) return path_join(2, home, US(".claude"));
    if (us_eq_c(env, "~")) return path_resolve(home);
    if (us_starts_c(env, "~/") || us_starts_c(env, "~\\")) return path_join(2, home, us_slice(env, 2, env.n));
    return path_resolve(env);
}

// ---- utils/terminal.js after the launcher's COLUMNS - 4 ----

static double parse_int(us s) {  // Number.parseInt(s, 10)
    int32_t i = 0;
    while (i < s.n && js_is_space(s.p[i])) i++;
    bool neg = false;
    if (i < s.n && (s.p[i] == '+' || s.p[i] == '-')) neg = s.p[i++] == '-';
    int32_t start = i;
    double v = 0;
    while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') v = v * 10 + (s.p[i++] - '0');
    return i == start ? NAN : neg ? -v : v;
}

static int terminal_width(void) {
    double c = parse_int(env_get("COLUMNS"));
    if (!(c > 0)) return -1;  // and stdout is a pipe, so it has no width either
    double padded = fmax(1, c - 4);  // Claude Code pads the status line by 2 columns on each side
    if (padded >= 1e21) return 1;    // String() turns it into exponent notation, which parses as 1
    return (int)fmin(floor(padded), 1000);
}

// ---- index.js ----

static bool hud_disabled(void) {
    us v = us_lower(us_trim(env_get("CLAUDE_HUD_DISABLE")));
    return v.n && !us_eq_c(v, "0") && !us_eq_c(v, "false") && !us_eq_c(v, "off") && !us_eq_c(v, "no");
}

static bool context_unreported(jv root) {
    jv cw = jget(root, "context_window"), native = jget(cw, "used_percentage"), u = jget(cw, "current_usage");
    static const char *const f[] = {"input_tokens", "cache_creation_input_tokens", "cache_read_input_tokens"};
    double total = 0;
    for (size_t i = 0; i < countof(f); i++) {
        jv x = jget(u, f[i]);
        total += jis_num(x) ? jnum(x) : 0;
    }
    return !(jis_num(native) && jnum(native) > 0) && total == 0;
}

static bool needs_transcript(Config *c, jv root) {
    auto d = &c->d;
    return d->show_tools || d->show_skills || d->show_mcp || d->show_agents || d->show_todos || d->show_config_counts ||
           d->show_session_tokens || d->show_compactions || d->show_advisor || d->show_session_start || d->show_last_response ||
           d->show_effort || d->model_source != MS_STDIN || context_unreported(root);
}

// resolveVcsStatus: jj wins in a repo that has one and opts in; git is the fallback.
static VcsStatus *vcs_status(Config *c, jv root) {
    jv cwd = jget(root, "cwd");
    if (!jtruthy(cwd)) return nullptr;
    if (!jis_str(cwd)) {
        UNSUPPORTED("non-string cwd");
        return nullptr;
    }
    if (c->jj.enabled && is_jj_repo(jstr(cwd))) {
        VcsStatus *st = jj_status(jstr(cwd));
        if (st) return st;
    }
    return c->git.enabled ? git_status(jstr(cwd), jget(jget(root, "workspace"), "repo"), c->git.show_file_stats) : nullptr;
}

#if !defined(HUD_C_TEST)
// " <N>ms" in green on the last line, up to the moment before writing; where it starts is
// os_run_ms's. Not upstream output, hence left out of the test build.
static void append_run_time(sb *out) {
    if (out->n && out->p[out->n - 1] == '\n') out->n--;
    sb_us(out, US(" \x1b[32m"));
    sb_int(out, (int64_t)(os_run_ms() + 0.5));
    sb_us(out, US("ms\x1b[0m\n"));
}
#endif

static bool has_extra_cmd(void) {
    us *args;
    int n = os_args(&args);
    for (int i = 0; i < n; i++)
        if (us_eq_c(args[i], "--extra-cmd") || us_starts_c(args[i], "--extra-cmd=")) return true;
    return false;
}

static void run(const char *buf, size_t n, sb *out) {
    if (hud_disabled()) return;
    if (!init_locale()) UNSUPPORTED("locale or time zone not reproduced");
    Ctx ctx = {.now = now_ms()};
    ctx.home = os_home();
    ctx.claude_dir = claude_dir_of(ctx.home);
    Config cfg;
    ctx.cfg = &cfg;

    // readStdin: JSON.parse of at most 256K characters; nothing usable prints the init line
    jv root = nullptr;
    if (n && us_utf8_len(buf, n) <= 256 * 1024) root = parse_json(buf, n);
    load_config(&cfg, ctx.claude_dir, ctx.home);
    if (!root || !(yyjson_is_obj(root) || yyjson_is_arr(root))) {
        sb_us(out, US("[claude-hud] Initializing...\n"));
        if (OS_INIT_NOTE) sb_c(out, OS_INIT_NOTE), sb_ch(out, '\n');
        return;
    }
    ctx.input = root;
    if (has_extra_cmd() && us_trim(env_get("CLAUDE_HUD_ALLOW_EXTRA_CMD")).n) UNSUPPORTED("--extra-cmd");

    Transcript tr = {.session_start = NAN, .last_assistant_at = NAN, .ultracode = -1};
    if (needs_transcript(&cfg, root)) {
        jv tp = jget(root, "transcript_path");
        if (jis_str(tp)) parse_transcript(jstr(tp), &tr);
    }
    ctx.t = &tr;
    ctx.vcs = vcs_status(&cfg, root);
    if (cfg.d.show_memory && !cfg.compact) memory_usage(&ctx);
    gather_usage(&ctx);
    if (cfg.d.show_config_counts) count_configs(&ctx);
    if (cfg.d.show_daily_cost || cfg.d.show_weekly_cost) cost_totals(&ctx);
    if (cfg.d.show_speed) output_speed(&ctx);
    if (cfg.d.show_auth || cfg.d.show_auth_user) read_auth(&ctx);

    // frame.js
    ctx.columns = terminal_width();
    ctx.width = cfg.force_max_width && cfg.max_width >= 0 ? cfg.max_width : ctx.columns >= 0 ? ctx.columns : cfg.max_width;
    ctx.bar_width = ctx.columns < 0 || ctx.columns >= 100 ? 10 : ctx.columns >= 60 ? 6 : 4;
    render(&ctx, out);
}

#if defined(HUD_C_TRACE)
// Observation build only. Beside the executable, so nothing has to be configured: the trace log gets
// one line per refresh (time, duration, and what was not reproduced exactly), and capture
// keeps each stdin for test/compare/replay.mjs.
static us beside_exe(const char *name) { return path_join(2, os_exe_dir(), us_c(name)); }
#endif

int hud_main(void) {
#if defined(HUD_C_TRACE)
    double t0 = os_run_ms();
#endif
    // Read stdin to EOF, so the writer never sees a broken pipe; claude-hud's 250 ms first-byte
    // wait is moot for a piped request.
    size_t n;
    char *buf = os_read_stdin(&n);
#if defined(HUD_C_TRACE)
    // Whole milliseconds since the epoch: a name and a timestamp, not a measurement.
    int64_t wall = (int64_t)os_now_ms();
    us capture = beside_exe("capture");
    os_mkdir(capture);
    us name = us_cat(4, us_int(wall), US("-"), us_int(os_pid()), US(".json"));
    write_file(path_join(2, capture, name), us_utf8(buf, n), 0, WRITE_NEW);
#endif
    sb out = {0};
    run(buf, n, &out);
#if defined(HUD_C_TEST)
    // The comparison scripts count these separately: the output may differ from upstream.
    if (g_unsupported) {
        write_err("unsupported: ");
        write_err(g_unsupported);
        write_err("\r\n");
        return 3;
    }
#else
    if (out.n) append_run_time(&out);
#endif
    size_t len;
    char *u8 = us_to_utf8(sb_done(&out), &len);
    os_write_out(u8, len);
#if defined(HUD_C_TRACE)
    // The duration in tenths of a millisecond, then shown with one decimal.
    int64_t tenths = (int64_t)((os_run_ms() - t0) * 10 + 0.5);
    sb line = {0};
    sb_int(&line, wall), sb_ch(&line, ' ');
    sb_int(&line, tenths / 10), sb_ch(&line, '.'), sb_int(&line, tenths % 10);
    sb_us(&line, US(" transcript=")), sb_c(&line, g_transcript_state), sb_ch(&line, ' ');
    if (g_unsupported) sb_us(&line, US("unsupported: ")), sb_c(&line, g_unsupported);
    else sb_us(&line, US("ok"));
    sb_ch(&line, '\n');
    size_t ln;
    char *l8 = us_to_utf8(sb_done(&line), &ln);
    os_append(beside_exe("claude-hud-c-trace.log"), l8, ln);
#endif
    return 0;
}