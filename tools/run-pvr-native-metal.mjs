import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';

// Integration smoke test with a finite cadence ELF, not a performance gate.
const [appArg, elfArg, outputArg, ...extra] = process.argv.slice(2);
if (!appArg || !elfArg || !outputArg || extra.length)
  throw Error('Usage: node tools/run-pvr-native-metal.mjs APP CADENCE_ELF NEW_OUTPUT_DIRECTORY');
const app = resolve(appArg), elf = resolve(elfArg), output = resolve(outputArg);
mkdirSync(output);
const hash = path => createHash('sha256').update(readFileSync(path)).digest('hex');
const appHash = hash(app), elfHash = hash(elf);
const env = { ...process.env };
for (const key of Object.keys(env))
  if (key.startsWith('FLYCAST_PVR_') || key.startsWith('FLYCAST_HYBRID_')) delete env[key];
Object.assign(env, { MTL_DEBUG_LAYER: '1', FLYCAST_PVR_NATIVE_METAL: '1',
  FLYCAST_PVR_DECOUPLED_PRESENT: '1', FLYCAST_HYBRID_SOFT: '1',
  FLYCAST_HYBRID_VERIFY: '1', FLYCAST_HYBRID_VERIFY_VRAM: '1' });
let stdout = '', stderr = '', timedOut = false, forced = false, stopping = false;
const result = await new Promise((accept, reject) => {
  const child = spawn(app, ['-config', 'audio:backend=null', '-config', 'config:pvr.rend=7',
    '-config', 'config:rend.ThreadedRendering=yes', '-config', 'config:rend.EmulateFramebuffer=yes',
    '-config', 'config:rend.LinearInterpolation=no', '-config', 'config:rend.Rotate90=no',
    '-config', 'config:rend.vsync=yes', '-config', 'config:Debug.SerialConsoleEnabled=yes',
    '-config', 'config:Debug.SerialPTY=false', elf], { env, stdio: ['ignore', 'pipe', 'pipe'] });
  let grace, finish;
  const stop = () => {
    if (stopping) return;
    stopping = true;
    child.kill('SIGINT');
    grace = setTimeout(() => { forced = true; child.kill('SIGKILL'); }, 5000);
  };
  const timer = setTimeout(() => { timedOut = true; stop(); }, 60000);
  const check = () => {
    if (!finish && (stdout.includes('PVR_CADENCE_END submitted=660') || stderr.includes('PVR_CADENCE_END submitted=660')))
      finish = setTimeout(stop, 2000);
  };
  child.stdout.on('data', data => { stdout += data; check(); });
  child.stderr.on('data', data => { stderr += data; check(); });
  child.on('error', error => { clearTimeout(timer); clearTimeout(grace); clearTimeout(finish); reject(error); });
  child.on('close', (code, signal) => {
    clearTimeout(timer); clearTimeout(grace); clearTimeout(finish);
    accept({ code, signal });
  });
});
writeFileSync(resolve(output, 'stdout.log'), stdout);
writeFileSync(resolve(output, 'stderr.log'), stderr);
const callbacks = [...stderr.matchAll(/^PVR_METAL_PRESENT sequence=(\d+) id=(-?\d+) time=([\d.]+)$/gm)]
  .map(m => ({ sequence: Number(m[1]), id: Number(m[2]), time: Number(m[3]) }));
const displayedIds = new Set(callbacks.filter(x => x.time > 0).map(x => x.id));
const missingIds = Array.from({ length: 660 }, (_, id) => id).filter(id => !displayedIds.has(id));
const combined = stdout + '\n' + stderr;
const comparisons = [...combined.matchAll(/HYBRID verify_vram bytes=8388608 mismatches=0 rtt=0/g)].length;
const mismatch = /HYBRID verify(?:_vram)?[^\n]*mismatches=[1-9]/.test(combined);
const passed = stopping && !timedOut && !forced && (result.code === 0 || result.signal === 'SIGINT')
  && appHash === hash(app) && elfHash === hash(elf)
  && stderr.includes('PVR_METAL_CONTEXT active=1') && comparisons >= 40 && !mismatch
  && callbacks.filter(x => x.time > 0).length >= 40;
const report = { appHash, elfHash, ...result, timedOut, forced, comparisons, callbacks: callbacks.length,
  skippedCallbacks: callbacks.filter(x => x.time === 0).length, passed,
  missingIds, measuredMissingIds: missingIds.filter(id => id >= 60),
  performanceValidated: false, hardwareValidated: false };
writeFileSync(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
console.log(report);
if (!passed) process.exitCode = 1;
