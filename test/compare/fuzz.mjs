// Fuzz differential test: synthetic transcripts with hostile content, random configs, stdin and
// widths, the test build against a reference (common.mjs) byte for byte, written files included. A
// differing case is kept in the work directory. Usage:
//   node fuzz.mjs <HudTestExe> <Reference> <WorkDir> <Cases> [Seed] [FakeJjDir]
// FakeJjDir holds the jj stand-in the build makes (fake-jj in the build directory); without it the jj cases
// only see jj missing.
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { checkTestBuild, contents, matches, reference, restore, run, runReference, snapshot } from './common.mjs';

const [hudExe, refArg, workDir, casesArg, seedArg, fakeJj] = process.argv.slice(2);
const total = Number(casesArg);
let seed = Number(seedArg ?? 1);
const rnd = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);
const pick = (a) => a[Math.floor(rnd() * a.length)];
const chance = (p) => rnd() < p;
const int = (a, b) => a + Math.floor(rnd() * (b - a + 1));
const ref = reference(refArg);
fs.mkdirSync(workDir, { recursive: true });

const TEXT = [
  'hello', 'Read', 'a', '', '   ', 'x'.repeat(70), '中文内容测试', '很长的中文待办事项'.repeat(8), 'emoji 😀 ok', '👨‍👩‍👧‍👦 family',
  '🇨🇳🇺🇸 flags', 'e\u0301 combining', 'क्षत्रिय', 'ภาษาไทย', '\x1b[31mred\x1b[0m', '\x1b]8;;http://x\x07link\x1b]8;;\x07',
  'bidi \u202Eevil\u202C', 'ctl\u0007bell\u0000nul', 'lone \ud800 surrogate', 'line\u2028sep', 'para\u2029sep', 'tab\there',
  'a | b │ c', '[bracket', 'trailing ] bracket', '⏱️ ⚠ ✓ ◐', 'ｆｕｌｌ ｗｉｄｔｈ', '\uFEFFbom', 'nbsp\u00A0x', 'mixed 中 a 😀 é',
  'C:\\Users\\a b\\file.txt', '/unix/path/to/some/deeply/nested/file-name.js', 'claude-opus-4-7', 'claude-haiku-4-5-20251001[1m]',
  'claude-3-5-sonnet-20240620', 'claude-', 'opus', 'Sonnet', 'gpt-4o', '\uD83D', '\uDE00x', 'ﬁ ligature', 'İstanbul', '<synthetic>',
  'multi\nline\tcommand  with   spaces', '\x1b[1m\x1b[0m',
];
const TOOL_NAMES = ['Read', 'Write', 'Edit', 'Glob', 'Grep', 'Bash', 'Skill', 'Task', 'Agent', 'TodoWrite', 'TaskCreate', 'TaskUpdate',
  'mcp__github__create_issue', 'mcp__a__b__c', 'mcp____x', 'mcp__srv__', 'WebSearch', 'PowerShell', 'mcp__中文__工具', 'X'];
const MODELS = ['claude-opus-5-5', 'claude-sonnet-4-5-20250929', 'claude-haiku-4-5-20251001', 'claude-3-7-sonnet-20250219', 'gpt-5', '\x1b[1mweird\x1b[0m', 'claude-opus-4-1[1m]', '<synthetic>'];

let clock = Date.parse('2026-09-20T08:00:00.000Z');
const ids = [];
const iso = () => new Date((clock += int(0, 90000))).toISOString();

// Mostly string ids, sometimes a number, so Map's typed keys are exercised.
const someId = () => (ids.length && chance(0.4) ? pick(ids) : chance(0.05) ? int(1, 9) : `toolu_${crypto.randomUUID().slice(0, 8)}`);

