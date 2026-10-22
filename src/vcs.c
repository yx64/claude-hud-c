// Port of claude-hud dist/git.js, git-runner.js and jj.js: one child process at a time (os_run),
// the whole tree ended when it overruns its time.
#include <math.h>
#include <string.h>

#include "hud.h"

// ---- git-runner.js ----

static const char *const GIT_ENV[] = {"GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0", "GCM_INTERACTIVE=Never", nullptr};

static int run_git(us git, us cwd, const char *const *args, uint32_t timeout, us *out) {
    return os_run(git, args, cwd, GIT_ENV, timeout + OS_GIT_EXTRA_MS, 1024 * 1024, out);
}

// ---- git.js ----

#define QUIET "-c", "core.quotePath=false", "--no-optional-locks"

static bool github_name(jv v) {  // /^[A-Za-z0-9_.-]+$/ on String(v ?? '')
    if (jis_nullish(v)) return false;
    if (!jis_str(v)) {
        UNSUPPORTED("non-string repository owner or name");
        return false;
    }
    us s = jstr(v);
    if (!s.n) return false;
    for (int32_t i = 0; i < s.n; i++) {
        uint16_t c = s.p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    // new URL() would fold these path segments away.
    if (us_eq_c(s, ".") || us_eq_c(s, "..")) UNSUPPORTED("repository owner or name that is a dot segment");
    return true;
}

static us encode_uri_component(us s) {
    sb b = {0};
    static const char hex[] = "0123456789ABCDEF";
    size_t n;
    char *u8 = us_to_utf8(s, &n);
    for (int32_t i = 0; i < s.n; i++)
        if (s.p[i] >= 0xD800 && s.p[i] <= 0xDFFF) {
            bool pair = s.p[i] <= 0xDBFF && i + 1 < s.n && s.p[i + 1] >= 0xDC00 && s.p[i + 1] <= 0xDFFF;
            if (!pair) UNSUPPORTED("lone surrogate in a branch name");
            i += pair;
        }
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)u8[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c && strchr("-_.!~*'()", c))) sb_ch(&b, c);
        else sb_ch(&b, '%'), sb_ch(&b, (uint16_t)hex[c >> 4]), sb_ch(&b, (uint16_t)hex[c & 15]);
    }
    return sb_done(&b);
}

static us github_ref_url(jv repo, us ref) {
    if (!jstr_eq(jget(repo, "host"), "github.com") || !github_name(jget(repo, "owner")) || !github_name(jget(repo, "name"))) return US_EMPTY;
    us base = us_cat(US("https://github.com/"), jstr(jget(repo, "owner")), US("/"), jstr(jget(repo, "name")));
    if (us_starts_c(ref, "detached:") && ref.n > 9) {
        bool hex = true;
        for (int32_t i = 9; i < ref.n; i++) hex &= (ref.p[i] >= '0' && ref.p[i] <= '9') || (ref.p[i] >= 'a' && ref.p[i] <= 'f');
        if (hex) return us_cat(base, US("/commit/"), us_slice(ref, 9, ref.n));
    }
    sb b = {0};
    sb_us(&b, base);
    sb_us(&b, US("/tree/"));
    for (int32_t i = 0; i <= ref.n;) {
        int32_t j = us_find_ch(ref, '/', i);
        if (j < 0) j = ref.n;
        us seg = us_slice(ref, i, j);
        if (us_eq_c(seg, ".") || us_eq_c(seg, "..")) UNSUPPORTED("branch with a dot segment");
        if (i) sb_ch(&b, '/');
        sb_us(&b, encode_uri_component(seg));
        i = j + 1;
    }
    return sb_done(&b);
}

static us after_fields(us record, int fields) {  // pathAfterFields, indexOf wrap-around included
    int32_t index = 0;
    for (int f = 0; f < fields; f++) index = us_find_ch(record, ' ', index) + 1;
    return us_slice(record, index, record.n);
}

static int classify(uint16_t x, uint16_t y) {  // renames, copies and UU conflicts count as modified
    if (x == 'A' || (x == 'U' && y == 'A')) return TF_ADDED;
    if (x == 'D' || y == 'D') return TF_DELETED;
    if (x == 'M' || y == 'M' || x == 'R' || x == 'C' || x == 'U') return TF_MODIFIED;
    return -1;
}

static int split_nul(us s, us **out) {
    int n = 1;
    for (int32_t i = 0; i < s.n; i++) n += s.p[i] == 0;
    us *r = arena_alloc(sizeof(us) * (size_t)n);
    int k = 0;
    int32_t start = 0;
    for (int32_t i = 0; i <= s.n; i++)
        if (i == s.n || s.p[i] == 0) r[k++] = us_slice(s, start, i), start = i + 1;
    *out = r;
    return k;
}

