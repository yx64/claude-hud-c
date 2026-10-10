// The parser state kept between refreshes; see transcript-state.h.
//
// Parsing is a fold over lines, and Claude Code only ever appends to a transcript, so the state
// after the last complete line is saved and the next refresh parses only what came after it. A
// saved state is taken up only when this very build wrote it, for the same file (os_identity), at
// an offset the file still reaches, with the same bytes in the file's first window and in
// the window before the offset; anything else parses from the start. A rewrite in the middle,
// outside both windows, would go unseen: that is the append-only assumption.
//
// The state file holds the parser's structures as they are in memory: the hash maps with their
// slot arrays, the vectors, and the strings they point at, each pointer stored as the offset of
// its target in the file. Loading reads the file into one buffer, checks every offset against it,
// and turns the offsets back into pointers; nothing is re-inserted, rehashed or copied string by
// string, and the buffer then is the state. That layout is this build's alone, hence the build
// check.
#if !defined(HUD_C_NO_TRANSCRIPT_STATE)
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "transcript-state.h"

enum { WINDOW = 4096 };
static const uint64_t STATE_MAGIC = 0x3165746174534448;  // "HDState1"
static const char BUILD[] = __DATE__ " " __TIME__;

typedef struct {
    uint64_t magic;
    uint64_t off;  // at byte 8, where test/cache.mjs reads it
    uint64_t volume, file;
    uint64_t head, tail;  // hashes of the transcript's first window and of the window before off
    uint64_t size;        // of the state file
    char build[sizeof BUILD];
    const char *why;  // the first unsupported case among the lines saved, or null
    Parser *parser;
    Transcript *transcript;
} Header;

static_assert(offsetof(Header, off) == 8);

// Every VEC(T) is laid out alike, which lets one routine save and load them all.
typedef VEC(void) AnyVec;
static_assert(sizeof(AnyVec) == sizeof(ToolVec) && offsetof(AnyVec, n) == offsetof(ToolVec, n) && offsetof(AnyVec, cap) == offsetof(ToolVec, cap));
static_assert(sizeof(AnyVec) == sizeof(StrVec) && sizeof(AnyVec) == sizeof(AgentVec) && sizeof(AnyVec) == sizeof(TodoVec));

// Where each kind of element keeps its strings.
static const size_t TOOL_STRS[] = {offsetof(Tool, name), offsetof(Tool, target)};
static const size_t AGENT_STRS[] = {offsetof(Agent, type), offsetof(Agent, desc), offsetof(Agent, model)};
static const size_t TODO_STRS[] = {offsetof(Todo, content)};
static const size_t STR_STRS[] = {0};

static uint64_t fnv1a(const void *p, size_t n) {
    uint64_t h = 14695981039346656037u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 1099511628211u;
    return h;
}

// The hash of the transcript's bytes [lo, hi), at most a window: taken from buf, which holds n
// bytes from offset `from` on, when they lie there, else read from the file.
static bool range_hash(const TranscriptFile *f, const char *buf, uint64_t from, size_t n, uint64_t lo, uint64_t hi, uint64_t *out) {
    if (buf && lo >= from && hi <= from + n) {
        *out = fnv1a(buf + (lo - from), (size_t)(hi - lo));
        return true;
    }
    char w[WINDOW];
    if (!read_at(f->h, lo, w, (uint32_t)(hi - lo))) return false;
    *out = fnv1a(w, (size_t)(hi - lo));
    return true;
}

// <Dir>/<transcript file name without .jsonl>.bin. Two transcripts with one file name share it and
// only cost each other a full parse: a state belongs to a file by its identity, not by path.
static us state_path(us transcript) {
    us dir = US_EMPTY;
#if defined(HUD_C_TEST)
    dir = env_get("HUD_CACHE_DIR");  // data from the comparison scripts, like HUD_FAKE_NOW_MS
#endif
    if (!dir.n) {
        us t = os_temp_dir();
        if (!t.n) return US_EMPTY;
        dir = path_join(2, t, US("claude-hud-c"));
    }
    int32_t b = transcript.n;
    while (b > 0 && !is_sep(transcript.p[b - 1])) b--;
    us name = us_slice(transcript, b, transcript.n);
    if (us_ends_c(name, ".jsonl")) name.n -= 6;
    return name.n ? path_join(2, dir, us_cat(2, name, US(".bin"))) : US_EMPTY;
}