function block() {
  if (ids.length && chance(0.35)) {
    const b = { type: 'tool_result', tool_use_id: pick(ids), content: pick(TEXT) };
    if (chance(0.2)) b.is_error = pick([true, 1, 'yes', false, 0]);
    return b;
  }
  const name = pick(TOOL_NAMES);
  const id = someId();
  ids.push(id);
  const input = {};
  if (['Read', 'Write', 'Edit'].includes(name)) {
    if (chance(0.8)) input.file_path = pick([...TEXT, null]);
    else input.path = pick(TEXT);
  }
  if (['Glob', 'Grep'].includes(name)) input.pattern = pick(TEXT);
  if (name === 'Bash') input.command = pick(TEXT) + (chance(0.5) ? '   \n\t ' + pick(TEXT) : '');
  if (name === 'Skill') input.skill = pick(TEXT);
  if (name === 'Task' || name === 'Agent') {
    if (chance(0.7)) input.subagent_type = pick([...TEXT, 5, null]);
    if (chance(0.6)) input.model = pick([...MODELS, 'opus', 'sonnet']);
    if (chance(0.8)) input.description = pick([...TEXT, 7]);
    if (chance(0.3)) input.run_in_background = true;
  }
  if (name === 'TodoWrite') {
    input.todos = Array.from({ length: int(0, 6) }, () => ({ content: pick([...TEXT, 3]), status: pick(['pending', 'in_progress', 'completed', 'done', 'running', 'weird']) }));
  }
  if (name === 'TaskCreate') {
    if (chance(0.7)) input.subject = pick(TEXT);
    if (chance(0.4)) input.description = pick(TEXT);
    if (chance(0.5)) input.status = pick(['pending', 'running', 'done', 'complete', 'x']);
    if (chance(0.5)) input.taskId = chance(0.5) ? String(int(1, 5)) : pick([int(1, 5), 0, '']);
  }
  if (name === 'TaskUpdate') {
    input.taskId = chance(0.5) ? String(int(1, 6)) : pick([int(1, 6), '007', 'x']);
    if (chance(0.7)) input.status = pick(['in_progress', 'completed', 'not_started']);
    if (chance(0.3)) input.subject = pick(TEXT);
  }
  return { type: 'tool_use', id, name, input: chance(0.03) ? pick([null, 'str', 5]) : input };
}

function entry() {
  const r = rnd();
  const e = {};
  if (chance(0.95)) e.timestamp = chance(0.97) ? iso() : pick([0, 1e12, true]);
  if (chance(0.05)) e.isSidechain = true;
  if (r < 0.4) {
    e.type = 'assistant';
    e.message = { id: chance(0.9) ? `msg_${int(1, 40)}` : undefined, model: pick(MODELS), content: Array.from({ length: int(0, 3) }, block) };
    if (chance(0.9)) {
      e.message.usage = { input_tokens: int(0, 5000), output_tokens: int(0, 20000), cache_creation_input_tokens: int(0, 90000),
        cache_read_input_tokens: chance(0.9) ? int(0, 900000) : null };
      if (chance(0.05)) e.message.usage = pick([5, 'x', { input_tokens: -3.7 }]);
    }
    if (chance(0.1)) e.advisorModel = pick(MODELS);
  } else if (r < 0.75) {
    e.type = 'user';
    if (chance(0.3)) {
      e.message = { content: pick(['hi', '<command-name>/x', '<local-command-stdout>Set effort level to ultracode', '<local-command-stdout>Set effort level to high',
        '<local-command-stdout>Set effort level to ultracodeX', pick(TEXT)]) };
    } else {
      e.message = { content: Array.from({ length: int(1, 3) }, () => (chance(0.7) ? block() : { type: 'text', text: pick(['<command-message>x', 'plain', pick(TEXT)]) })) };
      if (chance(0.3)) e.toolUseResult = { resolvedModel: pick(MODELS), isAsync: chance(0.3), status: pick(['async_launched', 'done']) };
    }
  } else if (r < 0.8) {
    Object.assign(e, { type: 'system', subtype: 'compact_boundary', compactMetadata: { postTokens: pick([int(0, 90000), -1, 'x', 1.5]) } });
  } else if (r < 0.85 && ids.length) {
    Object.assign(e, { type: 'queue-operation', operation: 'enqueue', content: `<task-id>t${int(1, 9)}</task-id><tool-use-id>${pick(ids)}</tool-use-id>` });
  } else if (r < 0.9) {
    Object.assign(e, { type: 'attachment', attachment: { type: pick(['ultra_effort_enter', 'ultra_effort_exit', 'other']) } });
  } else {
    e.type = pick(['progress', 'summary', 'file-history-snapshot', 'custom-title']);
  }
  return e;
}

