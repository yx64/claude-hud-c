// Port of claude-hud dist/transcript.js parseTranscript().
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#endif

#include "transcript-state.h"

enum { TOOLS_KEPT = 20, AGENTS_KEPT = 10, TOOLS_TRACKED = 256 };
// The piece of a transcript read at a time (parse_transcript). The test build's is small and odd, so that the
// cases' lines straddle pieces and outgrow the buffer.
#if defined(HUD_C_TEST)
enum { READ_CHUNK = 4093 };
#else
enum { READ_CHUNK = 1 << 20 };
#endif

// ---- ordered map ----

static uint32_t hash_bytes(const char *k, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ (uint8_t)k[i]) * 16777619u;
    return h;
}

static void om_rehash(omap *m) {
    int32_t want = 16;
    while (want < m->alive * 4 + 4) want *= 2;
    m->slot = arena_alloc((size_t)want * sizeof(int32_t));
    for (int32_t i = 0; i < want; i++) m->slot[i] = -1;
    m->nslot = want;
    for (int32_t i = 0; i < m->n; i++) {
        if (!m->e[i].alive) continue;
        uint32_t h = hash_bytes(m->e[i].k, m->e[i].kn) & (uint32_t)(want - 1);
        while (m->slot[h] != -1) h = (h + 1) & (uint32_t)(want - 1);
        m->slot[h] = i;
    }
}

static int32_t *om_slot(omap *m, const char *k, uint32_t kn) {
    if (!m->nslot) return nullptr;
    uint32_t h = hash_bytes(k, kn) & (uint32_t)(m->nslot - 1);
    for (;;) {
        int32_t s = m->slot[h];
        if (s == -1) return nullptr;
        if (s >= 0 && m->e[s].kn == kn && memcmp(m->e[s].k, k, kn) == 0) return &m->slot[h];
        h = (h + 1) & (uint32_t)(m->nslot - 1);
    }
}

om_ent *om_get(omap *m, const char *k, uint32_t kn) {
    int32_t *s = om_slot(m, k, kn);
    return s ? &m->e[*s] : nullptr;
}

om_ent *om_put(omap *m, const char *k, uint32_t kn, bool *created) {
    om_ent *e = om_get(m, k, kn);
    if (created) *created = !e;
    if (e) return e;
    if (m->n == m->cap) {
        int32_t cap = m->cap ? m->cap * 2 : 16;
        om_ent *ne = arena_alloc((size_t)cap * sizeof(om_ent));
        if (m->n) memcpy(ne, m->e, (size_t)m->n * sizeof(om_ent));
        m->e = ne, m->cap = cap;
    }
    e = &m->e[m->n];
    *e = (om_ent){.k = arena_dup(k, kn), .kn = kn, .alive = true};
    m->n++, m->alive++;
    // Tombstones count against the load factor too, so rebuild generously.
    if (!m->nslot || (m->n - m->head) * 2 >= m->nslot) om_rehash(m);
    else {
        uint32_t h = hash_bytes(k, kn) & (uint32_t)(m->nslot - 1);
        while (m->slot[h] >= 0) h = (h + 1) & (uint32_t)(m->nslot - 1);
        m->slot[h] = m->n - 1;
    }
    return e;
}

void om_del(omap *m, om_ent *e) {
    int32_t *s = om_slot(m, e->k, e->kn);
    if (s) *s = -2;
    e->alive = false;
    m->alive--;
}

om_ent *om_oldest(omap *m) {
    while (m->head < m->n && !m->e[m->head].alive) m->head++;
    return m->head < m->n ? &m->e[m->head] : nullptr;
}

// The entries still alive, in order, under a new slot table; what each old entry's index became, -1 for
// one dropped.
static int32_t *om_compact(omap *m) {
    int32_t *to = arena_alloc(sizeof(int32_t) * (size_t)(m->n + 1));
    int32_t k = 0;
    for (int32_t i = 0; i < m->n; i++) {
        to[i] = m->e[i].alive ? k : -1;
        if (m->e[i].alive) m->e[k++] = m->e[i];
    }
    m->n = k, m->head = 0;
    om_rehash(m);
    return to;
}

// ---- keys with Map's SameValueZero: the type is part of the key ----

static void str_key(us s, const char **k, uint32_t *kn) {
    char *p = arena_alloc((size_t)s.n * 2 + 1);
    p[0] = 's';
    if (s.n) memcpy(p + 1, s.p, (size_t)s.n * 2);
    *k = p, *kn = (uint32_t)s.n * 2 + 1;
}

// objects numbers the keys of non-scalar ids; it is part of the parser state.
static void val_key(jv v, unsigned *objects, const char **k, uint32_t *kn) {
    if (jis_str(v)) return str_key(jstr(v), k, kn);
    char buf[48];
    char *e = buf + 1;
    if (jis_num(v)) {  // the digits of a number are ASCII: one byte each
        sb b = {0};
        sb_num(&b, jnum(v) == 0 ? 0 : jnum(v));
        buf[0] = 'n';
        for (int32_t i = 0; i < b.n; i++) *e++ = (char)b.p[i];
    } else if (v && yyjson_is_bool(v)) buf[0] = 'b', *e++ = yyjson_get_bool(v) ? '1' : '0';
    else if (v && yyjson_is_null(v)) buf[0] = 'z';
    else buf[0] = 'o', e = put_int(e, ++*objects, 1);  // an object is only ever equal to itself
    *k = arena_dup(buf, (size_t)(e - buf)), *kn = (uint32_t)(e - buf);
}

