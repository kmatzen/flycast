import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';

// Integration regression, not a performance test or hardware reference.
const [appArg, gameArg, outputArg, modeArg] = process.argv.slice(2);
if (process.argv.length < 5 || process.argv.length > 6 || (modeArg && !['--paused', '--error'].includes(modeArg)))
  throw Error('Usage: node tools/run-pvr-lifecycle.mjs /path/to/Flycast /path/to/game.cue /new/output/directory [--paused|--error]');
const paused = modeArg === '--paused';
const injectError = modeArg === '--error';
const app = resolve(appArg), game = resolve(gameArg), output = resolve(outputArg);
mkdirSync(output);
const hash = () => createHash('sha256').update(readFileSync(app)).digest('hex');
const executableHash = hash();
const report = { schema: 1, app, game, executableHash, paused, injectError, hardwareValidated: false, cases: [] };
const successMarker = injectError ? 'PVR_LIFECYCLE renderer_error_recovered running=0 decoupled=1'
  : 'PVR_LIFECYCLE reinit_complete running=1 decoupled=1';

for (const frame of (injectError ? [64] : [480, 720])) {
  if (hash() !== executableHash) throw Error('Executable changed during lifecycle regression');
  const env = { ...process.env };
  for (const key of Object.keys(env))
    if (key.startsWith('FLYCAST_HYBRID_') || key.startsWith('FLYCAST_PVR_')) delete env[key];
  Object.assign(env, {
    FLYCAST_HYBRID_SOFT: '1', FLYCAST_HYBRID_VERIFY: '1', FLYCAST_HYBRID_VERIFY_VRAM: '1',
    FLYCAST_PVR_DECOUPLED_PRESENT: '1', FLYCAST_PVR_TRACE_DELIVERY: '1',
    FLYCAST_PVR_TEST_REINIT_FRAME: String(frame),
  });
  if (paused) env.FLYCAST_PVR_TEST_PAUSED_REINIT = '1';
  if (injectError) {
    delete env.FLYCAST_PVR_TEST_REINIT_FRAME;
    env.FLYCAST_PVR_TEST_RENDER_ERROR = '1';
  }
  const log = await new Promise((accept, reject) => {
    const child = spawn(app, ['-config', 'audio:backend=null', '-config', 'config:pvr.rend=7',
      '-config', 'config:rend.ThreadedRendering=yes', '-config', 'config:rend.EmulateFramebuffer=yes',
      '-config', 'config:rend.vsync=yes', game], { env, stdio: ['ignore', 'pipe', 'pipe'] });
    let text = '', stopping = false, timedOut = false, forced = false, grace, faultStop;
    const stop = () => {
      if (stopping) return;
      stopping = true;
      child.kill('SIGINT');
      grace = setTimeout(() => { forced = true; child.kill('SIGKILL'); }, 5000);
    };
    const timer = setTimeout(() => { timedOut = true; stop(); }, 150000);
    const receive = data => {
      text += data.toString('utf8');
      const marker = text.indexOf(successMarker);
      if (injectError && marker >= 0 && !faultStop)
        faultStop = setTimeout(stop, 500);
      if (!injectError && marker >= 0 && [...text.slice(marker).matchAll(/HYBRID verify_vram bytes=8388608 mismatches=0 rtt=0/g)].length >= 40)
        stop();
    };
    child.stdout.on('data', receive);
    child.stderr.on('data', receive);
    child.on('error', error => {
      clearTimeout(timer); clearTimeout(grace); clearTimeout(faultStop); reject(error);
    });
    child.on('close', (code, signal) => {
      clearTimeout(timer); clearTimeout(grace); clearTimeout(faultStop);
      writeFileSync(resolve(output, `frame-${frame}.log`), text, { flag: 'wx' });
      if (timedOut || forced || !stopping || (code !== 0 && signal !== 'SIGINT'))
        reject(Error(`Lifecycle run failed: frame=${frame} code=${code} signal=${signal} timeout=${timedOut} forced=${forced}`));
      else accept(text);
    });
  });
  const marker = log.indexOf(successMarker);
  const before = marker >= 0 ? log.slice(0, marker) : '';
  const after = marker >= 0 ? log.slice(marker) : '';
  const count = text => [...text.matchAll(/HYBRID verify_vram bytes=8388608 mismatches=0 rtt=0/g)].length;
  const finalReports = [...log.matchAll(/PVR_DELIVERY_FINAL[^\n]*/g)].map(match => match[0]);
  const passed = hash() === executableHash && marker >= 0 && count(before) > 0
    && (injectError ? count(after) === 0 && log.includes('PVR_LIFECYCLE injected_render_error processed=64')
      : count(after) >= 40 && log.includes(`PVR_LIFECYCLE reinit_begin frame=${frame} running=1`))
    && finalReports.length === 2
    && (!injectError || /^PVR_DELIVERY_FINAL published=0 swapped_unique=0 /.test(finalReports[1]))
    && (!paused || log.includes('PVR_LIFECYCLE paused_reinit running=0 retained=1 drawn=1'))
    && !/HYBRID verify(?:_vram)?[^\n]*mismatches=[1-9]/.test(log)
    && /HYBRID verify pixels=307200 mismatches=0/.test(injectError ? before : after);
  report.cases.push({ frame, passed, beforeVramComparisons: count(before), afterVramComparisons: count(after), finalReports });
  writeFileSync(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(`${injectError ? 'Injected renderer failure after processed frame' : 'Renderer recreation at UI frame'} ${frame}: ${passed ? 'PASS' : 'FAIL'}`);
  if (!passed) throw Error(`Inspect ${output}/frame-${frame}.log`);
}