function transcript() {
  ids.length = 0;
  clock = Date.parse('2026-09-20T08:00:00.000Z') + int(0, 86400000);
  const lines = [];
  for (let i = 0, n = int(5, 200); i < n; i++) {
    const r = rnd();
    if (r < 0.02) lines.push('');
    else if (r < 0.03) lines.push('   ');
    else if (r < 0.04) lines.push(pick(['null', '5', '[]', '"str"', '{"type":"assistant"', '{bad json}', '[{"type":"assistant"}]']));
    else lines.push(JSON.stringify(entry()));
  }
  let text = lines.join(chance(0.1) ? '\r\n' : '\n');
  if (chance(0.7)) text += '\n';
  if (chance(0.05)) text += '{"type":"user","timestamp":"2026-09-2';
  return text;
}

const B = [true, false];
const DISPLAY = {
  showModel: B, showProject: B, showAddedDirs: B, addedDirsLayout: ['inline', 'line'], showContextBar: B,
  contextValue: ['percent', 'tokens', 'remaining', 'both'], showConfigCounts: B, showCost: B, showRoutedCost: B, showDailyCost: B, showWeeklyCost: B,
  showDuration: B, showTokenBreakdown: B, showUsage: B, usageValue: ['percent', 'remaining'], usagePace: B,
  usageBarEnabled: B, showResetLabel: B, usageCompact: B, showModelScopedUsage: B,
  showTools: B, showSkills: B, showMcp: B, toolNameMaxLength: [0, 1, 5, 20, 2.5], toolsMaxVisible: [0, 1, 4, 10], skillsMaxVisible: [0, 1, 4],
  showAgents: B, showTodos: B, showSessionName: B, showAuth: B, showAuthUser: B,
  authUserLength: [0, 3, 8], showClaudeCodeVersion: B, showEffortLevel: B, effortFormat: ['full', 'symbol', 'text'],
  showPromptCache: B, showCacheHitRate: B, showSessionTokens: B, showOutputStyle: B, showMemoryUsage: B, showSpeed: B,
  showSessionStartDate: B, showLastResponseAt: B, showCompactions: B,
  contextWarningThreshold: [70, 0, 100, -5], contextCriticalThreshold: [85, 1], usageThreshold: [0, 50], sevenDayThreshold: [80, 0], environmentThreshold: [0, 20],
  modelFormat: ['full', 'compact', 'short'], modelOverride: ['', 'Custom │ Model'], modelSource: ['stdin', 'transcript', 'auto'], showProvider: B,
  providerName: ['', 'Prov', '  '], customLine: ['', 'custom | line 中文'], customLinePosition: ['first', 'last'],
  timeFormat: ['relative', 'absolute', 'both', 'elapsed', 'elapsedAndAbsolute'], hourCycle: ['auto', 'h11', 'h12', 'h23', 'h24'],
  showClockSeconds: B, showAdvisor: B, advisorOverride: ['', 'claude-sonnet-4-6'], autoCompactWindow: [null, 300000, 1.5],
  mergeGroups: [undefined, [], [['context', 'usage']], [['tools', 'agents', 'todos']], [['project', 'context'], ['usage', 'promptCache', 'environment', 'cacheHitRate']]],
  rightAlign: [[], ['usage'], ['todos', 'promptCache']],
};
const ELEMENTS = ['project', 'addedDirs', 'context', 'usage', 'promptCache', 'cacheHitRate', 'memory', 'environment', 'tools', 'skills', 'mcp', 'agents', 'todos', 'sessionTime'];

// An external usage snapshot for the case: valid, stale, broken, or with model-scoped windows.
function usageSnapshot(now) {
  const date = () => pick([new Date(now - int(0, 600000)).toISOString(), now - 1000, Math.floor(now / 1000), 'garbage', null]);
  const s = { updated_at: chance(0.9) ? date() : undefined };
  if (chance(0.7)) s.five_hour = { used_percentage: pick([0, 33.3, 90, 100, 'x']), resets_at: chance(0.7) ? date() : undefined };
  if (chance(0.7)) s.seven_day = { used_percentage: pick([0, 55, 80, 100]), resets_at: chance(0.7) ? date() : undefined };
  if (chance(0.4)) s.balance_label = pick([...TEXT, 'b'.repeat(60)]);
  if (chance(0.4)) s.model_scoped = Array.from({ length: int(0, 10) }, () => ({ display_name: pick(TEXT), utilization: pick([null, 12, 99.5, 'x']),
    resets_at: pick([new Date(now + 3600e3).toISOString(), 'bad', undefined]) }));
  return pick([JSON.stringify(s), JSON.stringify(s), 'null', '{bad']);
}