// ---- small helpers ----

#define VPUSH(v, x)                                                                   \
    do {                                                                              \
        if ((v).n == (v).cap) {                                                       \
            int32_t c_ = (v).cap ? (v).cap * 2 : 16;                                  \
            void *q_ = arena_alloc((size_t)c_ * sizeof(*(v).p));                      \
            if ((v).n) memcpy(q_, (v).p, (size_t)(v).n * sizeof(*(v).p));            \
            (v).p = q_, (v).cap = c_;                                                 \
        }                                                                             \
        (v).p[(v).n++] = (x);                                                         \
    } while (0)

// The elements of v that m's entries reach, in the entries' order, each entry renumbered to its own.
#define VCOMPACT(m, v)                                                                \
    do {                                                                              \
        typeof((v).p) q_ = arena_alloc(sizeof *q_ * (size_t)((m).n + 1));             \
        for (int32_t i_ = 0; i_ < (m).n; i_++)                                        \
            q_[i_] = (v).p[(m).e[i_].i], (m).e[i_].i = i_;                            \
        (v).p = q_, (v).n = (v).cap = (m).n;                                          \
    } while (0)

// A deleted entry is never reused, so without this a saved state would grow with every tool and response
// of the session, though only the last ones are tracked: the maps lose their deleted entries, and toolv,
// agentv and errv the elements only those reached. Nothing else changes: the order of what is left, the
// lookups, and what the next deletion takes.
void parser_compact(Parser *s) {
    if (s->tools.alive < s->tools.n) {
        om_compact(&s->tools);
        VCOMPACT(s->tools, s->toolv);
    }
    if (s->mcp_err.alive < s->mcp_err.n) {
        om_compact(&s->mcp_err);
        VCOMPACT(s->mcp_err, s->errv);
    }
    // agent_ids holds indices of agents' entries, so the agents are renumbered only while every id still
    // tracked reaches an agent still tracked. Deleting an agent deletes its ids, so that always holds; the
    // check only guards it.
    bool ids_live = true;
    for (int32_t i = 0; i < s->agent_ids.n; i++)
        if (s->agent_ids.e[i].alive && !s->agents.e[s->agent_ids.e[i].i].alive) ids_live = false;
    if (ids_live && s->agents.alive < s->agents.n) {
        int32_t *to = om_compact(&s->agents);
        VCOMPACT(s->agents, s->agentv);
        for (int32_t i = 0; i < s->agent_ids.n; i++)
            if (s->agent_ids.e[i].alive) s->agent_ids.e[i].i = to[s->agent_ids.e[i].i];
    }
    // Nothing refers to the entries of these by position (skills and mcp delete none, so nothing changes
    // for them).
    omap *plain[] = {&s->skills, &s->mcp, &s->completions, &s->usage, &s->task_idx, &s->agent_ids};
    for (size_t i = 0; i < countof(plain); i++)
        if (plain[i]->alive < plain[i]->n) om_compact(plain[i]);
}

static double count(jv v) {
    if (!jis_num(v)) return 0;
    double d = jnum(v);
    if (!isfinite(d)) return 0;
    d = trunc(d);
    return d > 0 ? d : 0;
}

bool sanitize_model(jv v, us *out) {
    if (!jis_str(v)) return false;
    us s = truncate_string(us_trim(sanitize_display(jstr(v))), 80, US_EMPTY);
    if (!s.n) return false;
    *out = s;
    return true;
}

// name(): sanitized, trimmed, at most 64 code units
static bool short_name(jv v, us *out) {
    if (!jis_str(v)) return false;
    us s = us_trim(sanitize_display(jstr(v)));
    if (!s.n) return false;
    *out = truncate_string(s, 64, US("…"));
    return true;
}

// text(): sanitized, undefined when that leaves nothing
static bool text_of(jv v, us *out) {
    if (!jis_str(v)) return false;
    us s = sanitize_display(jstr(v));
    if (!s.n) return false;
    *out = s;
    return true;
}

static bool is_line_term(uint16_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

// /^mcp__(.+?)__(.+)$/ on String(toolName), then name() on the server part. String() of
// anything but a string never starts with "mcp__".
static bool mcp_server(us name, us *out) {
    if (!us_starts_c(name, "mcp__")) return false;
    for (int32_t i = 0; i < name.n; i++)
        if (is_line_term(name.p[i])) return false;
    for (int32_t i = 6; i + 2 < name.n; i++) {
        if (name.p[i] == '_' && name.p[i + 1] == '_') {
            us s = us_trim(sanitize_display(us_slice(name, 5, i)));
            if (!s.n) return false;
            *out = truncate_string(s, 64, US("…"));
            return true;
        }
    }
    return false;
}

static int task_status(jv v) {  // -1 for null
    static const struct { const char *name; int status; } map[] = {
        {"pending", TD_PENDING},     {"not_started", TD_PENDING}, {"in_progress", TD_IN_PROGRESS}, {"running", TD_IN_PROGRESS},
        {"completed", TD_COMPLETED}, {"complete", TD_COMPLETED},  {"done", TD_COMPLETED}};
    if (!jis_str(v)) return -1;
    for (size_t i = 0; i < countof(map); i++)
        if (jstr_eq(v, map[i].name)) return map[i].status;
    return -1;
}

// <tag>([^<]+)</tag>, first match
static bool tag_match(const char *s, size_t n, const char *open, const char *close, const char **mp, size_t *mn) {
    size_t ol = strlen(open), cl = strlen(close);
    for (size_t i = 0; i + ol <= n; i++) {
        if (memcmp(s + i, open, ol) != 0) continue;
        size_t j = i + ol;
        while (j < n && s[j] != '<') j++;
        if (j > i + ol && j + cl <= n && memcmp(s + j, close, cl) == 0) {
            *mp = s + i + ol, *mn = j - i - ol;
            return true;
        }
    }
    return false;
}

static int32_t u16len(jv v) {  // .length of a string value
    const uint8_t *p = (const uint8_t *)yyjson_get_str(v);
    size_t n = yyjson_get_len(v);
    int32_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) k++;
        if (p[i] >= 0xF0) k++;
    }
    return k;
}

