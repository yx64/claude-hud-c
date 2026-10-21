#pragma once
#include "base.h"

// Insertion-ordered map with JS Map semantics: overwriting keeps the position, delete leaves a
// hole that iteration skips. Keys are raw bytes.
typedef struct { const char *k; uint32_t kn; bool alive; int32_t i; double d[4]; } om_ent;
typedef struct { om_ent *e; int32_t n, cap, alive, head; int32_t *slot; int32_t nslot; } omap;
om_ent *om_get(omap *m, const char *k, uint32_t kn);
om_ent *om_put(omap *m, const char *k, uint32_t kn, bool *created);
void om_del(omap *m, om_ent *e);
om_ent *om_oldest(omap *m);

// ---- transcript.js ----

enum { ST_RUNNING, ST_COMPLETED, ST_ERROR };
enum { TD_PENDING, TD_IN_PROGRESS, TD_COMPLETED };

// No presence flags: a string is empty where upstream has no usable string, and a time is NaN
// while unknown.
typedef struct { us name, target; int status; } Tool;

typedef struct {
    us type, desc, model;
    bool background;
    int status;
    double start, end;
} Agent;

typedef struct { us content; int status; } Todo;

typedef struct {
    Tool *tools;
    int ntools;
    us *skills, *mcp_servers, *mcp_errors;
    int nskills, nmcp, nmcp_err;
    Agent *agents;
    int nagents;
    Todo *todos;
    int ntodos;
    double session_start, last_assistant_at;  // NaN when absent
    us advisor, last_model;
    bool ultracode;
    int compactions;
    double context_tokens;
    double tok_in, tok_out, tok_cc, tok_cr;
} Transcript;

void parse_transcript(us path, double now, Transcript *t);  // now: the refresh's clock, for lines without a time
#if defined(HUD_C_TRACE)
extern const char *g_transcript_state;  // how parse_transcript used and kept its saved state, for the trace log
#endif
bool sanitize_model(jv v, us *out);  // model-source.js sanitizeTranscriptModel

// ---- config.js mergeConfig ----

enum { EL_PROJECT, EL_ADDED_DIRS, EL_CONTEXT, EL_USAGE, EL_PROMPT_CACHE, EL_CACHE_HIT_RATE, EL_MEMORY, EL_ENVIRONMENT,
       EL_TOOLS, EL_SKILLS, EL_MCP, EL_AGENTS, EL_TODOS, EL_SESSION_TIME, EL_COUNT };
enum { SEG_MODEL, SEG_PROJECT, SEG_ADVISOR, SEG_SESSION_NAME, SEG_VERSION, SEG_EXTRA, SEG_DURATION, SEG_COST,
       SEG_SPEED, SEG_AUTH, SEG_COUNT };
enum { CV_PERCENT, CV_TOKENS, CV_REMAINING, CV_BOTH };
enum { TF_RELATIVE, TF_ABSOLUTE, TF_BOTH, TF_ELAPSED, TF_ELAPSED_ABSOLUTE };
enum { HC_AUTO, HC_H11, HC_H12, HC_H23, HC_H24 };
enum { MF_FULL, MF_COMPACT, MF_SHORT };
enum { MS_AUTO, MS_STDIN, MS_TRANSCRIPT };
enum { EF_FULL, EF_SYMBOL, EF_TEXT };
enum { LANG_EN };  // the string table in render.c has one column per language

typedef struct {
    int lang;
    bool compact, show_separators, force_max_width;
    int path_levels;  // 1..3, 0 for "full"
    int max_width;    // -1 for null
    int elem[EL_COUNT], nelem;
    int plo[SEG_COUNT], nplo;
    int group[EL_COUNT][EL_COUNT], group_len[EL_COUNT], ngroups;
    bool right_align[EL_COUNT];
    struct {
        bool enabled, show_dirty, show_ahead_behind, show_file_stats, show_worktree, wrap;
        double push_warn, push_crit;
    } git;
    struct { bool enabled, show_dirty, show_conflicts; } jj;
    struct {
        bool show_model, show_project, show_added_dirs, added_dirs_line, show_context_bar;
        int context_value;
        bool show_config_counts, show_cost, show_routed_cost, show_daily_cost, show_weekly_cost, show_duration, show_speed;
        bool show_token_breakdown, show_usage, usage_remaining, usage_bar, show_reset_label, usage_compact, show_model_scoped, usage_pace;
        bool show_tools, show_skills, show_mcp, show_agents, show_todos, show_session_name, show_auth, show_auth_user;
        double tool_name_max, tools_max_visible, skills_max_visible, auth_user_length;
        bool show_cc_version, show_effort, show_memory, show_prompt_cache, show_cache_hit_rate, show_session_tokens;
        bool show_output_style, show_session_start, show_last_response, show_compactions;
        int effort_format;
        double ctx_warn, ctx_crit, usage_threshold, seven_day_threshold, env_threshold;
        us external_usage_path, external_usage_write_path;
        double external_usage_freshness;
        int model_format, model_source, time_format, hour_cycle;
        us model_override, provider_name, custom_line, advisor_override;
        bool show_provider, custom_first, show_clock_seconds, show_advisor;
        double auto_compact_window;  // 0 for null
    } d;
    struct {
        us context, usage, warning, usage_warning, critical, model, project, git, git_branch, label, custom, bar_filled, bar_empty;
    } c;
} Config;

void load_config(Config *cfg, us claude_dir, us home);

// ---- render context ----

typedef struct { us label; double percent, reset; } Scoped;  // NaN for null
typedef struct {
    double five, seven, five_reset, seven_reset;  // NaN for null
    bool has_balance;
    us balance;
    Scoped *scoped;
    int nscoped;
} Usage;

enum { TF_ADDED, TF_DELETED, TF_MODIFIED };
typedef struct { us basename, full; int type; bool has_diff; double added, deleted; } GitFile;
typedef struct {
    bool jj;
    us branch, branch_url;  // branch_url empty when there is none
    bool dirty, conflict;
    double ahead, behind;
    bool has_file_stats, has_line_diff;
    double modified, added, deleted, untracked;
    GitFile *files;
    int nfiles;
    double line_added, line_deleted;
} VcsStatus;

typedef struct {
    jv input;  // the stdin JSON
    Transcript *t;
    Config *cfg;
    int claude_md, rules, mcp, hooks;
    bool has_costs, has_week;
    double today, week;
    bool has_usage;
    Usage usage;
    bool has_auth_method, has_auth_user;
    us auth_method, auth_user;
    bool has_memory;
    double mem_total, mem_used, mem_percent;
    bool has_speed;
    double speed;
    VcsStatus *vcs;  // null when there is none
    double now;
    int columns;    // the terminal width, -1 when unknown
    int width;      // the frame width, -1 for null
    int bar_width;
    us claude_dir, home;
} Ctx;

// ---- data.c: what main() gathers before rendering ----

void count_configs(Ctx *ctx);
void read_auth(Ctx *ctx);
void gather_usage(Ctx *ctx);
void cost_totals(Ctx *ctx);  // after gather_usage: the weekly window comes from it
void memory_usage(Ctx *ctx);
void output_speed(Ctx *ctx);
bool native_cost(Ctx *ctx, double *out);

void render(Ctx *ctx, sb *out);
double js_round(double x);
double to_percent(double v);  // rounded, then clamped to 0..100
bool is_bedrock_id(jv id);
bool is_vertex_id(jv id);
bool init_locale(void);

// ---- vcs.c: git.js and jj.js ----

VcsStatus *git_status(us cwd, jv repo, bool line_diffs);
bool is_jj_repo(us cwd);
VcsStatus *jj_status(us cwd);
