// The saved transcript state: a refresh that resumes from it must print exactly what a full parse
// prints. Transcripts (copies of real ones, and synthetic ones mixing every line terminator) grow
// in place cut point by cut point: at line ends, inside lines, right after a \r, inside the three
// bytes of U+2028. At each step the test build, resuming, is compared with the reference parsing
// the same bytes, and the saved state must sit right after the last complete line. Then the cases
// where a saved state must not be taken up: the file rewritten in its first bytes or just before
// the offset, replaced by another file, cut shorter, the state file damaged or cut short; two
// transcripts with one file name, whose states share a name; plus an unsupported case inside the
// saved part, and a line that takes the clock. Usage:
//   node cache.mjs <HudTestExe> <Reference> <WorkDir> [maxRealTranscripts]
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { checkTestBuild, matches, reference, run, runReference } from './common.mjs';

const [hudExe, refArg, workDirArg, maxArg] = process.argv.slice(2);
const ref = reference(refArg);
const workDir = path.resolve(workDirArg);
const maxReal = Number(maxArg ?? 8);
const realClaude = path.join(os.homedir(), '.claude');
fs.mkdirSync(workDir, { recursive: true });

// Everything the transcript feeds is shown; nothing that writes runtime files is.
const cfg = path.join(workDir, 'claude');
fs.mkdirSync(path.join(cfg, 'plugins', 'claude-hud'), { recursive: true });
fs.writeFileSync(path.join(cfg, 'plugins', 'claude-hud', 'config.json'), JSON.stringify({
  gitStatus: { enabled: false },
  display: { showTools: true, showSkills: true, showMcp: true, showAgents: true, showTodos: true, showSessionTokens: true, showCompactions: true,
    showAdvisor: true, showSessionStartDate: true, showLastResponseAt: true, showEffortLevel: true, showConfigCounts: true, modelSource: 'auto',
    toolsMaxVisible: 0, skillsMaxVisible: 0 },
}));
const NOW = Date.parse('2026-10-10T12:00:00Z');

let seq = 0;
const stats = { steps: 0, equal: 0, diff: 0, offsetWrong: 0, unsupported: 0 };
const failures = [];

async function refresh(tp, cache, extraEnv = {}) {
  const input = JSON.stringify({ session_id: 's', transcript_path: tp, cwd: workDir, model: { id: 'claude-opus-5-5', display_name: 'Opus' },
    effort: { level: 'high' } });
  const env = { ...process.env, CLAUDE_CONFIG_DIR: cfg, HUD_FAKE_NOW_MS: String(NOW), HUD_FAKE_MEM: '17179869184:5368709120', ...extraEnv };
  delete env.COLUMNS;
  const rc = await run(hudExe, [], { ...env, HUD_CACHE_DIR: cache }, input);
  const rn = await runReference(ref, { ...env, HUD_CACHE_DIR: `${cache}-reference` }, input);
  checkTestBuild(hudExe, ref, rc, rn);
  return { rc, rn };
}

// Same output, or both builds report the same unsupported case (common.mjs: matches).
function compare(label, { rc, rn }) {
  stats.steps++;
  if (rc.code === 3) stats.unsupported++;
  if (rc.code === 3 && ref.kind === 'upstream') return true;
  const same = matches(ref, rc, rn);
  if (same) stats.equal++;
  else {
    stats.diff++;
    if (failures.length < 20) failures.push({ label, code: [rc.code, rn.code], c: rc.out.toString(), n: rn.out.toString(), cerr: rc.err, nerr: rn.err });
  }
  return same;
}

// Where the saved state should be after a refresh of buf: right after the last line that is
// complete, as parse_transcript decides it (readline's terminators; a \r at the very end may
// still become \r\n).
function resumeOffset(buf) {
  let i = 0;
  for (;;) {
    let j = i;
    let term = 1;
    for (; j < buf.length; j++) {
      const b = buf[j];
      if (b === 0x0a || b === 0x0d) break;
      if (b === 0xe2 && j + 2 < buf.length && buf[j + 1] === 0x80 && (buf[j + 2] === 0xa8 || buf[j + 2] === 0xa9)) {
        term = 3;
        break;
      }
    }
    if (j >= buf.length || (buf[j] === 0x0d && j + 1 === buf.length)) return i;
    if (buf[j] === 0x0d && buf[j + 1] === 0x0a) j++;
    i = j + term;
    if (i === buf.length) return i;
  }
}