static bool parse_digits(us s, double *v) {  // Number.parseInt(s, 10) for git's plain counts
    int32_t i = 0;
    while (i < s.n && js_is_space(s.p[i])) i++;
    bool neg = false;
    if (i < s.n && (s.p[i] == '+' || s.p[i] == '-')) neg = s.p[i++] == '-';
    int32_t start = i;
    double x = 0;
    while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') x = x * 10 + (s.p[i++] - '0');
    if (i == start) return false;
    *v = neg ? -x : x;
    return true;
}

// QUIET, then what keeps git from running commands a repository's own config names: core.fsmonitor on
// every status, and filter drivers whenever git hashes a changed file. Both are turned off for drivers
// that do not come from the user's global or system config, so git-lfs keeps working; a driver name git
// cannot take back in -c makes git refuse to run.
typedef struct { const char **p; int n; } Args;

static Args safe_args(us git, us cwd) {
    us out, *f;
    int nf = 0;
    // exits 1 when no filter is configured
    if (run_git(git, cwd, (const char *const[]){"config", "--show-scope", "-z", "--get-regexp", "^filter\\.", nullptr}, 1000, &out) == RUN_OK)
        nf = split_nul(out, &f);
    us *names = arena_alloc(sizeof(us) * (size_t)(nf / 2 + 1));
    int nn = 0;
    for (int i = 0; i + 1 < nf; i += 2) {
        if (us_eq_c(f[i], "global") || us_eq_c(f[i], "system")) continue;
        us key = f[i + 1];
        int32_t nl = us_find_ch(key, '\n', 0);
        if (nl >= 0) key.n = nl;
        int32_t dot = key.n - 1;
        while (dot >= 0 && key.p[dot] != '.') dot--;
        us name = dot > 7 ? us_slice(key, 7, dot) : US_EMPTY;  // filter.<name>.<key>
        bool seen = !name.n;
        for (int k = 0; k < nn && !seen; k++) seen = us_eq(names[k], name);
        if (!seen) names[nn++] = name;
    }
    static const char *const head[] = {QUIET, "-c", "core.fsmonitor=false"};
    static const char *const keys[] = {"clean=", "process=", "required=false"};
    Args a = {arena_alloc(sizeof(char *) * (countof(head) + 2 * countof(keys) * (size_t)nn)), 0};
    for (size_t j = 0; j < countof(head); j++) a.p[a.n++] = head[j];
    for (int k = 0; k < nn; k++)
        for (size_t j = 0; j < countof(keys); j++) {
            size_t len;
            a.p[a.n++] = "-c";
            a.p[a.n++] = us_to_utf8(us_cat(US("filter."), names[k], US("."), us_c(keys[j])), &len);
        }
    return a;
}

// safe, then tail (null-terminated)
static const char *const *with(Args safe, const char *const *tail) {
    int nt = 0;
    while (tail[nt]) nt++;
    const char **a = arena_alloc(sizeof(char *) * (size_t)(safe.n + nt + 1));
    memcpy(a, safe.p, sizeof(char *) * (size_t)safe.n);
    memcpy(a + safe.n, tail, sizeof(char *) * (size_t)(nt + 1));
    return a;
}