static void set_add(omap *m, StrVec *vec, us v) {
    const char *k;
    uint32_t kn;
    str_key(v, &k, &kn);
    bool created;
    om_ent *e = om_put(m, k, kn, &created);
    if (created) {
        e->i = vec->n;
        VPUSH(*vec, v);
    }
}

static bool tool_target(jv name, jv input, us *out) {
    if (!jtruthy(input)) return false;
    if (jstr_eq(name, "Read") || jstr_eq(name, "Write") || jstr_eq(name, "Edit")) {
        jv v = jget(input, "file_path");
        return text_of(jis_nullish(v) ? jget(input, "path") : v, out);
    }
    if (jstr_eq(name, "Glob") || jstr_eq(name, "Grep")) return text_of(jget(input, "pattern"), out);
    if (jstr_eq(name, "Skill")) return short_name(jget(input, "skill"), out);
    if (jstr_eq(name, "Bash")) {
        jv v = jget(input, "command");
        if (!jis_str(v)) return false;
        const char *raw = yyjson_get_str(v);
        size_t rn = yyjson_get_len(v);
        // Only the first 30 code units are shown, and a command can run to pages, so it is taken a prefix at a
        // time, longer each round. A prefix that comes out longer than 30 begins as the whole command does,
        // graphemes included: dropping a character and collapsing white space look at nothing after it, and
        // what was cut is trimmed off the end. Not so for an escape sequence that may run past the cut, so a
        // command with an ESC is taken whole; a cut inside a character moves back to where it starts.
        bool esc = memchr(raw, 0x1B, rn) != nullptr;
        for (size_t want = 256;; want *= 2) {
            size_t take = esc || want >= rn ? rn : want;
            while (take < rn && take && ((uint8_t)raw[take] & 0xC0) == 0x80) take--;
            if (!take) take = rn;
            us s = sanitize_display(us_utf8(raw, take));
            sb b = {0};
            for (int32_t i = 0; i < s.n;) {
                if (js_is_space(s.p[i])) {
                    while (i < s.n && js_is_space(s.p[i])) i++;
                    sb_ch(&b, ' ');
                } else sb_ch(&b, s.p[i++]);
            }
            us cmd = us_trim(sb_done(&b));
            if (take < rn && cmd.n <= 30) continue;
            if (!cmd.n) return false;
            *out = cmd.n > 30 ? us_cat(us_trim_end(truncate_string(cmd, 30, US_EMPTY)), US("...")) : cmd;
            return true;
        }
    }
    return false;
}

