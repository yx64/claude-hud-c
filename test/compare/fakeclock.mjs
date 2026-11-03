// Preloaded into the upstream HUD with `node --import`: freezes Date at HUD_FAKE_NOW_MS and the
// memory figures at HUD_FAKE_MEM (<Total>:<Free> bytes), the values the test build of claude-hud-c
// reads from the same variables, so time- and memory-dependent text matches byte for byte.
import cp from 'node:child_process';
import fs from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const fixed = Number(process.env.HUD_FAKE_NOW_MS);
if (fixed > 0) {
  const Real = Date;
  class FakeDate extends Real {
    constructor(...a) {
      if (a.length === 0) super(fixed);
      else super(...a);
    }
    static now() {
      return fixed;
    }
  }
  globalThis.Date = FakeDate;
}

const mem = /^(\d+):(\d+)$/.exec(process.env.HUD_FAKE_MEM ?? '');
if (mem) {
  os.totalmem = () => Number(mem[1]);
  os.freemem = () => Number(mem[2]);
  // On Linux upstream reads /proc/meminfo first, in kB, which cannot carry every byte count; made
  // unreadable, it falls back to the two figures above.
  const statSync = fs.statSync;
  fs.statSync = (p, ...rest) => {
    if (p === '/proc/meminfo') throw new Error('faked away');
    return statSync(p, ...rest);
  };
}

// Upstream runs its logic in a second node under the permission model, with an environment of its
// own choosing; that one gets this file and the two variables as well.
const spawn = cp.spawn;
cp.spawn = (cmd, args, opts) => {
  if (cmd === process.execPath && Array.isArray(args) && args.includes('--permission')) {
    args = [`--allow-fs-read=${fileURLToPath(import.meta.url)}`, `--import=${import.meta.url}`, ...args];
    const fake = { HUD_FAKE_NOW_MS: process.env.HUD_FAKE_NOW_MS ?? '', HUD_FAKE_MEM: process.env.HUD_FAKE_MEM ?? '' };
    opts = { ...opts, env: { ...opts?.env, ...fake } };
  }
  return spawn(cmd, args, opts);
};
syncBuiltinESMExports();