function config(sb, now) {
  const display = {};
  for (const [k, v] of Object.entries(DISPLAY)) if (chance(0.7)) display[k] = pick(v);
  if (chance(0.3)) {
    fs.writeFileSync(sb.usageIn, usageSnapshot(now));
    display.externalUsagePath = pick([sb.usageIn, '  ' + sb.usageIn + ' ', 'relative.json']);
    if (chance(0.5)) display.externalUsageFreshnessMs = pick([0, 300000, 1e12]);
  }
  if (chance(0.2)) display.externalUsageWritePath = pick([sb.usageOut, sb.usageOut.replace(/\.json$/, '.txt')]);
  const c = { gitStatus: { enabled: false }, display };
  if (chance(0.1)) c.jjStatus = { enabled: true, showDirty: chance(0.5), showConflicts: chance(0.5) };
  if (chance(0.3)) c.lineLayout = 'compact';
  if (chance(0.05)) c.layout = pick(['separators', 'default', { lineLayout: 'expanded', showSeparators: true, pathLevels: 2 }, 5]);
  if (chance(0.3)) c.elementOrder = ELEMENTS.filter(() => chance(0.8)).sort(() => rnd() - 0.5);
  if (chance(0.3)) c.projectLineOrder = ['cost', 'model', 'auth', 'project', 'duration', 'version'].filter(() => chance(0.5));
  if (chance(0.3)) c.pathLevels = pick([1, 2, 3, 'full', 4]);
  if (chance(0.2)) c.showSeparators = true;
  if (chance(0.2)) Object.assign(c, { maxWidth: pick([30, 60, 100, 0.5]), forceMaxWidth: chance(0.5) });
  if (chance(0.3)) c.colors = { label: pick(['cyan', 'dim', 196, '#aabbcc', 3.5]), barFilled: pick(['█', '■', '👍', 'ab', '\u0301']), context: pick(['green', 22, '#ff0000']) };
  return c;
}

const DIRS = ['C:\\测试 目录\\é#?%[x]', 'C:\\Users\\a b', 'D:\\', 'C:\\a\\..\\b\\.\\c\\', 'C:\\' + 'n'.repeat(40), 'E:\\x\\很长很长很长很长很长很长很长的名字'];