// Whether find_task falls back to a position for this id: a string of digits only.
static bool digit_key(const om_ent *e) {
    if (e->k[0] != 's' || e->kn < 3) return false;
    for (uint32_t j = 1; j < e->kn; j += 2) {
        uint16_t c;
        memcpy(&c, e->k + j, 2);
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// moveTasks: points each live task id at its todo's new index, to[old index], or retires it with -1 so it
// can't fall back to whichever task now holds its old position. Only a digit id could fall back, so any
// other is deleted instead: it finds nothing all the same, and a saved state does not keep it.
static void move_tasks(Parser *s, const int32_t *to) {
    for (int32_t i = 0; i < s->task_idx.n; i++) {
        om_ent *e = &s->task_idx.e[i];
        if (!e->alive || e->i < 0) continue;
        e->i = to[e->i];
        if (e->i < 0 && !digit_key(e)) om_del(&s->task_idx, e);
    }
}

// TodoWrite replaces the list. Each todo takes over the ids of the first unclaimed old todo with the same
// content, so duplicate-content todos each keep their own; dropped todos' ids retire.
static void replace_todos(Parser *s, TodoVec next) {
    int32_t *to = arena_alloc(sizeof(int32_t) * (size_t)(s->todos.n + 1));
    for (int32_t j = 0; j < s->todos.n; j++) to[j] = -1;
    int32_t lo = 0;  // the old todos before it are all claimed: a list written again in order scans none twice
    for (int32_t i = 0; i < next.n; i++) {
        while (lo < s->todos.n && to[lo] >= 0) lo++;
        for (int32_t j = lo; j < s->todos.n; j++) {
            if (to[j] < 0 && us_eq(s->todos.p[j].content, next.p[i].content)) {
                to[j] = i;
                break;
            }
        }
    }
    s->todos = next;
    move_tasks(s, to);
}

// findTask: the id TaskCreate returned, or a 1-based position; a retired id finds nothing
static int32_t find_task(Parser *s, jv id) {
    sb b = {0};
    if (jis_str(id)) sb_us(&b, jstr(id));
    else if (jis_num(id)) sb_num(&b, jnum(id));
    else return -1;
    us key = sb_done(&b);
    const char *k;
    uint32_t kn;
    str_key(key, &k, &kn);
    om_ent *e = om_get(&s->task_idx, k, kn);
    if (e) return e->i;
    if (!key.n) return -1;
    double v = 0;
    for (int32_t i = 0; i < key.n; i++) {
        if (key.p[i] < '0' || key.p[i] > '9') return -1;
        v = v * 10 + (key.p[i] - '0');
    }
    v -= 1;
    return v >= 0 && v < s->todos.n ? (int32_t)v : -1;
}

static void tool_use(Parser *s, jv block, double at) {
    jv name = jget(block, "name"), input = jget(block, "input"), id = jget(block, "id");
    us v;
    if (jstr_eq(name, "Skill") && short_name(jget(input, "skill"), &v)) set_add(&s->skills, &s->skillv, v);
    if (jis_str(name) && mcp_server(jstr(name), &v)) set_add(&s->mcp, &s->mcpv, v);
    const char *k;
    uint32_t kn;
    val_key(id, &s->objects, &k, &kn);
    if (jstr_eq(name, "Task") || jstr_eq(name, "Agent")) {
        Agent a = {.status = ST_RUNNING, .start = at, .end = NAN};
        jv st = jget(input, "subagent_type"), d = jget(input, "description");
        if (jis_nullish(st)) a.type = US("agent");
        else if (jis_str(st)) a.type = jstr(st);
        if (jis_str(d)) a.desc = jstr(d);
        sanitize_model(jget(input, "model"), &a.model);
        a.background = jis_true(jget(input, "run_in_background"));
        bool created;
        om_ent *en = om_put(&s->agents, k, kn, &created);
        if (created) {
            en->i = s->agentv.n;
            VPUSH(s->agentv, a);
        } else s->agentv.p[en->i] = a;
        if (s->agents.alive > AGENTS_KEPT) {
            om_del(&s->agents, om_oldest(&s->agents));
            // Completions and launch ids only matter for agents still tracked.
            for (int32_t i = 0; i < s->completions.n; i++) {
                om_ent *c = &s->completions.e[i];
                if (c->alive && !om_get(&s->agents, c->k, c->kn)) om_del(&s->completions, c);
            }
            for (int32_t i = 0; i < s->agent_ids.n; i++) {
                om_ent *c = &s->agent_ids.e[i];
                if (c->alive && !s->agents.e[c->i].alive) om_del(&s->agent_ids, c);
            }
        }
    } else if (jstr_eq(name, "TodoWrite")) {
        jv todos = jget(input, "todos");
        if (!jis_arr(todos)) return;
        TodoVec next = {0};
        size_t idx, max;
        jv t;
        yyjson_arr_foreach(todos, idx, max, t) {
            us content;
            int status = task_status(jget(t, "status"));
            if (text_of(jget(t, "content"), &content) && status >= 0) VPUSH(next, ((Todo){content, status}));
        }
        replace_todos(s, next);
    } else if (jstr_eq(name, "TaskCreate")) {
        us content;
        if (!text_of(jget(input, "subject"), &content) && !text_of(jget(input, "description"), &content)) content = US("Untitled task");
        int status = task_status(jget(input, "status"));
        VPUSH(s->todos, ((Todo){content, status >= 0 ? status : TD_PENDING}));
        int32_t last = s->todos.n - 1;
        // Claude Code numbers TaskCreate tasks 1, 2, 3... and never reuses a number.
        us numbered = us_int(++s->tasks_created);
        jv raw = jget(input, "taskId");
        us tid = US_EMPTY;
        if (jis_str(raw) || jis_num(raw)) {
            sb b = {0};
            if (jis_str(raw)) sb_us(&b, jstr(raw));
            else sb_num(&b, jnum(raw));
            tid = sb_done(&b);
        }
        const char *tk;
        uint32_t tkn;
        str_key(tid.n ? tid : numbered, &tk, &tkn);
        om_put(&s->task_idx, tk, tkn, nullptr)->i = last;
        if (!tid.n && jtruthy(id)) om_put(&s->task_idx, k, kn, nullptr)->i = last;
    } else if (jstr_eq(name, "TaskUpdate")) {
        int32_t i = find_task(s, jget(input, "taskId"));
        if (i < 0) return;
        if (jstr_eq(jget(input, "status"), "deleted")) {
            // Drop the task, retire its ids, and shift later tasks' ids up one.
            int32_t *to = arena_alloc(sizeof(int32_t) * (size_t)s->todos.n);
            for (int32_t j = 0; j < s->todos.n; j++) to[j] = j == i ? -1 : j > i ? j - 1 : j;
            memmove(&s->todos.p[i], &s->todos.p[i + 1], sizeof *s->todos.p * (size_t)(s->todos.n - i - 1));
            s->todos.n--;
            move_tasks(s, to);
            return;
        }
        int status = task_status(jget(input, "status"));
        if (status >= 0) s->todos.p[i].status = status;
        us content;
        if (text_of(jget(input, "subject"), &content) || text_of(jget(input, "description"), &content)) s->todos.p[i].content = content;
    } else {
        Tool tool = {.name = jstr(name), .status = ST_RUNNING};
        if (!jis_str(name)) UNSUPPORTED("non-string tool name");
        tool_target(name, input, &tool.target);
        bool created;
        om_ent *en = om_put(&s->tools, k, kn, &created);
        if (created) {
            en->i = s->toolv.n;
            VPUSH(s->toolv, tool);
        } else s->toolv.p[en->i] = tool;
        // Tracked beyond those shown, so an MCP call's late error still counts, but bounded.
        if (s->tools.alive > TOOLS_TRACKED) om_del(&s->tools, om_oldest(&s->tools));
    }
}

static void tool_result(Parser *s, jv block, jv entry, double at) {
    const char *k;
    uint32_t kn;
    val_key(jget(block, "tool_use_id"), &s->objects, &k, &kn);
    om_ent *te = om_get(&s->tools, k, kn);
    if (te) {
        Tool *tool = &s->toolv.p[te->i];
        bool err = jtruthy(jget(block, "is_error"));
        tool->status = err ? ST_ERROR : ST_COMPLETED;
        us srv;
        if (mcp_server(tool->name, &srv)) {
            const char *sk;
            uint32_t skn;
            str_key(srv, &sk, &skn);
            om_ent *have = om_get(&s->mcp_err, sk, skn);
            if (err) {
                set_add(&s->mcp_err, &s->errv, srv);
                if (s->mcp_err.alive > 64) om_del(&s->mcp_err, om_oldest(&s->mcp_err));
            } else if (have) om_del(&s->mcp_err, have);
        }
    }
    om_ent *ae = om_get(&s->agents, k, kn);
    if (ae) {
        Agent *a = &s->agentv.p[ae->i];
        jv tur = jget(entry, "toolUseResult");
        jv agent_id = jget(tur, "agentId");
        if (jis_str(agent_id) && yyjson_get_len(agent_id)) {
            a->agent_id = jstr(agent_id);
            const char *ik;
            uint32_t ikn;
            str_key(a->agent_id, &ik, &ikn);
            om_put(&s->agent_ids, ik, ikn, nullptr)->i = (int32_t)(ae - s->agents.e);
        }
        us m;
        if (sanitize_model(jget(tur, "resolvedModel"), &m)) a->model = m;
        if (jis_true(jget(tur, "isAsync")) || jstr_eq(jget(tur, "status"), "async_launched")) a->background = true;
        if (!a->background) a->end = at;
    }
}

static void assistant(Parser *s, jv e, double at) {
    Transcript *r = s->r;
    if (!isnan(at)) r->last_assistant_at = at;
    jv adv = jget(e, "advisorModel");
    if (jis_str(adv) && yyjson_get_len(adv)) r->advisor = truncate_string(jstr(adv), 64, US_EMPTY);
    jv msg = jget(e, "message");
    us m;
    if (sanitize_model(jget(msg, "model"), &m) && !us_eq_c(m, "<synthetic>")) r->last_model = m;
    jv raw = jget(msg, "usage");
    if (!jtruthy(raw)) {
        s->has_idless = false;
        return;
    }
    double u[4] = {count(jget(raw, "input_tokens")), count(jget(raw, "output_tokens")),
                   count(jget(raw, "cache_creation_input_tokens")), count(jget(raw, "cache_read_input_tokens"))};
    // A main-conversation request always sends some context, so zero is a placeholder: gateways that write
    // one record per content block give thinking blocks all-zero usage. The last real size stays.
    double context = u[0] + u[2] + u[3];
    if (!jis_true(jget(e, "isSidechain")) && context > 0) r->context_tokens = context;
    jv id = jget(msg, "id");
    if (jis_str(id) && yyjson_get_len(id) && u16len(id) <= 128) {
        s->has_idless = false;
        // Claude Code logs one response several times, so usage is the per-field max per id.
        const char *k = yyjson_get_str(id);
        uint32_t kn = (uint32_t)yyjson_get_len(id);
        bool created;
        om_ent *en = om_put(&s->usage, k, kn, &created);
        for (int i = 0; i < 4; i++) en->d[i] = created ? u[i] : fmax(en->d[i], u[i]);
        if (s->usage.alive > 4096) {
            om_ent *oldest = om_oldest(&s->usage);
            for (int i = 0; i < 4; i++) s->settled[i] += oldest->d[i];
            om_del(&s->usage, oldest);
        }
        return;
    }
    // Without an id, only an identical record right after the previous one is a duplicate.
    bool same = s->has_idless && memcmp(s->idless, u, sizeof u) == 0;
    if (!same)
        for (int i = 0; i < 4; i++) s->settled[i] += u[i];
    memcpy(s->idless, u, sizeof u);
    s->has_idless = true;
}

// A tool line without a time takes the clock of the refresh that parses it, the one the rest of the refresh
// reads too. Upstream takes the time per line as it parses, and keeps it in the state it saves, as this
// parser's saved state keeps it: a refresh resuming past such a line shows the time of the refresh that read it.
static void on_line(Parser *s, jv e, double now) {
    Transcript *r = s->r;
    if (!e || !(yyjson_is_obj(e) || yyjson_is_arr(e))) {
        s->has_idless = false;
        return;
    }
    jv ts = jget(e, "timestamp");
    double at = jtruthy(ts) ? date_from_json(ts) : NAN;
    if (!isnan(at) && isnan(r->session_start)) r->session_start = at;
    jv type = jget(e, "type");
    if (jstr_eq(type, "assistant")) assistant(s, e, at);
    else s->has_idless = false;
    jv msg = jget(e, "message"), content = jget(msg, "content");
    if (jstr_eq(type, "user") && jis_str(content)) {
        static const char pre[] = "<local-command-stdout>Set effort level to ";
        const char *c = yyjson_get_str(content);
        size_t n = yyjson_get_len(content), m = sizeof pre - 1;
        if (n > m && memcmp(c, pre, m) == 0) {
            size_t j = m;
            while (j < n && ((c[j] >= 'a' && c[j] <= 'z') || (c[j] >= 'A' && c[j] <= 'Z') || (c[j] >= '0' && c[j] <= '9') || c[j] == '_')) j++;
            // Without case: the run holds only letters, digits and _, and | 32 lowers only a letter
            // onto one.
            bool ultra = j - m == 9;
            for (size_t k = 0; ultra && k < 9; k++) ultra = (c[m + k] | 32) == "ultracode"[k];
            if (j > m) r->ultracode = ultra;
        }
    }
    if (jstr_eq(type, "attachment")) {
        jv at_type = jget(jget(e, "attachment"), "type");
        if (jstr_eq(at_type, "ultra_effort_enter")) r->ultracode = true;
        if (jstr_eq(at_type, "ultra_effort_exit")) r->ultracode = false;
    }
    if (jstr_eq(type, "system") && jstr_eq(jget(e, "subtype"), "compact_boundary") && !isnan(at)) {
        r->compactions++;
        jv post = jget(jget(e, "compactMetadata"), "postTokens");
        double d = jnum(post);
        r->context_tokens = jis_num(post) && isfinite(d) && d >= 0 ? trunc(d) : 0;
    }
    // A background agent's tool_result lands at launch; its completion is this enqueue, or (forked and
    // resumed sessions) a queued_command attachment carrying the same notice.
    jv notice = nullptr;
    if (jstr_eq(type, "queue-operation") && jstr_eq(jget(e, "operation"), "enqueue")) notice = jget(e, "content");
    else if (jstr_eq(type, "attachment") && jstr_eq(jget(jget(e, "attachment"), "type"), "queued_command")) notice = jget(jget(e, "attachment"), "prompt");
    if (jis_str(notice) && !isnan(at)) {
        const char *c = yyjson_get_str(notice), *task, *tool;
        size_t n = yyjson_get_len(notice), taskn, tooln;
        bool has_task = tag_match(c, n, "<task-id>", "</task-id>", &task, &taskn);
        const char *k = nullptr;
        uint32_t kn = 0;
        // Some notices carry only <task-id>, the agentId of the launch's tool_result.
        if (tag_match(c, n, "<tool-use-id>", "</tool-use-id>", &tool, &tooln)) str_key(us_utf8(tool, tooln), &k, &kn);
        else if (has_task) {
            const char *tk;
            uint32_t tkn;
            str_key(us_utf8(task, taskn), &tk, &tkn);
            om_ent *id = om_get(&s->agent_ids, tk, tkn);
            if (id) k = s->agents.e[id->i].k, kn = s->agents.e[id->i].kn;
        }
        // Both carriers can log the same notice; the earliest is when the agent finished. Background Bash
        // and Monitor tasks send the same notice; only agents' are kept.
        if (k && has_task && om_get(&s->agents, k, kn)) {
            om_ent *prev = om_get(&s->completions, k, kn);
            if (!prev || at < prev->d[0]) om_put(&s->completions, k, kn, nullptr)->d[0] = at;
        }
    }
    // Only real turns carry activity: an api-request-blob line replays the blocks of earlier ones.
    if ((jstr_eq(type, "user") || jstr_eq(type, "assistant")) && jis_arr(content)) {
        double t = isnan(at) ? now : at;
        size_t idx, max;
        jv block;
        yyjson_arr_foreach(content, idx, max, block) {
            jv bt = jget(block, "type");
            bool use = jstr_eq(bt, "tool_use") && jtruthy(jget(block, "id")) && jtruthy(jget(block, "name"));
            bool result = jstr_eq(bt, "tool_result") && jtruthy(jget(block, "tool_use_id"));
            if ((use || result) && isnan(at)) s->took_clock = true;
            if (use) tool_use(s, block, t);
            if (result) tool_result(s, block, e, t);
        }
    }
}

// The first \n at or after i, else n: where a line ends. Sixteen bytes per step; the byte-by-byte scan
// was a fifth of the whole parse.
static size_t line_stop(const char *s, size_t i, size_t n) {
#if defined(__SSE2__)
    const __m128i nl = _mm_set1_epi8('\n');
    for (; i + 16 <= n; i += 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(s + i)), nl));
        if (m) return i + (size_t)__builtin_ctz(m);
    }
