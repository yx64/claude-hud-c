// Shared by the comparison scripts: the reference a test build is compared against, running a
// refresh on either side, and the runtime files a refresh writes.
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

// The upstream version the port reproduces. Moving to another one is a deliberate step: change
// this, run the comparison scripts against that version's source, and follow up on every difference.
export const VERSION = '0.10.0';

const runner = fileURLToPath(new URL('./run-upstream.mjs', import.meta.url));
const fakeClock = fileURLToPath(new URL('./fakeclock.mjs', import.meta.url));

// A release or observation build appends " <N>ms" to the last line and cannot freeze its clock;
// comparing one, on either side, reports every case as different.
const RUN_TIME = / \x1b\[32m\d+ms\x1b\[0m\n$/;
export function checkTestBuild(exe, ref, c, n) {
  const timed = (r) => r.code === 0 && RUN_TIME.test(r.out.toString());
  const which = timed(c) && !timed(n) ? exe : ref.kind === 'exe' && timed(n) && !timed(c) ? ref.path : null;
  if (which) {
    console.error(`${which} is not a test build (its output ends with the run time); pass claude-hud-c-test from the build directory`);
    process.exit(4);
  }
}

// The reference named on the command line, one of:
//   - an upstream claude-hud source tree of VERSION with its built dist/, when following upstream;
//   - an earlier test build, when the code changes and the output must not.
// Nothing is looked up or downloaded: comparing against a source the caller did not choose
// proves nothing about the one they meant.
export function reference(arg) {
  if (!arg) throw new Error('no reference given: pass an upstream claude-hud source directory or an earlier claude-hud-c-test');
  const p = path.resolve(arg);
  if (fs.statSync(p, { throwIfNoEntry: false })?.isFile()) {
    if (process.platform === 'win32' && !p.toLowerCase().endsWith('.exe')) throw new Error(`reference ${p} is a file but not an .exe`);
    return { kind: 'exe', path: p };
  }
  if (!fs.existsSync(path.join(p, 'dist', 'index.js'))) throw new Error(`reference ${p} has no dist/index.js; build the upstream source first`);
  const pkg = JSON.parse(fs.readFileSync(path.join(p, 'package.json'), 'utf8'));
  if (pkg.name !== 'claude-hud' || pkg.version !== VERSION) throw new Error(`reference ${p} is ${pkg.name} ${pkg.version}, not claude-hud ${VERSION} (test/common.mjs VERSION)`);
  return { kind: 'upstream', path: p };
}

export function run(cmd, args, env, input) {
  return new Promise((resolve) => {
    const p = spawn(cmd, args, { env, windowsHide: true });
    const out = [], err = [];
    p.stdout.on('data', (d) => out.push(d));
    p.stderr.on('data', (d) => err.push(d));
    p.on('close', (code) => resolve({ code, out: Buffer.concat(out), err: Buffer.concat(err).toString() }));
    p.stdin.end(input);
  });
}

export const runReference = (ref, env, input) => ref.kind === 'exe'
  ? run(ref.path, [], env, input)
  : run(process.execPath, ['--import', pathToFileURL(fakeClock).href, runner, ref.path], env, input);

// The files a refresh may write, with their times: everything under <ConfigDir>/plugins/claude-hud
// but the config, plus extra paths such as a usage snapshot.
export function snapshot(cfgDir, extra = []) {
  const files = new Map();
  const walk = (d) => {
    if (!fs.existsSync(d)) return;
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (e.isFile() && e.name !== 'config.json') files.set(p, null);
    }
  };
  walk(path.join(cfgDir, 'plugins', 'claude-hud'));
  for (const p of extra) if (fs.existsSync(p)) files.set(p, null);
  for (const p of files.keys()) {
    const st = fs.statSync(p);
    files.set(p, { data: fs.readFileSync(p), atime: st.atime, mtime: st.mtime });
  }
  return files;
}

// Puts the files back as they were; a file that did not exist moves into movedDir rather than
// being deleted.
export function restore(before, after, movedDir) {
  for (const [p, s] of before) {
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, s.data);
    fs.utimesSync(p, s.atime, s.mtime);
  }
  for (const p of after.keys()) {
    if (before.has(p) || !fs.existsSync(p)) continue;
    fs.mkdirSync(movedDir, { recursive: true });
    fs.renameSync(p, path.join(movedDir, crypto.randomUUID()));
  }
}

// The written contents as text, for comparing and reporting; times are not compared.
export function contents(files) {
  return [...files].map(([p, s]) => `${p}\n${s.data.toString()}`).sort().join('\n---\n');
}