VcsStatus *git_status(us cwd, jv repo, bool line_diffs) {
    us git, out;
    if (!os_find_git(&git)) return nullptr;
    Args safe = safe_args(git, cwd);
    // Uncommitted changes inside a submodule do not make the repository dirty, and git inside one reads its config.
    int r = run_git(git, cwd, with(safe, (const char *const[]){"status", "--porcelain=v2", "--branch", "-z", "--ignore-submodules=dirty", nullptr}), 1000, &out);
    VcsStatus *st = arena_alloc(sizeof *st);
    *st = (VcsStatus){0};
    if (r == RUN_TIMEOUT) {
        // A status that timed out in a large repo still leaves the branch worth showing.
        if (run_git(git, cwd, (const char *const[]){"rev-parse", "--abbrev-ref", "HEAD", nullptr}, 1000, &out) != RUN_OK) return nullptr;
        us b = us_trim(out);
        if (!b.n || us_eq_c(b, "HEAD")) return nullptr;
        st->branch = b, st->branch_url = github_ref_url(repo, b);
        return st;
    }
    if (r != RUN_OK) return nullptr;
    us *rec;
    int nrec = split_nul(out, &rec);
    us oid = US_EMPTY, head = US_EMPTY;
    bool has_oid = false, has_head = false;
    st->files = arena_alloc(sizeof(GitFile) * (size_t)(nrec + 1));
    for (int i = 0; i < nrec; i++) {
        us x = rec[i];
        if (us_starts_c(x, "# branch.oid ")) oid = us_slice(x, 13, x.n), has_oid = true;
        else if (us_starts_c(x, "# branch.head ")) {
            head = us_slice(x, 14, x.n);
            has_head = !us_eq_c(head, "(detached)");
        } else if (us_starts_c(x, "# branch.ab ")) {
            us ab = us_slice(x, 12, x.n);  // /^\+(\d+) -(\d+)$/
            int32_t sp = us_find_ch(ab, ' ', 0);
            us a = sp > 1 && ab.p[0] == '+' ? us_slice(ab, 1, sp) : US_EMPTY;
            us b = sp > 0 && sp + 2 < ab.n && ab.p[sp + 1] == '-' ? us_slice(ab, sp + 2, ab.n) : US_EMPTY;
            bool ok = a.n && b.n;
            for (int32_t k = 0; ok && k < a.n; k++) ok = a.p[k] >= '0' && a.p[k] <= '9';
            for (int32_t k = 0; ok && k < b.n; k++) ok = b.p[k] >= '0' && b.p[k] <= '9';
            if (ok) parse_digits(a, &st->ahead), parse_digits(b, &st->behind);
        } else if (us_starts_c(x, "? ")) {
            st->dirty = true;
            st->untracked++;
        } else if (x.n >= 2 && x.p[1] == ' ' && (x.p[0] == '1' || x.p[0] == '2' || x.p[0] == 'u')) {
            st->dirty = true;
            us full = after_fields(x, x.p[0] == '1' ? 8 : x.p[0] == '2' ? 9 : 10);
            if (x.p[0] == '2') i++;  // a rename or copy is followed by its original path
            int type = classify(x.n > 2 ? x.p[2] : 0, x.n > 3 ? x.p[3] : 0);
            if (type < 0) continue;
            if (type == TF_ADDED) st->added++;
            else if (type == TF_DELETED) st->deleted++;
            else st->modified++;
            int32_t slash = full.n - 1;
            while (slash >= 0 && full.p[slash] != '/') slash--;
            st->files[st->nfiles++] = (GitFile){us_slice(full, slash + 1, full.n), full, type};
        }
    }
    us branch = head;
    if (!has_head) {
        us tag;
        if (run_git(git, cwd, (const char *const[]){"describe", "--tags", "--exact-match", "HEAD", nullptr}, 1000, &tag) == RUN_OK && us_trim(tag).n) branch = us_trim(tag);
        else {
            bool hex = has_oid && oid.n >= 7;
            for (int32_t k = 0; hex && k < oid.n; k++) hex = (oid.p[k] >= '0' && oid.p[k] <= '9') || (oid.p[k] >= 'a' && oid.p[k] <= 'f');
            if (!hex) return nullptr;
            branch = us_cat(US("detached:"), us_slice(oid, 0, 7));
        }
    }
    if (!branch.n) return nullptr;
    st->branch = branch;
    st->branch_url = github_ref_url(repo, branch);
    if (!st->dirty) return st;
    st->has_file_stats = true;
    // diff-index, not diff: porcelain diff refreshes stale stat data into .git/index whatever
    // --no-optional-locks says, and a timeout mid-write leaves .git/index.lock behind. -M keeps the
    // rename detection diff turns on by default.
    if (line_diffs && run_git(git, cwd, with(safe, (const char *const[]){"diff-index", "-M", "--numstat", "-z", "--ignore-submodules=dirty", "HEAD", nullptr}), 2000, &out) == RUN_OK) {
        // "added\tdeleted\tpath", or for a rename an empty path followed by the old and new paths
        nrec = split_nul(out, &rec);
        typedef struct { us path; double a, d; } Diff;
        Diff *diffs = arena_alloc(sizeof(Diff) * (size_t)(nrec + 1));
        int nd = 0;
        for (int i = 0; i < nrec; i++) {
            us x = rec[i];
            int32_t t1 = us_find_ch(x, '\t', 0), t2 = t1 < 0 ? -1 : us_find_ch(x, '\t', t1 + 1);
            if (t1 < 0 || t2 < 0) continue;
            us p = us_slice(x, t2 + 1, x.n);
            if (!p.n) {
                p = i + 2 < nrec ? rec[i + 2] : US_EMPTY;
                i += 2;
            }
            double a, d;
            if (!parse_digits(us_slice(x, 0, t1), &a) || !parse_digits(us_slice(x, t1 + 1, t2), &d)) continue;  // binary file
            int at = -1;
            for (int k = 0; k < nd && at < 0; k++)
                if (us_eq(diffs[k].path, p)) at = k;
            if (at < 0) diffs[at = nd++].path = p;
            diffs[at].a = a, diffs[at].d = d;
        }
        st->has_line_diff = true;
        for (int k = 0; k < nd; k++) st->line_added += diffs[k].a, st->line_deleted += diffs[k].d;
        for (int f = 0; f < st->nfiles; f++)
            for (int k = 0; k < nd; k++)
                if (us_eq(diffs[k].path, st->files[f].full)) st->files[f].has_diff = true, st->files[f].added = diffs[k].a, st->files[f].deleted = diffs[k].d;
    }
    return st;
}

