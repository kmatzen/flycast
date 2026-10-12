import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';
import { analyzeStages } from './read-pvr-stages.mjs';

// Integration smoke test with a finite cadence ELF, not a performance gate.
const [appArg, elfArg, outputArg, ...options] = process.argv.slice(2);
if (!appArg || !elfArg || !outputArg || new Set(options).size !== options.length
    || options.some(x => !['--timing', '--stages', '--ordered'].includes(x)))
  throw Error('Usage: node tools/run-pvr-native-metal.mjs APP CADENCE_ELF NEW_OUTPUT_DIRECTORY [--timing] [--stages] [--ordered]');
const timing = options.includes('--timing'), stages = options.includes('--stages');
const ordered = options.includes('--ordered');
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
if (timing) {
  env.MTL_DEBUG_LAYER = '0';
  delete env.FLYCAST_HYBRID_VERIFY;
  delete env.FLYCAST_HYBRID_VERIFY_VRAM;
  env.FLYCAST_AUDIO_TRACE_DELIVERY = '1';
}
if (stages) env.FLYCAST_PVR_TRACE_STAGES = '1';
if (ordered) env.FLYCAST_PVR_ORDERED_PRESENT = '1';
let stdout = '', stderr = '', timedOut = false, forced = false, stopping = false;
const result = await new Promise((accept, reject) => {
  const child = spawn(app, ['-config', `audio:backend=${timing ? 'sdl2' : 'null'}`, '-config', 'config:pvr.rend=7',
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
const integrationPassed = stopping && !timedOut && !forced && (result.code === 0 || result.signal === 'SIGINT')
  && appHash === hash(app) && elfHash === hash(elf)
  && stderr.includes('PVR_METAL_CONTEXT active=1') && (timing ? comparisons === 0 : comparisons >= 40) && !mismatch
  && (!timing || /AUDIO_DELIVERY_FINAL backend=sdl2/.test(stderr))
  && (!ordered || /PVR_ORDERED_FINAL dropped=\d+/.test(stderr))
  && callbacks.filter(x => x.time > 0).length >= 40;
const first = new Map();
for (const record of callbacks)
  if (record.time > 0 && record.id >= 60 && record.id < 660 && !first.has(record.id)) first.set(record.id, record);
const records = [...first.values()].sort((a, b) => a.time - b.time);
const gaps = records.slice(1).map((x, i) => ({ from: records[i].id, to: x.id, ms: (x.time - records[i].time) * 1000 }));
const sorted = gaps.map(x => x.ms).sort((a, b) => a - b);
let stageReport;
if (stages) {
  // Validate producer/UI records using their common steady clock. Native
  // presentedTime has a different time domain: do not subtract across them.
  const validated = analyzeStages(stderr);
  const records = [...stderr.matchAll(/^PVR_STAGE stage=(publish|select) sequence=(\d+) id=(-?\d+) time_us=(\d+)$/gm)]
    .map(m => ({ stage: m[1], sequence: +m[2], id: +m[3], time: +m[4] }));
  stageReport = { publishToSelect: validated.publishToSelect,
    missing: missingIds.map(id => ({ id,
      publications: records.filter(x => x.stage === 'publish' && x.id === id),
      selections: records.filter(x => x.stage === 'select' && x.id === id),
      callbacks: callbacks.filter(x => x.id === id) })) };
}
const report = { appHash, elfHash, timing, stages, ordered, stageReport, ...result, timedOut, forced, comparisons, callbacks: callbacks.length,
  orderedFinal: stderr.match(/PVR_ORDERED_FINAL[^\n]*/)?.[0],
  skippedCallbacks: callbacks.filter(x => x.time === 0).length, integrationPassed,
  missingIds, measuredMissingIds: missingIds.filter(id => id >= 60),
  measuredFirstPresentationGapMs: { p50: sorted[Math.floor(sorted.length * .5)], p95: sorted[Math.floor(sorted.length * .95)], max: sorted.at(-1) },
  longGaps: gaps.filter(x => x.ms > 20),
  audio: stderr.match(/AUDIO_DELIVERY_FINAL[^\n]*/)?.[0],
  performanceValidated: false, hardwareValidated: false };
writeFileSync(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
console.log(report);
if (!integrationPassed) process.exitCode = 1;
