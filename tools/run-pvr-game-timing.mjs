import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';

const [appArg, gameArg, outputArg, mode, ...extra] = process.argv.slice(2);
if (!appArg || !gameArg || !outputArg || extra.length || (mode && mode !== '--reverse'))
  throw Error('Usage: node tools/run-pvr-game-timing.mjs APP GAME NEW_OUTPUT_DIRECTORY [--reverse]');
const app = resolve(appArg), game = resolve(gameArg), output = resolve(outputArg);
mkdirSync(output);
const hash = () => createHash('sha256').update(readFileSync(app)).digest('hex');
const appHash = hash();
const report = { appHash, game, durationMs: 45000, audioBufferFrames: 2822, cases: [], hardwareValidated: false,
  scope: 'Unattended boot/idle samples, not an interactive playthrough or audible-quality assessment' };
for (const native of (mode === '--reverse' ? [false, true] : [true, false])) {
  if (hash() !== appHash) throw Error('Application changed');
  const name = native ? 'native' : 'opengl';
  const env = { ...process.env, MTL_DEBUG_LAYER: '0' };
  for (const key of Object.keys(env))
    if (key.startsWith('FLYCAST_PVR_') || key.startsWith('FLYCAST_HYBRID_')) delete env[key];
  Object.assign(env, { FLYCAST_HYBRID_SOFT: '1', FLYCAST_PVR_DECOUPLED_PRESENT: '1',
    FLYCAST_PVR_TRACE_EMULATION: '1', FLYCAST_AUDIO_TRACE_DELIVERY: '1' });
  if (native) env.FLYCAST_PVR_NATIVE_METAL = '1';
  let stdout = '', stderr = '', stopping = false, forced = false;
  const result = await new Promise((accept, reject) => {
    const child = spawn(app, ['-config', 'audio:backend=sdl2', '-config', 'config:pvr.rend=7',
      '-config', `config:aica.BufferSize=${report.audioBufferFrames}`,
      '-config', 'config:rend.ThreadedRendering=yes', '-config', 'config:rend.EmulateFramebuffer=yes',
      '-config', 'config:rend.LinearInterpolation=no', '-config', 'config:rend.Rotate90=no',
      '-config', 'config:rend.vsync=yes', game], { env, stdio: ['ignore', 'pipe', 'pipe'] });
    let grace;
    const timer = setTimeout(() => {
      stopping = true;
      child.kill('SIGINT');
      grace = setTimeout(() => { forced = true; child.kill('SIGKILL'); }, 5000);
    }, report.durationMs);
    child.stdout.on('data', data => { stdout += data; });
    child.stderr.on('data', data => { stderr += data; });
    child.on('error', error => { clearTimeout(timer); clearTimeout(grace); reject(error); });
    child.on('close', (code, signal) => {
      clearTimeout(timer); clearTimeout(grace); accept({ code, signal });
    });
  });
  writeFileSync(resolve(output, `${name}.stdout.log`), stdout);
  writeFileSync(resolve(output, `${name}.stderr.log`), stderr);
  const speeds = [...stderr.matchAll(/PVR_EMULATION[^\n]*speed=([0-9.]+)/g)].map(m => +m[1]);
  const audio = stderr.match(/AUDIO_DELIVERY_FINAL[^\n]*/)?.[0];
  const nativeCallbacks = [...stderr.matchAll(/^PVR_METAL_PRESENT sequence=(\d+) id=(-?\d+) time=([\d.]+)$/gm)];
  const integrationPassed = stopping && !forced && (result.code === 0 || result.signal === 'SIGINT')
    && hash() === appHash && speeds.length > 0 && !!audio
    && (!native || (stderr.includes('PVR_METAL_CONTEXT active=1') && nativeCallbacks.length > 0));
  const capture = { name, ...result, forced, integrationPassed, speeds, audio,
    lastPeriodicAudio: [...stderr.matchAll(/^AUDIO_DELIVERY [^\n]*/gm)].at(-1)?.[0],
    nativeCallbacks: nativeCallbacks.length, skippedCallbacks: nativeCallbacks.filter(m => +m[3] === 0).length };
  report.cases.push(capture);
  writeFileSync(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(capture);
  if (!integrationPassed) { process.exitCode = 1; break; }
}