// ---- jj.js ----

static bool real_directory(us cwd, us *out) {
    us r;
    if (!real_path(path_resolve(cwd), &r) || os_lstat_kind(r) != 1) return false;
    *out = r;
    return true;
}

// Walks up for a `.jj` directory, as jj does; a nearer .git ends the walk. lstat: a link is never
// a directory here.
bool is_jj_repo(us cwd) {
    us dir;
    if (!real_directory(cwd, &dir)) return false;
    for (int depth = 0; depth < 64; depth++) {
        if (os_lstat_kind(path_join(dir, US(".jj"))) == 1) return true;
        if (os_lstat_kind(path_join(dir, US(".git")))) return false;
        us parent = dir_of(dir);
        if (us_eq(parent, dir)) break;
        dir = parent;
    }
    return false;
}

static us jj_label(us v) {  // sanitized, trimmed, at most 64 code points
    us s = us_trim(sanitize_display(v));
    int32_t i = 0;
    for (int k = 0; k < 64 && i < s.n; k++) {
        int32_t l;
        us_cp_at(s, i, &l);
        i += l;
    }
    return us_slice(s, 0, i);
}

VcsStatus *jj_status(us cwd) {
    us dir, jj, out;
    if (!real_directory(cwd, &dir) || !os_find_jj(dir, &jj)) return nullptr;
    static const char *const args[] = {
        "--ignore-working-copy", "--at-operation=@", "--no-pager", "log", "-r", "@", "--no-graph", "--color", "never", "-T",
        "change_id.shortest(8) ++ \"\\x1f\" ++ self.local_bookmarks().map(|bookmark| bookmark.name()).join(\"\\x1e\") ++ \"\\x1f\" ++ "
        "if(self.empty(), \"0\", \"1\") ++ \"\\x1f\" ++ if(self.conflict(), \"1\", \"0\")",
        nullptr};
    if (os_run(jj, args, dir, (const char *const[]){nullptr}, 2000, 16 * 1024, &out) != RUN_OK) return nullptr;
    if (us_ends_c(out, "\r\n")) out.n -= 2;
    else if (us_ends_c(out, "\n")) out.n -= 1;
    us f[5];
    int nf = 0;
    for (int32_t i = 0, start = 0; i <= out.n; i++) {
        if (i < out.n && (out.p[i] == '\r' || out.p[i] == '\n')) return nullptr;
        if (i == out.n || out.p[i] == 0x1F) {
            if (nf == 4) return nullptr;
            f[nf++] = us_slice(out, start, i), start = i + 1;
        }
    }
    if (nf != 4) return nullptr;
    if (!(us_eq_c(f[2], "0") || us_eq_c(f[2], "1")) || !(us_eq_c(f[3], "0") || us_eq_c(f[3], "1"))) return nullptr;
    if (!jj_label(f[0]).n) return nullptr;
    us first = f[0];
    if (f[1].n) {
        int32_t end = us_find_ch(f[1], 0x1E, 0);
        for (int32_t i = 0, start = 0; i <= f[1].n; i++)
            if (i == f[1].n || f[1].p[i] == 0x1E) {
                if (i == start) return nullptr;  // an empty bookmark name
                start = i + 1;
            }
        first = end < 0 ? f[1] : us_slice(f[1], 0, end);
    }
    us branch = jj_label(first);
    if (!branch.n) return nullptr;
    VcsStatus *st = arena_alloc(sizeof *st);
    *st = (VcsStatus){.jj = true, .branch = branch, .dirty = us_eq_c(f[2], "1"), .conflict = us_eq_c(f[3], "1")};
    return st;
}