#elif defined(__ARM_NEON) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // NEON has no movemask: narrowing each 16-bit lane by a 4-bit shift leaves four mask bits per
    // byte, in byte order, in one 64-bit lane.
    const uint8x16_t nl = vdupq_n_u8('\n');
    for (; i + 16 <= n; i += 16) {
        uint8x16_t eq = vceqq_u8(vld1q_u8((const uint8_t *)(s + i)), nl);
        uint64_t m = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq), 4)), 0);
        if (m) return i + (size_t)(__builtin_ctzll(m) >> 2);
    }
#endif
    while (i < n && s[i] != '\n') i++;
    return i;
}

// readline's line end at or after i, else n: \n, \r, \r\n, U+2028 or U+2029 (E2 80 A8 / E2 80 A9), its
// length in *term. A \r or a lead byte E2 among the last bytes read is undecided until more follow, or
// the file ends (eof): n until then.
static size_t readline_stop(const char *s, size_t i, size_t n, bool eof, size_t *term) {
    for (; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        *term = 1;
        if (c == '\n') return i;
        if (c == '\r') {
            if (i + 1 == n && !eof) return n;
            if (i + 1 < n && s[i + 1] == '\n') *term = 2;
            return i;
        }
        if (c != 0xE2) continue;
        if (i + 2 >= n) {
            if (!eof) return n;
        } else if ((uint8_t)s[i + 1] == 0x80 && ((uint8_t)s[i + 2] == 0xA8 || (uint8_t)s[i + 2] == 0xA9)) {
            *term = 3;
            return i;
        }
    }
    return n;
}