// The offset recorded in the state saved for tp, or null when there is none: <Cache>/<name>.bin for
// <name>.jsonl, the offset at byte 8 (transcript-state.c's Header).
function savedOffset(cache, tp) {
  const f = path.join(cache, `${path.basename(tp, '.jsonl')}.bin`);
  return fs.existsSync(f) ? Number(fs.readFileSync(f).readBigUInt64LE(8)) : null;
}

// Grows tp from nothing through the cut points, comparing at each step. The saved state must sit
// right after the last complete line, or, given a limit, never past it.
async function grow(name, bytes, cuts) {
  const tp = path.join(workDir, 't', `${name}.jsonl`);
  const cache = path.join(workDir, 'cache', `${name}-${seq++}`);
  fs.mkdirSync(path.dirname(tp), { recursive: true });
  fs.writeFileSync(tp, '');
  let at = 0;
  for (const cut of [...cuts, cuts[cuts.length - 1]]) {
    if (cut > at) fs.appendFileSync(tp, bytes.subarray(at, cut));
    at = cut;
    compare(`${name} @${cut}`, await refresh(tp, cache));
    const want = resumeOffset(bytes.subarray(0, cut)) || null;
    const got = savedOffset(cache, tp);
    if (got !== want) {
      stats.offsetWrong++;
      if (failures.length < 20) failures.push({ label: `${name} @${cut}`, savedOffset: got, expected: want });
    }
  }
  return { tp, cache };
}

// ---- synthetic transcripts ----

const TERMS = ['\n', '\r\n', '\r', '\u2028', '\u2029'];
function synthetic(lines, termOf) {
  return Buffer.from(lines.map((l, i) => JSON.stringify(l) + termOf(i)).join(''), 'utf8');
}

function syntheticLines(n, t0 = Date.parse('2026-10-10T10:00:00Z')) {
  const ts = (i) => new Date(t0 + i * 1000).toISOString();
  const lines = [];
  for (let i = 0; i < n; i++) {
    const tool = ['Read', 'Edit', 'Bash', 'Grep', 'mcp__srv__call', 'Skill', 'Task', 'TodoWrite', 'TaskCreate', 'TaskUpdate'][i % 10];
    const input = tool === 'Skill' ? { skill: `skill-${i % 3}` }
      : tool === 'Task' ? { subagent_type: 'general', description: `job ${i} …─✓`, model: 'claude-haiku-4-5-20251001', run_in_background: i % 4 === 0 }
      : tool === 'TodoWrite' ? { todos: [{ content: `step ${i}`, status: 'completed' }, { content: `next ${i}`, status: 'in_progress' }] }
      : tool === 'TaskCreate' ? { subject: `task ${i}`, taskId: String(i) }
      : tool === 'TaskUpdate' ? { taskId: String(i - 1), status: 'completed' }
      : tool === 'Bash' ? { command: `echo ${'x'.repeat(i % 40)}` }
      : { file_path: `C:\\work\\${'深'.repeat(i % 5)}file-${i}.txt`, pattern: 'a.*b' };
    lines.push({ type: 'assistant', timestamp: ts(i), requestId: `r${i}`, advisorModel: i % 7 === 0 ? 'claude-opus-4-7' : undefined,
      message: { id: `msg_${Math.floor(i / 2)}`, model: i % 5 === 0 ? 'claude-sonnet-4-5' : 'claude-opus-5-5',
        usage: { input_tokens: i, output_tokens: 2 * i, cache_creation_input_tokens: i % 3, cache_read_input_tokens: 100 * i },
        content: [{ type: 'tool_use', id: `toolu_${i}`, name: tool, input }] } });
    if (i % 3 !== 2) {
      lines.push({ type: 'user', timestamp: ts(i), toolUseResult: { status: i % 8 === 0 ? 'async_launched' : 'done' },
        message: { content: [{ type: 'tool_result', tool_use_id: `toolu_${i}`, is_error: i % 9 === 4, content: 'ok' }] } });
    }
    if (i % 11 === 5) lines.push({ type: 'system', subtype: 'compact_boundary', timestamp: ts(i), compactMetadata: { postTokens: 1000 * i } });
    if (i % 13 === 6) lines.push({ type: 'queue-operation', operation: 'enqueue', timestamp: ts(i), content: `<tool-use-id>toolu_${i - 2}</tool-use-id><task-id>k${i}</task-id>` });
    if (i % 17 === 3) lines.push({ type: 'user', timestamp: ts(i), message: { content: `<local-command-stdout>Set effort level to ${i % 2 ? 'ultracode' : 'max'} (x)` } });
    if (i % 6 === 1) lines.push({ type: 'assistant', timestamp: ts(i), message: { usage: { input_tokens: 5, output_tokens: 5 } } });
    if (i % 5 === 3) lines.push({ type: 'last-prompt', lastPrompt: `p ${i}` });
  }
  return lines;
}

