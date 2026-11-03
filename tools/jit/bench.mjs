// Runs bench.lua several times and prints the median of each benchmark with
// the JIT on and off: node bench.mjs [runs] [--jit-only]
import { execFileSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const runs = Number(args.find((a) => /^\d+$/.test(a)) || 5);
const jitOnly = args.includes('--jit-only');

function measure(flags) {
  const results = {};
  for (let r = 0; r < runs; r++) {
    const out = execFileSync('node', [join(here, 'run.mjs'), ...flags, join(here, 'bench.lua')], { encoding: 'utf8' });
    for (const line of out.split('\n')) {
      const m = line.match(/^(.*?)\s+([\d.]+) ms$/);
      if (m) (results[m[1]] ||= []).push(Number(m[2]));
    }
  }
  const median = (xs) => xs.sort((a, b) => a - b)[xs.length >> 1];
  return Object.fromEntries(Object.entries(results).map(([k, v]) => [k, median(v)]));
}

const jit = measure(['--threshold', '50']);  // the engine's threshold
const interp = jitOnly ? null : measure(['--nojit']);
console.log(`${'benchmark'.padEnd(24)} ${'jit ms'.padStart(8)}${interp ? ` ${'interp ms'.padStart(10)} ${'speedup'.padStart(8)}` : ''}`);
for (const [name, t] of Object.entries(jit)) {
  let line = `${name.padEnd(24)} ${t.toFixed(1).padStart(8)}`;
  if (interp) line += ` ${interp[name].toFixed(1).padStart(10)} ${(interp[name] / t).toFixed(1).padStart(7)}x`;
  console.log(line);
}