function stdin(tp, cwd, now) {
  const sec = Math.floor(now / 1000);
  const s = { session_id: chance(0.9) ? `s${int(1, 5)}` : pick(['', 5]), transcript_path: tp, cwd: chance(0.8) ? cwd : pick(DIRS) };
  const m = {};
  if (chance(0.8)) m.display_name = pick(['Opus 5.5', 'Sonnet 4.5 (1M context)', '', '  ', 'Claude Haiku 4.5', '中文模型', '\x1b[31mred\x1b[0m']);
  if (chance(0.9)) m.id = pick(['claude-opus-5-5[1m]', 'opusplan', 'us.anthropic.claude-sonnet-4-5-20250929-v1:0', 'claude-3-5-haiku@20241022', 'claude-fable-5-1', 'minimax-m2.7']);
  s.model = m;
  if (chance(0.5)) s.version = pick(['2.1.295', '', '\x1b[2mx\x1b[0m', 'v'.repeat(40)]);
  if (chance(0.4)) s.session_name = pick(TEXT);
  if (chance(0.4)) s.output_style = { name: pick(TEXT) };
  if (chance(0.8)) s.cost = { total_cost_usd: pick([0, 0.0049, 0.05, 0.12345, 1.005, 12.345, 999.995, -1]), total_duration_ms: pick([0, 59999, 60000, 3600000 * 5, -1]),
    total_api_duration_ms: pick([0, 400, 5000, 90000]) };
  if (chance(0.9)) {
    s.context_window = { context_window_size: pick([200000, 1000000, 0]), current_usage: { input_tokens: int(0, 200000), output_tokens: int(0, 9000), cache_creation_input_tokens: int(0, 9000), cache_read_input_tokens: int(0, 900000) } };
    if (chance(0.6)) s.context_window.used_percentage = pick([0, 0.4, 12.5, 50.5, 99.9, 100, 140]);
    if (chance(0.2)) s.context_window.current_usage = { input_tokens: 0 };
  }
  if (chance(0.7)) {
    s.rate_limits = {};
    if (chance(0.8)) s.rate_limits.five_hour = { used_percentage: pick([0, 12.4, 74.5, 75, 89.5, 90, 100, 120]), resets_at: sec + pick([-10, 30, 3600, 9000, 17000]) };
    if (chance(0.6)) s.rate_limits.seven_day = { used_percentage: pick([0, 50, 80, 99, 100]), resets_at: sec + pick([60, 400000, 600000]) };
  }
  if (chance(0.5)) s.prompt_cache = { caching_observed: pick([true, false, 1]), warm: pick([true, false]), expires_at: sec + pick([-5, 30, 100, 250, 3000]),
    ttl: pick(['5m', '1h', undefined, 'x']), hit_ratio: pick([0, 0.5, 0.99999, 1.2, undefined]) };
  if (chance(0.6)) s.effort = pick([{ level: 'high' }, { level: 'MAX ' }, { level: '' }, { level: 'weird' }, 'high', { level: 'ultracode' }]);
  if (chance(0.3)) s.workspace = { added_dirs: Array.from({ length: int(0, 7) }, () => pick([...DIRS, '', 'C:\\x\\', '\\\\'])) };
  return s;
}

function sandbox(k) {
  const proj = path.join(workDir, `f${k}`, 'proj');
  const cfg = path.join(proj, '.claude');
  if (!fs.existsSync(cfg)) {
    fs.mkdirSync(path.join(cfg, 'plugins', 'claude-hud'), { recursive: true });
    fs.mkdirSync(path.join(cfg, 'rules', 'sub'), { recursive: true });
    fs.writeFileSync(path.join(cfg, 'rules', 'a.md'), '');
    fs.writeFileSync(path.join(cfg, 'rules', 'sub', 'b.md'), '');
    fs.writeFileSync(path.join(cfg, 'settings.json'), JSON.stringify({ hooks: { A: [], B: [] }, mcpServers: { x: {} } }));
    fs.writeFileSync(path.join(proj, '.mcp.json'), JSON.stringify({ mcpServers: { p: {}, q: {} } }));
    fs.mkdirSync(path.join(proj, '.jj'));
    fs.writeFileSync(path.join(cfg, '.claude.json'), JSON.stringify({ oauthAccount: { organizationType: 'claude_team_plan', organizationRateLimitTier: 'tier_5X', displayName: 'Someone 中文' } }));
  }
  return { proj, cfg, moved: path.join(workDir, `f${k}`, 'moved'), cache: path.join(workDir, `f${k}`, 'hud-cache'), usageIn: path.join(proj, 'usage-in.json'), usageOut: path.join(proj, 'usage-out.json') };
}