// ---- saving: the structures copied whole, their pointers made offsets ----

typedef struct { char *p; size_t n, cap; } Blob;

// Appends n bytes at a multiple of align; where they start.
static size_t put(Blob *b, const void *x, size_t n, size_t align) {
    size_t at = (b->n + align - 1) & ~(align - 1);
    if (at + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < at + n) cap *= 2;
        b->p = xrealloc(b->p, cap), b->cap = cap;
    }
    memset(b->p + b->n, 0, at - b->n);
    if (n) memcpy(b->p + at, x, n);  // an empty array's pointer may be null
    b->n = at + n;
    return at;
}

// What is already in the blob, at an offset: the blob moves as it grows, so never hold a pointer.
#define AT(b, off, T) ((T *)((b)->p + (off)))

// A pointer as saved: its target's offset plus one, so that null stays null.
static void *ref(size_t off) { return (void *)(uintptr_t)(off + 1); }

static void put_us(Blob *b, size_t at) {
    us s = *AT(b, at, us);
    void *p = s.n > 0 ? ref(put(b, s.p, (size_t)s.n * 2, 2)) : nullptr;
    AT(b, at, us)->p = p;
}

static void put_map(Blob *b, size_t at) {
    omap m = *AT(b, at, omap);
    size_t e = put(b, m.e, sizeof *m.e * (size_t)m.n, 16);
    for (int32_t i = 0; i < m.n; i++) {
        void *k = ref(put(b, m.e[i].k, m.e[i].kn, 1));
        AT(b, e, om_ent)[i].k = k;
    }
    size_t slot = put(b, m.slot, sizeof *m.slot * (size_t)m.nslot, 16);
    omap *saved = AT(b, at, omap);
    saved->e = m.n ? ref(e) : nullptr;
    saved->cap = m.n;
    saved->slot = m.nslot ? ref(slot) : nullptr;
}

static void put_vec(Blob *b, size_t at, size_t elem, const size_t *strs, size_t nstrs) {
    AnyVec v;
    memcpy(&v, b->p + at, sizeof v);
    size_t arr = put(b, v.p, elem * (size_t)v.n, 16);
    for (int32_t i = 0; i < v.n; i++)
        for (size_t k = 0; k < nstrs; k++) put_us(b, arr + elem * (size_t)i + strs[k]);
    v.p = v.n ? ref(arr) : nullptr;
    v.cap = v.n;
    memcpy(b->p + at, &v, sizeof v);
}

