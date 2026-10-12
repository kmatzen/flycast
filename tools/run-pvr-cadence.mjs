import { spawn } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';

const [appArg, elfArg, outputArg, trace] = process.argv.slice(2);
if (process.argv.length !== 5 && !(process.argv.length === 6 && trace === '--stages'))
  throw Error('Usage: node tools/run-pvr-cadence.mjs /path/to/Flycast /path/to/pvr-cadence-probe.elf /new/output/directory [--stages]');
const app = resolve(appArg), elf = resolve(elfArg), output = resolve(outputArg);
mkdirSync(output);
const hash = path => createHash('sha256').update(readFileSync(path)).digest('hex');
const report = { schema: 1, app, elf, stageTracing: trace === '--stages', appHash: hash(app), elfHash: hash(elf), hardwareValidated: false, cases: [] };

for (let run = 0; run < 4; ++run) {
  const vsync = run % 2 === 0;
  const name = `vsync-${vsync ? 'on' : 'off'}-${Math.floor(run / 2) + 1}`;
  if (hash(app) !== report.appHash || hash(elf) !== report.elfHash) throw Error('Input changed during cadence run');
  const env = { ...process.env };
  for (const key of Object.keys(env))
    if (key.startsWith('FLYCAST_PVR_') || key.startsWith('FLYCAST_HYBRID_')) delete env[key];
  Object.assign(env, { FLYCAST_HYBRID_SOFT: '1', FLYCAST_PVR_DECOUPLED_PRESENT: '1',
    FLYCAST_PVR_TRACE_DELIVERY: '1', FLYCAST_PVR_TRACE_CADENCE_IDS: '1',
    FLYCAST_PVR_TRACE_EMULATION: '1', FLYCAST_AUDIO_TRACE_DELIVERY: '1' });
  if (trace === '--stages') env.FLYCAST_PVR_TRACE_STAGES = '1';
  const log = await new Promise((accept, reject) => {
    const child = spawn(app, ['-config', 'audio:backend=sdl2', '-config', 'config:pvr.rend=7',
      '-config', 'config:rend.ThreadedRendering=yes', '-config', 'config:rend.EmulateFramebuffer=yes',
      '-config', `config:rend.vsync=${vsync ? 'yes' : 'no'}`,
      '-config', 'config:Debug.SerialConsoleEnabled=yes', '-config', 'config:Debug.SerialPTY=false', elf],
      { env, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '', stderr = '', stopping = false, timedOut = false, forced = false, grace, drain;
    const stop = () => {
      if (stopping) return;
      stopping = true;
      child.kill('SIGINT');
      grace = setTimeout(() => { forced = true; child.kill('SIGKILL'); }, 5000);
    };
    const timer = setTimeout(() => { timedOut = true; stop(); }, 60000);
    const receive = (stream, data) => {
      if (stream === 'stdout') stdout += data.toString('utf8');
      else stderr += data.toString('utf8');
      const text = stdout + '\n' + stderr;
      if (!drain && text.includes('PVR_CADENCE_END submitted=660') && text.includes('PVR_CADENCE_SWAP id=659 '))
        drain = setTimeout(stop, 300);
    };
    child.stdout.on('data', data => receive('stdout', data));
    child.stderr.on('data', data => receive('stderr', data));
    child.on('error', error => { clearTimeout(timer); clearTimeout(grace); clearTimeout(drain); reject(error); });
    child.on('close', (code, signal) => {
      clearTimeout(timer); clearTimeout(grace); clearTimeout(drain);
      // Serial output can arrive a byte at a time. Never merge chunks from
      // different pipes into protocol lines; their relative order is unknown.
      writeFileSync(resolve(output, `${name}.stdout.log`), stdout, { flag: 'wx' });
      writeFileSync(resolve(output, `${name}.stderr.log`), stderr, { flag: 'wx' });
      if (timedOut || forced || !stopping || (code !== 0 && signal !== 'SIGINT'))
        reject(Error(`${name}: code=${code} signal=${signal} timeout=${timedOut} forced=${forced}`));
      else accept(stdout + '\n' + stderr);
    });
  });
  if (hash(app) !== report.appHash || hash(elf) !== report.elfHash) throw Error('Input changed during cadence run');
  const ids = [...log.matchAll(/^PVR_CADENCE_SWAP id=(\d+) sequence=\d+$/gm)].map(match => Number(match[1]));
  const seen = new Set(ids);
  const missing = Array.from({ length: 660 }, (_, id) => id).filter(id => !seen.has(id));
  const progress = [...log.matchAll(/PVR_CADENCE_PROGRESS submitted=(\d+) vblanks=(\d+)/g)].map(match => [+match[1], +match[2]]);
  const ordered = ids.every((id, index) => id < 660 && (index === 0 || id > ids[index - 1]));
  const valid = log.includes('PVR_CADENCE_PROBE schema=1 execution=unclassified warmup=60 measured=600 width=640 height=480')
    && log.includes('PVR_CADENCE_END submitted=660')
    && log.includes('AUDIO_DELIVERY_FINAL backend=sdl2 ')
    && log.includes('PVR_DELIVERY_FINAL ')
    && ids.length > 0 && ordered && progress.length === 5
    && progress.every(([submitted, vblanks], i) => submitted === (i + 1) * 120 && vblanks === submitted - 1);
  const measuredMissing = missing.filter(id => id >= 60);
  report.cases.push({ name, vsync, valid, observedIds: ids.length, warmupMissing: missing.filter(id => id < 60),
    measuredMissing, losslessMeasured: valid && measuredMissing.length === 0,
    speeds: [...log.matchAll(/PVR_EMULATION[^\n]*speed=([0-9.]+)/g)].map(match => +match[1]),
    audio: log.match(/AUDIO_DELIVERY_FINAL[^\n]*/)?.[0], delivery: log.match(/PVR_DELIVERY_FINAL[^\n]*/)?.[0] });
  writeFileSync(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(`${name}: capture=${valid ? 'PASS' : 'FAIL'} measured_missing=${measuredMissing.length}`);
  if (!valid) throw Error(`Invalid capture: ${name}`);
}
if (report.cases.some(item => !item.losslessMeasured)) process.exitCode = 1;
