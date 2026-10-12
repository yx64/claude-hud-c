// Differential test: the test build against a reference (common.mjs), byte for byte, including the files
// both write. Real session transcripts crossed with stdin, config, auth, width, clock and memory
// variants, in sandbox config dirs, one of them inside a git repository. Usage:
//   node diff.mjs <HudTestExe> <Reference> <WorkDir> [maxCases]
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { checkTestBuild, contents, reference, restore, run, runReference, snapshot } from './common.mjs';

const [hudExe, refArg, workDir, maxArg] = process.argv.slice(2);
const maxCases = Number(maxArg ?? Infinity);
const ref = reference(refArg);
const realClaude = path.join(os.homedir(), '.claude');
fs.mkdirSync(workDir, { recursive: true });

function copyDir(src, dst) {
  fs.mkdirSync(dst, { recursive: true });
  for (const e of fs.readdirSync(src, { withFileTypes: true })) {
    const s = path.join(src, e.name), d = path.join(dst, e.name);
    if (e.isDirectory()) copyDir(s, d);
    else if (e.isFile()) fs.copyFileSync(s, d);
  }
}

const realConfig = JSON.parse(fs.readFileSync(path.join(realClaude, 'plugins', 'claude-hud', 'config.json'), 'utf8'));

const git = (cwd, ...args) => execFileSync('git', args, { cwd, stdio: 'ignore' });

// <w>/proj/.claude is CLAUDE_CONFIG_DIR and <w>/proj a cwd that overlaps it; <w>/other has its own
// project config; <w>/repo is a git repository with staged, modified, renamed and untracked files.
function makeSandbox(k) {
  const root = path.join(workDir, `w${k}`);
  const proj = path.join(root, 'proj');
  const cfg = path.join(proj, '.claude');
  if (!fs.existsSync(cfg)) {
    fs.mkdirSync(path.join(cfg, 'plugins', 'claude-hud'), { recursive: true });
    for (const f of ['settings.json', 'CLAUDE.md']) {
      if (fs.existsSync(path.join(realClaude, f))) fs.copyFileSync(path.join(realClaude, f), path.join(cfg, f));
    }
    if (fs.existsSync(path.join(realClaude, 'rules'))) copyDir(fs.realpathSync(path.join(realClaude, 'rules')), path.join(cfg, 'rules'));
    fs.writeFileSync(path.join(proj, 'CLAUDE.md'), '# test\n');
    fs.mkdirSync(path.join(root, 'other', '.claude', 'rules'), { recursive: true });
    fs.writeFileSync(path.join(root, 'other', '.claude', 'rules', 'r.md'), '');
    fs.writeFileSync(path.join(root, 'other', '.claude', 'settings.local.json'), JSON.stringify({ hooks: { Stop: [] }, disabledMcpjsonServers: ['b'] }));
    fs.writeFileSync(path.join(root, 'other', '.mcp.json'), JSON.stringify({ mcpServers: { a: {}, b: {} } }));
    const repo = path.join(root, 'repo');
    fs.mkdirSync(path.join(repo, 'sub'), { recursive: true });
    git(repo, 'init', '-q', '-b', 'feat/中文-x');
    for (const f of ['a.txt', 'b.txt', 'c.txt', 'sub/d.txt']) fs.writeFileSync(path.join(repo, f), 'one\ntwo\nthree\n');
    git(repo, 'add', '.');
    git(repo, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'init');
    fs.writeFileSync(path.join(repo, 'a.txt'), 'one\n2\nthree\nfour\n');
    git(repo, 'mv', 'b.txt', 'b2.txt');
    fs.writeFileSync(path.join(repo, 'new.txt'), 'x\n');
    git(repo, 'add', 'new.txt');
    fs.writeFileSync(path.join(repo, 'untracked file.txt'), 'u\n');
  }
  return { root, proj, cfg, repo: path.join(root, 'repo'), moved: path.join(root, 'moved'), cache: path.join(root, 'hud-cache') };
}

