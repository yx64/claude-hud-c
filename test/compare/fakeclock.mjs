// Preloaded into the upstream HUD with `node --import`: freezes Date at HUD_FAKE_NOW_MS and the
// memory figures at HUD_FAKE_MEM (<Total>:<Free> bytes), the values the test build of claude-hud-c
// reads from the same variables, so time- and memory-dependent text matches byte for byte.
import fsp from 'node:fs/promises';
import { syncBuiltinESMExports } from 'node:module';
import os from 'node:os';

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
  const readFile = fsp.readFile;
  fsp.readFile = (p, ...rest) => (p === '/proc/meminfo' ? Promise.reject(new Error('faked away')) : readFile(p, ...rest));
  syncBuiltinESMExports();
}