bool state_save(const TranscriptFile *f, const StateSlot *slot, uint64_t off, const char *buf, uint64_t from, size_t n, Parser *s) {
    us path = slot->path.n ? slot->path : state_path(f->path);
    if (!path.n) return false;
    Header hd = {.magic = STATE_MAGIC, .volume = f->volume, .file = f->file, .off = off};
    memcpy(hd.build, BUILD, sizeof BUILD);
    uint64_t w = off < WINDOW ? off : WINDOW;
    // A loaded state already hashed the first window, which an append leaves alone.
    if (slot->head_known && w == WINDOW) hd.head = slot->head;
    else if (!range_hash(f, buf, from, n, 0, w, &hd.head)) return false;
    if (!range_hash(f, buf, from, n, off - w, off, &hd.tail)) return false;

    Blob b = {0};
    size_t h = put(&b, &hd, sizeof hd, 16);
    if (g_unsupported) {
        void *why = ref(put(&b, g_unsupported, strlen(g_unsupported) + 1, 1));
        AT(&b, h, Header)->why = why;
    }
    size_t pa = put(&b, s, sizeof *s, 16), ta = put(&b, s->r, sizeof *s->r, 16);
    AT(&b, h, Header)->parser = ref(pa);
    AT(&b, h, Header)->transcript = ref(ta);
    AT(&b, pa, Parser)->r = nullptr;
    static const size_t maps[] = {offsetof(Parser, tools), offsetof(Parser, agents), offsetof(Parser, skills), offsetof(Parser, mcp),
                                  offsetof(Parser, mcp_err), offsetof(Parser, completions), offsetof(Parser, usage), offsetof(Parser, task_idx)};
    for (size_t i = 0; i < countof(maps); i++) put_map(&b, pa + maps[i]);
    put_vec(&b, pa + offsetof(Parser, toolv), sizeof(Tool), TOOL_STRS, countof(TOOL_STRS));
    put_vec(&b, pa + offsetof(Parser, agentv), sizeof(Agent), AGENT_STRS, countof(AGENT_STRS));
    put_vec(&b, pa + offsetof(Parser, todos), sizeof(Todo), TODO_STRS, countof(TODO_STRS));
    put_vec(&b, pa + offsetof(Parser, skillv), sizeof(us), STR_STRS, countof(STR_STRS));
    put_vec(&b, pa + offsetof(Parser, mcpv), sizeof(us), STR_STRS, countof(STR_STRS));
    put_vec(&b, pa + offsetof(Parser, errv), sizeof(us), STR_STRS, countof(STR_STRS));
    put_us(&b, ta + offsetof(Transcript, advisor));
    put_us(&b, ta + offsetof(Transcript, last_model));
    AT(&b, h, Header)->size = b.n;

    // Written aside and renamed over, so a reader finds a whole state or the one before.
    us tmp = us_cat(4, path, US("."), us_int(os_pid()), US(".tmp"));
    bool ok = write_bytes(tmp, b.p, b.n, 0, WRITE_ALWAYS) && os_rename(tmp, path);
    free(b.p);
    return ok;
}

// ---- loading: every offset checked against the file before it becomes a pointer ----

typedef struct { char *base; size_t n; } Span;

// A saved pointer back to a real one: its target must lie in the file, `bytes` long, at a
// multiple of align. Null only for nothing.
static bool fix(Span f, void *field, size_t bytes, size_t align) {
    uintptr_t v;
    memcpy(&v, field, sizeof v);
    if (!v) return bytes == 0;
    v -= 1;
    if (v > f.n || bytes > f.n - v || v % align) return false;
    char *p = f.base + v;
    memcpy(field, &p, sizeof p);
    return true;
}

static bool fix_us(Span f, us *s) { return s->n >= 0 && fix(f, &s->p, (size_t)s->n * 2, 2); }

static bool fix_str(Span f, const char **s) {  // NUL-terminated, or null
    uintptr_t v;
    memcpy(&v, s, sizeof v);
    return !v || (fix(f, s, 1, 1) && memchr(*s, 0, (size_t)(f.base + f.n - *s)));
}

// Entry indices are checked by the caller, which knows the vectors they point into.
static bool fix_map(Span f, omap *m) {
    if (m->n < 0 || m->alive < 0 || m->alive > m->n || m->head < 0 || m->head > m->n || m->nslot < 0 || (m->nslot & (m->nslot - 1)))
        return false;
    m->cap = m->n;
    if (!fix(f, &m->e, sizeof *m->e * (size_t)m->n, 16) || !fix(f, &m->slot, sizeof *m->slot * (size_t)m->nslot, 16)) return false;
    bool open = m->nslot == 0;  // a lookup ends at a free slot, so a full table would never end one
    for (int32_t i = 0; i < m->nslot; i++) {
        if (m->slot[i] < -2 || m->slot[i] >= m->n) return false;
        open |= m->slot[i] == -1;
    }
    if (!open) return false;
    for (int32_t i = 0; i < m->n; i++)
        if (!fix(f, &m->e[i].k, m->e[i].kn, 1)) return false;
    return true;
}

static bool fix_vec(Span f, void *vec, size_t elem, const size_t *strs, size_t nstrs) {
    AnyVec v;
    memcpy(&v, vec, sizeof v);
    if (v.n < 0) return false;
    v.cap = v.n;
    if (!fix(f, &v.p, elem * (size_t)v.n, 16)) return false;
    for (int32_t i = 0; i < v.n; i++)
        for (size_t k = 0; k < nstrs; k++)
            if (!fix_us(f, (us *)((char *)v.p + elem * (size_t)i + strs[k]))) return false;
    memcpy(vec, &v, sizeof v);
    return true;
}