function listTranscripts() {
  const out = [];
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (e.name.endsWith('.jsonl') && Date.now() - fs.statSync(p).mtimeMs > 15 * 60 * 1000) out.push(p);
    }
  };
  walk(path.join(realClaude, 'projects'));
  return out.sort();
}

function lastTimestamp(file) {
  const fd = fs.openSync(file, 'r');
  const size = fs.fstatSync(fd).size;
  const len = Math.min(size, 256 * 1024);
  const buf = Buffer.alloc(len);
  fs.readSync(fd, buf, 0, len, size - len);
  fs.closeSync(fd);
  const m = [...buf.toString('utf8').matchAll(/"timestamp":"([^"]+)"/g)];
  const t = m.length ? Date.parse(m[m.length - 1][1]) : NaN;
  return Number.isNaN(t) ? Date.parse('2026-09-01T00:00:00Z') : t;
}

const sec = (ms) => Math.floor(ms / 1000);
const STDIN_VARIANTS = [
  (tp, sid, cwd, now) => ({
    session_id: sid, transcript_path: tp, cwd, version: '2.1.295', session_name: 'diff 测试',
    model: { id: 'claude-opus-5-5[1m]', display_name: 'Opus 5.5 (1M context)' },
    workspace: { current_dir: cwd, project_dir: cwd, repo: { host: 'github.com', owner: 'o-w.n', name: 'r_e' }, git_worktree: 'wt' },
    output_style: { name: 'Explanatory' },
    cost: { total_cost_usd: 1.2345, total_duration_ms: 3_725_000, total_api_duration_ms: 90_000 },
    context_window: { context_window_size: 1000000, used_percentage: 12.5, remaining_percentage: 87.5,
      current_usage: { input_tokens: 10, output_tokens: 100, cache_creation_input_tokens: 500, cache_read_input_tokens: 120000 } },
    rate_limits: { five_hour: { used_percentage: 42.4, resets_at: sec(now) + 5000 }, seven_day: { used_percentage: 81, resets_at: sec(now) + 300000 } },
    prompt_cache: { caching_observed: true, warm: true, expires_at: sec(now) + 200, ttl: '5m', hit_ratio: 0.93712 },
    effort: { level: 'xhigh' },
  }),
  (tp, sid, cwd, now) => ({
    session_id: sid, transcript_path: tp, cwd,
    model: { id: 'claude-sonnet-4-5-20250929', display_name: 'Sonnet 4.5 (1M context)' },
    context_window: { context_window_size: 200000, current_usage: { input_tokens: 90000, cache_read_input_tokens: 80000 } },
    prompt_cache: { caching_observed: true, warm: true, expires_at: sec(now) + 3000, ttl: '1h', hit_ratio: 1.5 },
    effort: { level: 'High' },
  }),
  (tp, sid, cwd, now) => ({
    session_id: sid, transcript_path: tp, cwd,
    model: { id: 'claude-haiku-4-5' },
    cost: { total_cost_usd: 0.05, total_duration_ms: 20_000 },
    context_window: { context_window_size: 200000, used_percentage: 0, current_usage: { input_tokens: 0 } },
    rate_limits: { five_hour: { used_percentage: 100, resets_at: sec(now) + 90000 } },
    prompt_cache: { caching_observed: true, warm: false, expires_at: sec(now) + 100 },
    effort: { level: 'ultracode' },
  }),
  (tp, sid, cwd, now) => ({
    session_id: sid, transcript_path: tp, cwd, version: '\x1b[31m9.9\x1b[0m',
    model: { id: 'us.anthropic.claude-opus-4-1-20250805-v1:0', display_name: '' },
    cost: { total_cost_usd: 3.5 },
    workspace: { added_dirs: [path.join(cwd, 'a b'), 'D:\\测试\\很长很长很长很长很长很长很长很长的目录名', 'C:\\x', 'C:\\y', 'C:\\z', 'C:\\w'] },
    context_window: { context_window_size: 200000, used_percentage: 91, current_usage: { input_tokens: 150000, cache_creation_input_tokens: 2000, cache_read_input_tokens: 30000 } },
    rate_limits: { five_hour: { used_percentage: 10 }, seven_day: { used_percentage: 95, resets_at: sec(now) + 30 } },
  }),
];