// How many bytes at the end of a file readline never sees: its StringDecoder holds back the
// start of a character that is not complete yet and is never flushed at the end of the stream, so
// a last line cut inside a multi-byte character, U+2028 included, parses without those bytes.
// string_decoder's utf8CheckIncomplete, as it judges lead and continuation bytes: up to three
// bytes back for a lead byte, held when its character needs more than is there.
static size_t held_tail(const char *s, size_t n) {
    for (size_t back = 1; back <= 3 && back <= n; back++) {
        uint8_t c = (uint8_t)s[n - back];
        int need = c <= 0x7F ? 0 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : c >> 3 == 30 ? 4 : c >> 6 == 2 ? -1 : -2;
        if (need == -1) continue;  // a continuation byte
        return need > (int)back ? back : 0;
    }
    return 0;
}

#if defined(HUD_C_TRACE)
const char *g_transcript_state = "unread";
#endif

void parse_transcript(us path, double now, Transcript *r) {
    *r = (Transcript){.session_start = NAN, .last_assistant_at = NAN};
    if (!path.n) return;
    us full = path_resolve(path);
    void *h = open_read(full);
    if (!h) return;
    TranscriptFile tf = {full, h};
    if (!os_identity(h, &tf.volume, &tf.file, &tf.size)) {
        os_close(h);
        return;
    }
    // A case raised before the transcript keeps its precedence; the transcript's own first one is
    // what a saved state carries forward.
    const char *outer = g_unsupported;
    g_unsupported = nullptr;
    Parser s = {.r = r};
    StateSlot slot = {0};
    uint64_t start = 0;
    bool resumed = state_load(&tf, &slot, &s, &start);
    // What follows the state, up to the size the file had when opened, is read a piece at a time into one
    // buffer, the lines parsed dropped before the next piece: memory fresh from the system costs a fault per
    // page, which for a whole transcript of many MB took more than reading it. buf holds n bytes of the file
    // from offset base on; a line longer than the buffer doubles it, and scan is how far it holds no \n.
    uint64_t end = tf.size, base = start, at = start;
    size_t cap = (size_t)(end - start < READ_CHUNK ? end - start : READ_CHUNK) + 1, n = 0, scan = 0;
    char *buf = xmalloc(cap);
    bool eof = false;
    yyjson_read_flag flg = YYJSON_READ_ALLOW_INVALID_UNICODE;
    size_t poolcap = 0;
    void *pool = nullptr;
    bool kept = false, saved = false;
    // Upstream reads the file by bytes and ends a line at \n alone; a \r before it stays in the line, where
    // JSON.parse takes it for white space. Each line is decoded on its own, a character cut at the end of the
    // file included.
    for (size_t i = 0;;) {
        size_t j = scan < n ? line_stop(buf, scan, n) : n;
        if (j == n && !eof) {
            if (i) memmove(buf, buf + i, n - i), base += i, n -= i, i = 0;
            scan = n;
            if (n == cap) buf = xrealloc(buf, cap *= 2);
            size_t want = cap - n < READ_CHUNK ? cap - n : READ_CHUNK;
            if (want > end - at) want = (size_t)(end - at);
            // Cut shorter while being read: what came before is all there is.
            if (!want || !read_at(h, at, buf + n, (uint32_t)want)) eof = true;
            else n += want, at += want;
            continue;
        }
        // The state is kept before the first line that is not complete, the last one, still being written:
        // it counts for this refresh and is read again by the next.
        if (!kept && j == n) {
            uint64_t off = base + i;
            kept = true;
            // Saving a byte of state costs about what parsing a byte of transcript does, so a state taken up is
            // saved again only once what was parsed past it reaches a quarter of its size: until then each
            // refresh parses those lines again, which costs less than the save, and saves every few appends.
            // A line that took the refresh's clock is saved at once, since parsing it again would take
            // another.
            bool defer = resumed && off - start < slot.size / 4 && !s.took_clock;
            if (off && !(resumed && off == start) && !defer) saved = state_save(&tf, &slot, off, buf, base, n, &s);
        }
        if (i == n) break;
        size_t len = j - i;
        size_t need = yyjson_read_max_memory_usage(len, flg);
        if (need > poolcap) {
            free(pool);
            poolcap = need * 2;
            pool = xmalloc(poolcap);
        }
        yyjson_alc alc;
        yyjson_alc_pool_init(&alc, pool, poolcap);
        // raw.trim() ? JSON.parse(raw) : null; an unparsable line is skipped the same way.
        yyjson_doc *doc = len ? yyjson_read_opts(buf + i, len, flg, &alc, nullptr) : nullptr;
        char *fixed = nullptr;
        if (!doc && len) {
            size_t fn;
            fixed = json_fix_surrogates(buf + i, len, &fn);
            if (fixed) {
                yyjson_alc_pool_init(&alc, pool, poolcap);
                doc = yyjson_read_opts(fixed, fn, flg, &alc, nullptr);
            }
        }
        // Everything kept from a line is copied out of the pool, so the next line may reuse it.
        on_line(&s, doc ? yyjson_doc_get_root(doc) : nullptr, now);
        free(fixed);
        i = scan = j < n ? j + 1 : n;
    }
    free(pool);
    free(buf);
    os_close(h);
    if (outer) g_unsupported = outer;
#if defined(HUD_C_TRACE)
    g_transcript_state = resumed ? (saved ? "resumed+saved" : "resumed") : (saved ? "full+saved" : "full");
#endif
    (void)saved;
    for (int32_t i = 0; i < s.completions.n; i++) {
        om_ent *q = &s.completions.e[i];
        om_ent *ae = q->alive ? om_get(&s.agents, q->k, q->kn) : nullptr;
        if (ae && s.agentv.p[ae->i].background) s.agentv.p[ae->i].end = q->d[0];
    }
    // The maps' entries in order, those deleted skipped: the last TOOLS_KEPT tools, every agent tracked.
    int32_t from = s.tools.n;
    for (int k = 0; from > 0 && k < TOOLS_KEPT;) k += s.tools.e[--from].alive;
    r->tools = arena_alloc(sizeof(Tool) * TOOLS_KEPT);
    for (int32_t i = from; i < s.tools.n; i++)
        if (s.tools.e[i].alive) r->tools[r->ntools++] = s.toolv.p[s.tools.e[i].i];
    r->agents = arena_alloc(sizeof(Agent) * AGENTS_KEPT);
    for (int32_t i = 0; i < s.agents.n; i++) {
        if (!s.agents.e[i].alive) continue;
        Agent a = s.agentv.p[s.agents.e[i].i];
        if (!isnan(a.end)) a.status = ST_COMPLETED;
        r->agents[r->nagents++] = a;
    }
    r->skills = s.skillv.p, r->nskills = s.skillv.n;
    r->mcp_servers = s.mcpv.p, r->nmcp = s.mcpv.n;
    r->mcp_errors = arena_alloc(sizeof(us) * (size_t)(s.mcp_err.alive + 1));
    for (int32_t i = 0; i < s.mcp_err.n; i++)
        if (s.mcp_err.e[i].alive) r->mcp_errors[r->nmcp_err++] = s.errv.p[s.mcp_err.e[i].i];
    r->todos = s.todos.p, r->ntodos = s.todos.n;
    double tot[4];
    memcpy(tot, s.settled, sizeof tot);
    for (int32_t i = 0; i < s.usage.n; i++)
        if (s.usage.e[i].alive)
            for (int k = 0; k < 4; k++) tot[k] += s.usage.e[i].d[k];
    r->tok_in = tot[0], r->tok_out = tot[1], r->tok_cc = tot[2], r->tok_cr = tot[3];
}
// ---- addSubagentEfforts ----

