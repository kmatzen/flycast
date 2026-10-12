import { spawn } from 'node:child_process';
import { mkdirSync, openSync, closeSync, writeSync, readFileSync, writeFileSync, copyFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { readProbe } from './read-pvr-rtt-probe.mjs';

// Emulator-only regression runner. No output from this tool is a hardware golden.
const [flycastArg, sdkEnvArg, outputArg, modeArg] = process.argv.slice(2);
if (process.argv.length < 5 || process.argv.length > 6 || (modeArg && modeArg !== '--decoupled'))
  throw Error('Usage: node tools/run-pvr-rtt-probes.mjs /path/to/Flycast /path/to/KOS/environ.sh /new/output/directory [--decoupled]');
const decoupled = modeArg === '--decoupled';
const flycast = resolve(flycastArg), sdkEnv = resolve(sdkEnvArg), output = resolve(outputArg);
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const probeDir = resolve(root, 'tests/manual/pvr-rtt-probe');
mkdirSync(output); // Refuse to overwrite an existing run.
const hashFile = path => createHash('sha256').update(readFileSync(path)).digest('hex');
const cases = [0, 1, 2, 3].map(mode => ({ name: `mode-${mode}`, mode, alpha: 255, threshold: 0 }));
for (const alpha of [127, 128, 129])
  cases.push({ name: `alpha-${alpha}-threshold-128`, mode: 3, alpha, threshold: 128 });
for (const k of [1, 127, 128, 255])
  cases.push({ name: `krgb-k-${k}`, mode: 0, alpha: 0, threshold: 128, k });
// K must not substitute for vertex alpha or affect RGB in other formats.
for (const mode of [1, 2, 3])
  cases.push({ name: `mode-${mode}-k-255-alpha-0`, mode, alpha: 0, threshold: 128, k: 255 });
for (const mode of [0, 1, 2, 3])
  cases.push({ name: `mode-${mode}-dither`, mode, alpha: 255, threshold: 0, dither: 1 });
cases.push({ name: 'argb4444-dither-alpha-15', mode: 2, alpha: 15, threshold: 128, dither: 1 });
cases.push({ name: 'argb1555-dither-alpha-127', mode: 3, alpha: 127, threshold: 128, dither: 1 });

async function execute(command, args, env, logPath, stopAtEnd = false) {
  const fd = openSync(logPath, 'wx');
  let child;
  try {
    return await new Promise((accept, reject) => {
      let text = '', complete = false, timedOut = false, escalated = false;
      let graceTimer;
      child = spawn(command, args, { cwd: root, env, stdio: ['ignore', 'pipe', 'pipe'] });
      const stop = () => {
        child.kill('SIGINT');
        graceTimer = setTimeout(() => { escalated = true; child.kill('SIGKILL'); }, 5000);
      };
      const timeout = setTimeout(() => { timedOut = true; stop(); }, 60000);
      const consume = data => {
        writeSync(fd, data);
        text += data.toString('utf8');
        if (stopAtEnd && !complete && /(?:^|\n)PVR_RTT_END\r?\n/.test(text)) {
          complete = true;
          clearTimeout(timeout);
          stop();
        }
      };
      child.stdout.on('data', consume);
      child.stderr.on('data', consume);
      child.on('error', error => {
        clearTimeout(timeout); clearTimeout(graceTimer); reject(error);
      });
      child.on('close', (code, signal) => {
        clearTimeout(timeout); clearTimeout(graceTimer);
        if (timedOut || escalated || (stopAtEnd ? !complete : code !== 0))
          reject(Error(`Command failed: code=${code} signal=${signal} timeout=${timedOut} forcedStop=${escalated}; inspect ${logPath}`));
        else accept(text);
      });
    });
  } finally {
    closeSync(fd);
  }
}

const report = { execution: 'emulator', hardwareValidated: false,
  flycast, flycastSha256: hashFile(flycast), sdkEnv,
  scheduling: decoupled ? 'emulation-thread rendering with decoupled presentation' : 'single-thread',
  probeSourceSha256: hashFile(resolve(probeDir, 'probe.c')), cases: [] };
for (const item of cases) {
  if (hashFile(flycast) !== report.flycastSha256)
    throw Error('Flycast executable changed during the probe run; restart in a new output directory');
  const flags = `-DPVR_PROBE_PACK_MODE=${item.mode} -DPVR_PROBE_ALPHA=${item.alpha} -DPVR_PROBE_ALPHA_THRESHOLD=${item.threshold} -DPVR_PROBE_K=${item.k ?? 0} -DPVR_PROBE_DITHER=${item.dither ?? 0}`;
  await execute('/bin/bash', ['-c', 'source "$1" && make -B -C "$2" "CFLAGS=$3"',
    'pvr-probe-build', sdkEnv, probeDir, flags], process.env, resolve(output, `${item.name}.build.log`));
  const elf = resolve(output, `${item.name}.elf`);
  copyFileSync(resolve(probeDir, 'pvr-rtt-probe.elf'), elf);
  const env = { ...process.env };
  // Do not inherit unrelated renderer experiment switches.
  for (const key of Object.keys(env))
    if (key.startsWith('FLYCAST_HYBRID_') || key.startsWith('FLYCAST_PVR_')) delete env[key];
  Object.assign(env, { FLYCAST_HYBRID_SOFT: '1', FLYCAST_HYBRID_VERIFY: '1', FLYCAST_HYBRID_VERIFY_VRAM: '1' });
  if (decoupled) env.FLYCAST_PVR_DECOUPLED_PRESENT = '1';
  const args = ['-config', 'audio:backend=null', '-config', 'config:pvr.rend=7',
    '-config', `config:rend.ThreadedRendering=${decoupled ? 'yes' : 'no'}`, '-config', 'config:rend.EmulateFramebuffer=yes',
    '-config', 'config:rend.RenderToTextureBuffer=yes', '-config', 'config:Debug.SerialConsoleEnabled=yes',
    '-config', 'config:Debug.SerialPTY=false', elf];
  const log = await execute(flycast, args, env, resolve(output, `${item.name}.run.log`), true);
  if (hashFile(flycast) !== report.flycastSha256)
    throw Error('Flycast executable changed during the probe run; restart in a new output directory');
  const observed = readProbe(log);
  const vramVerified = /HYBRID verify_vram bytes=8388608 mismatches=0 rtt=1(?:\r?\n)/.test(log);
  const colorVerified = /HYBRID verify pixels=307200 mismatches=0(?:\r?\n)/.test(log);
  const nonzeroMismatch = /HYBRID verify(?:_vram)?[^\n]*mismatches=[1-9]/.test(log);
  // Dither has no independent expected interior here. Do not report a
  // documented-packing pass merely because the two implementations agree.
  const interiorExpectedColorVerified = item.dither ? null
    : observed.interiorPixelsDifferentFromRequestedColor === 0;
  const interiorWritten = observed.interiorPhaseHistograms.every(phase => !('a5a5' in phase));
  const passed = observed.packmode === item.mode && observed.alpha === item.alpha
    && observed.threshold === item.threshold && observed.k === (item.k ?? 0) && observed.expectedConfiguration
    && observed.dither === (item.dither ?? 0)
    && observed.prefixGuardChangedBytes === 0 && observed.suffixGuardChangedBytes === 0
    && observed.paddingChangedBytes === 0 && interiorWritten
    && (item.dither ? observed.interiorPixelsDifferentFromRequestedColor === null : interiorExpectedColorVerified)
    && vramVerified && colorVerified && !nonzeroMismatch;
  report.cases.push({ ...item, elfSha256: hashFile(elf), passed, interiorExpectedColorVerified,
    acceptanceScope: item.dither ? 'capture integrity and software-Metal agreement only' : 'documented undithered packing and software-Metal agreement',
    vramVerified, colorVerified, observed });
  writeFileSync(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(`${item.name}: ${passed ? 'PASS' : 'FAIL'}${item.dither ? ' (differential only; dither correctness unverified)' : ''}`);
  if (!passed) throw Error(`Probe regression: inspect ${output}`);
}
console.log(`Completed ${report.cases.length} emulator cases. Hardware validation remains false. Report: ${output}/report.json`);