const CONFIG_VARIANTS = [
  realConfig,
  { ...realConfig, display: { ...realConfig.display, timeFormat: 'relative', hourCycle: 'auto', contextValue: 'tokens', showSessionName: true,
    showLastResponseAt: true, showSessionStartDate: true, showOutputStyle: true, showAuthUser: true, toolsMaxVisible: 0, toolNameMaxLength: 6,
    modelFormat: 'short', showProvider: true, customLine: 'hello │ world', addedDirsLayout: 'line', showWeeklyCost: true, showCacheHitRate: true,
    usagePace: true, skillsMaxVisible: 1 }, showSeparators: true, pathLevels: 2 },
  { ...realConfig, elementOrder: ['tools', 'context', 'usage', 'project', 'todos', 'agents', 'promptCache', 'cacheHitRate', 'environment', 'skills', 'mcp', 'addedDirs', 'sessionTime'],
    projectLineOrder: ['cost', 'model'], colors: { label: 'cyan', context: '#12ab9f', usage: 33, barFilled: '■', barEmpty: '·', custom: 'red', git: 'green', gitBranch: 200 },
    gitStatus: { enabled: true, showAheadBehind: true, showFileStats: true, showWorktree: true, branchOverflow: 'wrap' },
    display: { ...realConfig.display, timeFormat: 'absolute', hourCycle: 'h12', showClockSeconds: true, usageCompact: true, effortFormat: 'symbol',
      mergeGroups: [['context', 'usage', 'promptCache'], ['tools', 'todos']], rightAlign: ['usage', 'todos'], contextValue: 'remaining', usageValue: 'remaining',
      showMemoryUsage: true, showCacheHitRate: true } },
  { ...realConfig, pathLevels: 'full', maxWidth: 70, forceMaxWidth: true, lineLayout: 'compact', gitStatus: { enabled: true, showFileStats: true },
    display: { ...realConfig.display, timeFormat: 'elapsedAndAbsolute', hourCycle: 'h24', usageBarEnabled: false, showSpeed: true,
      showResetLabel: false, effortFormat: 'text', modelSource: 'auto', showAdvisor: true, advisorOverride: '', contextWarningThreshold: 5, contextCriticalThreshold: 10,
      showTokenBreakdown: true, sevenDayThreshold: 0, autoCompactWindow: 400000, showWeeklyCost: true } },
];

const AUTH_VARIANTS = [
  null,
  { oauthAccount: { organizationType: 'claude_max', organizationRateLimitTier: 'default_claude_max_20x', emailAddress: 'someone.long.name@example.com' }, mcpServers: { a: {}, b: {} }, disabledMcpServers: ['b'] },
];

const COLUMNS = [undefined, '200', '80', '40'];
const NOW_OFFSETS = [10e3, 240e3, 2 * 3600e3, -60e3];
const MEMORY = ['17179869184:5368709120', '68719476736:1048576'];