us subagent_effort(us path, us agent_id) {
    // The id comes from the transcript and names a file, so only a plain token is used: /^[\w-]{1,64}$/.
    if (!agent_id.n || agent_id.n > 64) return US_EMPTY;
    for (int32_t i = 0; i < agent_id.n; i++) {
        uint16_t c = agent_id.p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return US_EMPTY;
    }
    // <session>/subagents/agent-<id>.jsonl beside <session>.jsonl. path.basename(p, '.jsonl') keeps a
    // name that is only the extension, unless the path is nothing else.
    us full = path_resolve(path);
    int32_t b = full.n;
    while (b > 0 && !is_sep(full.p[b - 1])) b--;
    us name = us_slice(full, b, full.n);
    if (us_eq_c(path, ".jsonl")) name = US_EMPTY;
    else if (name.n > 6 && us_ends_c(name, ".jsonl")) name.n -= 6;
    us file = path_join(dir_of(full), name, US("subagents"), us_cat(US("agent-"), agent_id, US(".jsonl")));
    // Checked before opening, which would block on a pipe.
    fstat_t st = stat_path(file);
    if (!st.ok || !st.is_file) return US_EMPTY;
    void *h = open_read(file);
    uint64_t size;
    if (!h) return US_EMPTY;
    if (!os_size(h, &size)) {
        os_close(h);
        return US_EMPTY;
    }
    // readline over a stream, so the scan stops at the first reply: read on only while no line has one.
    // Before reading on, the lines already scanned are dropped, and the scan resumes where it stopped:
    // only the last two bytes can be undecided, so a long line is not scanned again from its start.
    us effort = US_EMPTY;
    char *buf = nullptr;
    size_t n = 0, cap = 0, i = 0, from = 0;
    uint64_t at = 0;
    for (bool found = false; !found;) {
        bool eof = at == size;
        size_t term, j = readline_stop(buf, from, n, eof, &term);
        if (j == n && !eof) {
            if (i) memmove(buf, buf + i, n - i), n -= i, i = 0;
            from = n > 2 ? n - 2 : 0;
            uint32_t want = (uint32_t)(size - at < 65536 ? size - at : 65536);
            if (n + want > cap) {
                cap = (n + want) * 2;
                buf = xrealloc(buf, cap);
            }
            if (!read_at(h, at, buf + n, want)) break;  // gone mid-read
            n += want, at += want;
            continue;
        }
        size_t len = j - i;
        if (j == n && len) len -= held_tail(buf + i, len);
        jv e = len ? parse_json(buf + i, len) : nullptr;
        if (e && yyjson_is_obj(e) && jstr_eq(jget(e, "type"), "assistant")) {
            jv v = jget(e, "effort");
            if (jis_str(v)) effort = jstr(v);
            found = true;
        }
        if (j == n) break;
        i = from = j + term;
    }
    free(buf);
    os_close(h);
    return effort;
}
