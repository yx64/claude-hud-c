// Compares uniprobe (utf8proc) with this node's regex properties and Intl.Segmenter.
import { execFileSync } from 'node:child_process';

const exe = process.argv[2];
const props = execFileSync(exe, ['props'], { maxBuffer: 1 << 22 }).toString();
const rePict = /\p{Extended_Pictographic}/u, reMark = /^\p{Mark}$/u, reCtl = /^\p{Control}$/u;
let mism = 0;
const kinds = {}, assigned = [];
for (let c = 0; c < 0x110000; c++) {
  const s = String.fromCodePoint(c);
  const want = (rePict.test(s) ? 1 : 0) | (reMark.test(s) ? 2 : 0) | (reCtl.test(s) ? 4 : 0);
  if (props.charCodeAt(c) - 48 !== want) {
    mism++;
    const k = (props.charCodeAt(c) - 48) ^ want;
    kinds[k] = (kinds[k] ?? 0) + 1;
    if (!/\p{Cn}/u.test(s)) assigned.push(c.toString(16));
  }
}
console.log('property mismatches:', mism, 'differing bits', JSON.stringify(kinds), 'assigned code points among them', assigned.length, assigned.slice(0, 20).join(' '));

// Random strings biased towards the code points the segmentation rules care about.
const pools = [
  [0x61, 0x20, 0x4e2d, 0x0d, 0x0a, 0x7],
  [0x301, 0x200d, 0xfe0f, 0xfe0e, 0x1f3fb, 0x20e3],
  [0x1f600, 0x1f468, 0x1f469, 0x1f467, 0x2764, 0x1f1e8, 0x1f1f3, 0x1f1fa, 0x1f1f8, 0x23f1, 0xa9],
  [0x1100, 0x1161, 0x11a8, 0xac00, 0xac01],
  [0x915, 0x94d, 0x937, 0x93f, 0x930, 0x924, 0x94d, 0x930],
  [0xe20, 0xe32, 0xe29, 0xe33, 0xe44, 0x600, 0x110bd],
  [0xd800, 0xdc00, 0xdbff],
];
const seg = new Intl.Segmenter(undefined, { granularity: 'grapheme' });
let seed = 3;
const rnd = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);
const strings = [];
for (let i = 0; i < 20000; i++) {
  const n = 1 + Math.floor(rnd() * 12);
  const cps = [];
  for (let k = 0; k < n; k++) {
    const pool = pools[Math.floor(rnd() * pools.length)];
    cps.push(pool[Math.floor(rnd() * pool.length)]);
  }
  strings.push(cps);
}
// Lone surrogates must stay single units: build the JS string from UTF-16 units directly.
const toStr = (cps) => cps.map((c) => (c >= 0xd800 && c <= 0xdfff ? String.fromCharCode(c) : String.fromCodePoint(c))).join('');
// Adjacent high+low surrogate code points form a pair in a JS string; mirror that in the C input.
const toCps = (s) => { const out = []; for (let i = 0; i < s.length; ) { const c = s.codePointAt(i); out.push(c); i += c > 0xffff ? 2 : 1; } return out; };
const inputs = strings.map((cps) => toCps(toStr(cps)));
const res = execFileSync(exe, [], { input: inputs.map((cps) => cps.map((c) => c.toString(16)).join(' ')).join('\n') + '\n', maxBuffer: 1 << 26 }).toString().replace(/\r/g, '').split('\n');
let segm = 0;
inputs.forEach((cps, i) => {
  const s = toStr(cps);
  const want = Array.from(seg.segment(s), (x) => toCps(x.segment).map((c) => c.toString(16) + ' ').join('')).join('|');
  if (res[i] !== want) {
    if (segm++ < 10) console.log('segmentation mismatch', JSON.stringify(cps.map((c) => c.toString(16))), '\n  utf8proc', res[i], '\n  node    ', want);
  }
});
console.log('segmentation mismatches:', segm, 'of', inputs.length);
