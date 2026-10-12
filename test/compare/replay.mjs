// Replays stdin captured from live refreshes (the observation build's capture directory) through the test
// build and a reference (common.mjs) at one fixed clock, and compares the output and the written files
// byte for byte. The refreshes run against a sandbox copy of the real config dir, so the live
// files are not touched. Usage:
//   node replay.mjs <HudTestExe> <Reference> <CaptureDir> <WorkDir>
// Exit code: 1 when some output differs or nothing was compared, 2 when nothing differs but
// some input is not reproduced exactly, 0 otherwise.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { checkTestBuild, contents, reference, restore, run, runReference, snapshot } from './common.mjs';

const [hudExe, refArg, capDir, workDir] = process.argv.slice(2);
const ref = reference(refArg);
const realClaude = path.join(os.homedir(), '.claude');
fs.mkdirSync(workDir, { recursive: true });

// Refreshed on every run, so a reused work dir never compares against a stale config. The
// source is resolved first: cpSync checks a linked source by the link's own type despite
// dereference, so once the destination exists from an earlier run, the copy fails.
const cfg = path.join(workDir, 'claude');
fs.mkdirSync(path.join(cfg, 'plugins', 'claude-hud'), { recursive: true });
const copyIfPresent = (from, to) => {
  if (fs.existsSync(from)) fs.cpSync(fs.realpathSync(from), to, { recursive: true, dereference: true, force: true });
};
for (const f of ['settings.json', 'settings.local.json', 'CLAUDE.md', 'claude-hud.json', 'rules']) copyIfPresent(path.join(realClaude, f), path.join(cfg, f));
copyIfPresent(path.join(realClaude, 'plugins', 'claude-hud', 'config.json'), path.join(cfg, 'plugins', 'claude-hud', 'config.json'));
// With CLAUDE_CONFIG_DIR set, .claude.json is read from inside it.
copyIfPresent(path.join(os.homedir(), '.claude.json'), path.join(cfg, '.claude.json'));

const files = fs.readdirSync(capDir).filter((f) => f.endsWith('.json')).sort();
let equal = 0, diff = 0, unsupported = 0;
const now = Date.now();
for (const f of files) {
  const input = fs.readFileSync(path.join(capDir, f));
  const env = { ...process.env, CLAUDE_CONFIG_DIR: cfg, HUD_CACHE_DIR: path.join(workDir, 'hud-cache'), HUD_FAKE_NOW_MS: String(now), HUD_FAKE_MEM: '17179869184:5368709120' };
  const s0 = snapshot(cfg);
  const rc = await run(hudExe, [], env, input);
  const sc = snapshot(cfg);
  restore(s0, sc, path.join(workDir, 'moved'));
  const rn = await runReference(ref, env, input);
  const sn = snapshot(cfg);
  restore(s0, sn, path.join(workDir, 'moved'));
  checkTestBuild(hudExe, ref, rc, rn);
  if (rc.code === 3) {
    unsupported++;
    console.log(f, rc.err.trim());
  } else if (rc.code === 0 && rc.out.equals(rn.out) && contents(sc) === contents(sn)) equal++;
  else {
    diff++;
    console.log(f, `DIFF\n--- C\n${rc.out}--- reference\n${rn.out}--- files C\n${contents(sc)}\n--- files reference\n${contents(sn)}`);
  }
}
console.log(`reference ${ref.kind} ${ref.path}`);
console.log(`captured ${files.length}, equal ${equal}, diff ${diff}, unsupported ${unsupported}`);
process.exitCode = diff > 0 || equal + unsupported === 0 ? 1 : unsupported > 0 ? 2 : 0;