const stats = { cases: 0, equal: 0, diff: 0, unsupported: {} };
const kept = [];
let done = 0;
async function worker(k) {
  const sb = sandbox(k);
  while (done < total) {
    done++;
    const tp = path.join(sb.proj, `t${done}.jsonl`);
    fs.writeFileSync(tp, transcript());
    const now = clock + pick([-5000, 10000, 200000, 4000000]);
    const input = JSON.stringify(stdin(tp, sb.proj, now));
    const conf = config(sb, now);
    fs.writeFileSync(path.join(sb.cfg, 'plugins', 'claude-hud', 'config.json'), JSON.stringify(conf));
    const env = { ...process.env, CLAUDE_CONFIG_DIR: sb.cfg, HUD_CACHE_DIR: sb.cache, HUD_FAKE_NOW_MS: String(now), HUD_FAKE_MEM: pick(['17179869184:5368709120', '1000:999', '68719476736:0']),
      COLUMNS: String(int(1, 250)) };
    if (chance(0.1)) delete env.COLUMNS;
    const jjLogs = [path.join(sb.proj, 'jj-c.log'), path.join(sb.proj, 'jj-ref.log')];
    if (conf.jjStatus?.enabled && fakeJj) {
      const pathKey = Object.keys(env).find((k) => k.toUpperCase() === 'PATH') ?? 'PATH';
      env[pathKey] = `${fakeJj}${path.delimiter}${env[pathKey]}`;
      env.FAKE_JJ_OUT = pick(['abc12345\x1fmain\x1e2nd\x1f1\x1f0\n', 'abc12345\x1f\x1f0\x1f1\r\n', 'zz\x1f\x1f2\x1f0', 'a\x1fb\x1e\x1f1\x1f0', 'x\ny\x1f\x1f1\x1f0',
        ` \x1b[31m${pick(TEXT)}\x1f${pick(TEXT)}\x1f1\x1f1`, 'q'.repeat(20000)]);
      env.FAKE_JJ_EXIT = pick(['0', '0', '0', '1']);
    }
    const extra = [sb.usageOut];
    const s0 = snapshot(sb.cfg, extra);
    const rc = await run(hudExe, [], { ...env, FAKE_JJ_LOG: jjLogs[0] }, input);
    const sc = snapshot(sb.cfg, extra);
    restore(s0, sc, sb.moved);
    const rn = await runReference(ref, { ...env, FAKE_JJ_LOG: jjLogs[1] }, input);
    const sn = snapshot(sb.cfg, extra);
    const jjArgs = jjLogs.map((p) => (fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : ''));
    fs.mkdirSync(sb.moved, { recursive: true });
    for (const p of jjLogs) if (fs.existsSync(p)) fs.renameSync(p, path.join(sb.moved, crypto.randomUUID()));
    restore(s0, sn, sb.moved);
    checkTestBuild(hudExe, ref, rc, rn);
    stats.cases++;
    if (rc.code === 3) {
      const why = rc.err.trim();
      stats.unsupported[why] = (stats.unsupported[why] ?? 0) + 1;
      if (ref.kind === 'upstream') continue;
    }
    const ok = matches(ref, rc, rn) && contents(sc) === contents(sn) && jjArgs[0] === jjArgs[1];
    if (ok) stats.equal++;
    else {
      stats.diff++;
      const keep = path.join(workDir, `diff-${stats.diff}`);
      fs.mkdirSync(keep, { recursive: true });
      fs.copyFileSync(tp, path.join(keep, 'transcript.jsonl'));
      fs.writeFileSync(path.join(keep, 'stdin.json'), input);
      fs.copyFileSync(path.join(sb.cfg, 'plugins', 'claude-hud', 'config.json'), path.join(keep, 'config.json'));
      fs.writeFileSync(path.join(keep, 'env.json'), JSON.stringify({ now, COLUMNS: env.COLUMNS ?? null, HUD_FAKE_MEM: env.HUD_FAKE_MEM }));
      fs.writeFileSync(path.join(keep, 'c.txt'), rc.out);
      fs.writeFileSync(path.join(keep, 'ref.txt'), rn.out);
      fs.writeFileSync(path.join(keep, 'files-c.txt'), contents(sc));
      fs.writeFileSync(path.join(keep, 'files-ref.txt'), contents(sn));
      if (rc.err) fs.writeFileSync(path.join(keep, 'c-err.txt'), rc.err);
      fs.writeFileSync(path.join(keep, 'jj-args.txt'), jjArgs.join('--- reference\n'));
      if (kept.length < 10) kept.push(keep);
    }
  }
}
await Promise.all(Array.from({ length: 6 }, (_, k) => worker(k)));
console.log(`reference ${ref.kind} ${ref.path}`);
console.log(`cases ${stats.cases}, equal ${stats.equal}, diff ${stats.diff}`);
console.log('unsupported:', JSON.stringify(stats.unsupported));
for (const d of kept) console.log('kept', d);
// Nothing compared, as when every case is reported unsupported, is no pass.
process.exitCode = stats.diff > 0 || stats.equal + stats.diff === 0 ? 1 : 0;