static bool map_in(const omap *m, int32_t n) {
    for (int32_t i = 0; i < m->n; i++)
        if (m->e[i].alive && (m->e[i].i < 0 || m->e[i].i >= n)) return false;
    return true;
}

static bool fix_parser(Span f, Parser *p) {
    omap *maps[] = {&p->tools, &p->agents, &p->skills, &p->mcp, &p->mcp_err, &p->completions, &p->usage, &p->task_idx};
    for (size_t i = 0; i < countof(maps); i++)
        if (!fix_map(f, maps[i])) return false;
    return fix_vec(f, &p->toolv, sizeof(Tool), TOOL_STRS, countof(TOOL_STRS)) &&
           fix_vec(f, &p->agentv, sizeof(Agent), AGENT_STRS, countof(AGENT_STRS)) &&
           fix_vec(f, &p->todos, sizeof(Todo), TODO_STRS, countof(TODO_STRS)) &&
           fix_vec(f, &p->skillv, sizeof(us), STR_STRS, countof(STR_STRS)) && fix_vec(f, &p->mcpv, sizeof(us), STR_STRS, countof(STR_STRS)) &&
           fix_vec(f, &p->errv, sizeof(us), STR_STRS, countof(STR_STRS)) && map_in(&p->tools, p->toolv.n) && map_in(&p->agents, p->agentv.n) &&
           map_in(&p->skills, p->skillv.n) && map_in(&p->mcp, p->mcpv.n) && map_in(&p->mcp_err, p->errv.n) && map_in(&p->task_idx, p->todos.n);
}

bool state_load(const TranscriptFile *f, StateSlot *slot, Parser *s, uint64_t *off) {
    slot->path = state_path(f->path);
    if (!slot->path.n) return false;
    void *h = open_read(slot->path);
    if (!h) return false;
    uint64_t sz = 0;
    char *buf = nullptr;
    // The file is read whole, once: it never grows while it is being read.
    bool ok = os_size(h, &sz) && sz >= sizeof(Header) && sz < (1 << 30) && read_at(h, 0, buf = xmalloc((size_t)sz), (uint32_t)sz);
    os_close(h);
    Span sp = {buf, (size_t)sz};
    Header *hd = (Header *)buf;
    uint64_t w = 0, head = 0, tail = 0;
    if (ok) {
        w = hd->off < WINDOW ? hd->off : WINDOW;
        ok = hd->magic == STATE_MAGIC && memcmp(hd->build, BUILD, sizeof BUILD) == 0 && hd->size == sp.n && hd->volume == f->volume && hd->file == f->file && hd->off <= f->size &&
             range_hash(f, nullptr, 0, 0, 0, w, &head) && head == hd->head && range_hash(f, nullptr, 0, 0, hd->off - w, hd->off, &tail) &&
             tail == hd->tail && fix_str(sp, &hd->why) && fix(sp, &hd->parser, sizeof(Parser), 16) &&
             fix(sp, &hd->transcript, sizeof(Transcript), 16) && fix_parser(sp, hd->parser) && fix_us(sp, &hd->transcript->advisor) &&
             fix_us(sp, &hd->transcript->last_model);
    }
    if (!ok) {
        free(buf);
        return false;
    }
    // From here the buffer is the state, and like the arena it lives until the process ends.
    Transcript *r = s->r;
    *s = *hd->parser;
    s->r = r, s->used_now = false;
    *r = *hd->transcript;
    r->tools = nullptr, r->agents = nullptr, r->todos = nullptr, r->skills = r->mcp_servers = r->mcp_errors = nullptr;
    r->ntools = r->nagents = r->ntodos = r->nskills = r->nmcp = r->nmcp_err = 0;
    if (hd->why) UNSUPPORTED(hd->why);
    slot->head = head, slot->head_known = w == WINDOW;
    *off = hd->off;
    return true;
}
#endif