// Every terminator's end, and the bytes either side of it; inside a three-byte terminator too.
function terminatorCuts(bytes) {
  const cuts = new Set();
  for (let j = 0; j < bytes.length; j++) {
    const b = bytes[j];
    if (b === 0x0a || b === 0x0d) [j, j + 1, j + 2].forEach((c) => cuts.add(c));
    if (b === 0xe2 && bytes[j + 1] === 0x80 && (bytes[j + 2] === 0xa8 || bytes[j + 2] === 0xa9)) [j + 1, j + 2, j + 3, j + 4].forEach((c) => cuts.add(c));
  }
  cuts.add(bytes.length);
  return [...cuts].filter((c) => c > 0 && c <= bytes.length).sort((a, b) => a - b);
}

for (let t = 0; t < TERMS.length + 1; t++) {
  const lines = syntheticLines(40);
  const bytes = synthetic(lines, (i) => (t < TERMS.length ? TERMS[t] : TERMS[i % TERMS.length]));
  const cuts = terminatorCuts(bytes).filter((_, k, all) => k % 3 === 0 || k === all.length - 1);
  await grow(`synthetic-${t}`, bytes, cuts);
}

// ---- real transcripts ----

const real = [];
const walk = (d) => {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p);
    else if (e.name.endsWith('.jsonl')) real.push(p);
  }
};
walk(path.join(realClaude, 'projects'));
real.sort((a, b) => fs.statSync(b).size - fs.statSync(a).size);
let seed = 11;
const pick = (n) => Math.floor((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648 * n);
for (const src of real.slice(0, maxReal)) {
  const bytes = fs.readFileSync(src);
  const ends = [];
  for (let j = 0; j < bytes.length; j++) if (bytes[j] === 0x0a) ends.push(j + 1);
  const cuts = new Set([bytes.length]);
  for (let k = 0; k < 6 && ends.length; k++) {
    const e = ends[pick(ends.length)];
    cuts.add(e);
    cuts.add(Math.max(1, e - 1 - pick(200)));
  }
  await grow(`real-${path.basename(src, '.jsonl')}`, bytes, [...cuts].sort((a, b) => a - b));
}

// ---- a saved state that must not be taken up ----

// The same length with a tool name the tools line shows changed, at a position that must fall in
// [lo, hi): where the change lands decides which check is being tested.
function rewriteAt(bytes, needle, at, lo, hi) {
  if (at < lo || at >= hi) throw new Error(`${needle} at ${at}, outside [${lo}, ${hi}): the case would not test what it says`);
  const copy = Buffer.from(bytes);
  Buffer.from(needle.replace('Read', 'Gret').replace('Edit', 'Edix')).copy(copy, at);
  return copy;
}

{
  const bytes = synthetic(syntheticLines(15), () => '\n');
  const { tp, cache } = await grow('invalidate', bytes, [bytes.length]);
  const read = '"name":"Read"', edit = '"name":"Edit"';
  const steps = [
    ['first bytes rewritten', () => fs.writeFileSync(tp, rewriteAt(bytes, read, bytes.indexOf(read), 0, 4096 - read.length))],
    ['bytes before the offset rewritten', () => fs.writeFileSync(tp, rewriteAt(bytes, edit, bytes.lastIndexOf(edit), Math.max(4096, bytes.length - 4096), bytes.length))],
    ['file replaced', () => {
      const other = `${tp}.new`;
      fs.writeFileSync(other, rewriteAt(bytes, read, bytes.indexOf(read, bytes.length / 2), 0, bytes.length));
      fs.renameSync(other, tp);
    }],
    ['file cut shorter', () => fs.writeFileSync(tp, bytes.subarray(0, Math.floor(bytes.length / 2)))],
    ['state file damaged', () => {
      for (const f of fs.readdirSync(cache)) fs.writeFileSync(path.join(cache, f), Buffer.from('not a state'));
    }],
    ['state file cut short', () => {
      for (const f of fs.readdirSync(cache)) {
        const b = fs.readFileSync(path.join(cache, f));
        fs.writeFileSync(path.join(cache, f), b.subarray(0, Math.floor(b.length / 2)));
      }
    }],
  ];
  for (const [label, act] of steps) {
    fs.writeFileSync(tp, bytes);
    compare(`invalidate: setup for ${label}`, await refresh(tp, cache));
    act();
    compare(`invalidate: ${label}`, await refresh(tp, cache));
  }
}

// A replacing file whose bytes differ only between the two windows: only its file id tells.
{
  const filler = (k) => ({ type: 'last-prompt', lastPrompt: 'f'.repeat(5000), k });
  const bytes = synthetic([filler(1), ...syntheticLines(15), filler(2)], () => '\n');
  const { tp, cache } = await grow('replaced', bytes, [bytes.length]);
  const read = '"name":"Read"', other = `${tp}.new`;
  fs.writeFileSync(other, rewriteAt(bytes, read, bytes.indexOf(read), 4096, bytes.length - 4096 - read.length));
  fs.renameSync(other, tp);
  compare('replaced, differing only between the windows', await refresh(tp, cache));
}

// Two transcripts with one file name in different directories share a state file name: each must
// only ever resume its own state.
{
  const cache = path.join(workDir, 'cache', `same-name-${seq++}`);
  const a = path.join(workDir, 't', 'one', 'same.jsonl'), b = path.join(workDir, 't', 'two', 'same.jsonl');
  fs.mkdirSync(path.dirname(a), { recursive: true });
  fs.mkdirSync(path.dirname(b), { recursive: true });
  const bytesA = synthetic(syntheticLines(10), () => '\n');
  const bytesB = synthetic(syntheticLines(14, Date.parse('2026-10-09T08:00:00Z')), () => '\r\n');
  fs.writeFileSync(a, bytesA.subarray(0, bytesA.length / 2));
  fs.writeFileSync(b, bytesB.subarray(0, bytesB.length / 2));
  for (let k = 0; k < 3; k++) {
    if (k === 1) fs.appendFileSync(a, bytesA.subarray(bytesA.length / 2)), fs.appendFileSync(b, bytesB.subarray(bytesB.length / 2));
    compare(`same name, first, round ${k}`, await refresh(a, cache));
    compare(`same name, second, round ${k}`, await refresh(b, cache));
  }
}

// An unsupported case early in the file stays reported once that part comes from the saved state.
{
  const lines = syntheticLines(12);
  lines.splice(2, 0, { type: 'user', timestamp: 'October 10, 2026', message: { content: 'x' } });
  const bytes = synthetic(lines, () => '\n');
  await grow('unsupported', bytes, [Math.floor(bytes.length / 2), bytes.length]);
}

// An agent's tool line without a time takes the clock of the refresh parsing it; a state is saved past
// it all the same, and holds no clock, so later refreshes still agree with a full parse at their own clock.
{
  const lines = syntheticLines(8);
  const timeless = { type: 'assistant', message: { id: 'nt', content: [{ type: 'tool_use', id: 'toolu_nt', name: 'Task', input: { description: 'no time' } }] } };
  lines.splice(6, 0, timeless);
  const bytes = synthetic(lines, () => '\n');
  const lineStart = bytes.indexOf(Buffer.from(JSON.stringify(timeless)));
  await grow('timeless', bytes, [Math.floor(lineStart / 2), lineStart + 10, bytes.length]);
}

const report = path.join(workDir, 'cache-report.json');
fs.writeFileSync(report, JSON.stringify({ reference: ref, stats, failures }, null, 2));
console.log(`reference ${ref.kind} ${ref.path}`);
console.log(`steps ${stats.steps}, equal ${stats.equal}, diff ${stats.diff}, saved offset wrong ${stats.offsetWrong}, unsupported ${stats.unsupported}`);
console.log(`report: ${report}`);
process.exitCode = stats.diff || stats.offsetWrong || !stats.equal ? 1 : 0;