// Variants are picked by a fixed-seed generator, so no two dimensions move in lockstep and a
// rerun covers the same cases.
let seed = 7;
const pick = (n) => Math.floor((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648 * n);
const transcripts = listTranscripts();
const cases = [];
for (const tp of transcripts) {
  for (let r = 0; r < 8; r++) {
    cases.push({ tp, sv: pick(STDIN_VARIANTS.length), cv: pick(CONFIG_VARIANTS.length), av: pick(AUTH_VARIANTS.length),
      col: COLUMNS[pick(COLUMNS.length)], off: NOW_OFFSETS[pick(NOW_OFFSETS.length)], mem: MEMORY[pick(MEMORY.length)], repo: pick(2) === 1 });
  }
}
cases.length = Math.min(cases.length, maxCases);

const stats = { cases: 0, equal: 0, diff: 0, unsupported: {}, cTime: 0, nTime: 0 };
const diffs = [];
const WORKERS = 6;
// A worker takes all the cases of one transcript in a row, with its own parse-state cache: the first
// parses from the start, the rest resume from the state it saved.
const groups = [];
for (const c of cases) {
  if (groups.at(-1)?.[0].tp !== c.tp) groups.push([]);
  groups.at(-1).push(c);
}
let next = 0;

async function worker(k) {
  const sb = makeSandbox(k);
  while (next < groups.length) {
    for (const c of groups[next++]) {
      const now = Math.round(lastTimestamp(c.tp) + c.off);
      const sid = path.basename(c.tp, '.jsonl');
      const cwd = c.repo ? sb.repo : c.sv === 1 ? path.join(sb.root, 'other') : sb.proj;
      const input = JSON.stringify(STDIN_VARIANTS[c.sv](c.tp, sid, cwd, now));
      fs.writeFileSync(path.join(sb.cfg, 'plugins', 'claude-hud', 'config.json'), JSON.stringify(CONFIG_VARIANTS[c.cv]));
      fs.writeFileSync(path.join(sb.cfg, '.claude.json'), JSON.stringify(AUTH_VARIANTS[c.av] ?? {}));
      const env = { ...process.env, CLAUDE_CONFIG_DIR: sb.cfg, HUD_CACHE_DIR: sb.cache, HUD_FAKE_NOW_MS: String(now), HUD_FAKE_MEM: c.mem };
      delete env.COLUMNS;
      if (c.col) env.COLUMNS = c.col;
      const s0 = snapshot(sb.cfg);
      let t0 = performance.now();
      const rc = await run(hudExe, [], env, input);
      const tc = performance.now() - t0;
      const sc = snapshot(sb.cfg);
      restore(s0, sc, sb.moved);
      t0 = performance.now();
      const rn = await runReference(ref, env, input);
      const tn = performance.now() - t0;
      const sn = snapshot(sb.cfg);
      restore(s0, sn, sb.moved);
      checkTestBuild(hudExe, ref, rc, rn);
      stats.cases++;
      if (rc.code === 3) {
        const why = rc.err.trim();
        stats.unsupported[why] = (stats.unsupported[why] ?? 0) + 1;
        continue;
      }
      stats.cTime += tc, stats.nTime += tn;
      const filesSame = contents(sc) === contents(sn);
      const ok = rc.code === 0 && rc.out.equals(rn.out) && filesSame;
      if (ok) stats.equal++;
      else {
        stats.diff++;
        if (diffs.length < 20) diffs.push({ case: c, input, now, c: rc.out.toString(), n: rn.out.toString(), cerr: rc.err, nerr: rn.err, code: rc.code,
          filesSame, cFiles: contents(sc), nFiles: contents(sn) });
      }
    }
  }
}

await Promise.all(Array.from({ length: WORKERS }, (_, k) => worker(k)));
const report = path.join(workDir, 'diff-report.json');
fs.writeFileSync(report, JSON.stringify({ reference: ref, stats, diffs }, null, 2));
console.log(`reference ${ref.kind} ${ref.path}`);
console.log(`transcripts ${transcripts.length}, cases ${stats.cases}, equal ${stats.equal}, diff ${stats.diff}`);
console.log('unsupported:', JSON.stringify(stats.unsupported));
const compared = stats.equal + stats.diff;
if (compared) console.log(`mean ms (parallel, ${WORKERS} workers): C ${(stats.cTime / compared).toFixed(1)}, reference ${(stats.nTime / compared).toFixed(1)}`);
console.log(`report: ${report}`);
process.exitCode = stats.diff > 0 || compared === 0 ? 1 : 0;
