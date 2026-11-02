// Runs a Lua script on the Node build: node run.mjs [--nojit] [--threshold N] script.lua [args]
import createLua from '../../build/jit-node/lua.mjs';

const args = process.argv.slice(2);
let enabled = 1, threshold = 1, stats = false;
while (args[0]?.startsWith('--')) {
  const flag = args.shift();
  if (flag === '--nojit') enabled = 0;
  else if (flag === '--threshold') threshold = Number(args.shift());
  else if (flag === '--stats') stats = true;
}
const lua = await createLua({ noInitialRun: true });
lua._ljit_configure(enabled, threshold);
try {
  lua.callMain(args);
} catch (e) {
  if (e?.name !== 'ExitStatus') throw e;
}
if (stats) console.error(`[ljit] compiled ${lua._ljit_stats(0)}, not compiled ${lua._ljit_stats(1)}`);
