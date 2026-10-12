import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';

if (process.argv.length !== 4) throw Error('Usage: node tools/run-metal-window-probe.mjs /path/to/probe /new/output/directory');
const app = resolve(process.argv[2]), output = resolve(process.argv[3]);
mkdirSync(output);
const hash = () => createHash('sha256').update(readFileSync(app)).digest('hex');
const report = { app, hash: hash(), hardwareValidated: false, cases: [] };
for (let run = 0; run < 4; ++run) {
  const paced = run % 2 === 1;
  const name = `${paced ? 'paced' : 'immediate'}-${Math.floor(run / 2) + 1}`;
  if (hash() !== report.hash) throw Error('Binary changed');
  const result = spawnSync(app, paced ? ['--paced'] : [], {
    env: { ...process.env, MTL_DEBUG_LAYER: '1' }, encoding: 'utf8', timeout: 30000,
    killSignal: 'SIGKILL', maxBuffer: 8 * 1024 * 1024 });
  writeFileSync(resolve(output, `${name}.stdout.log`), result.stdout ?? '');
  writeFileSync(resolve(output, `${name}.stderr.log`), result.stderr ?? '');
  if (hash() !== report.hash || result.error || result.signal || ![0, 5].includes(result.status))
    throw Error(`${name}: capture failed status=${result.status} signal=${result.signal} error=${result.error}`);
  const records = [...result.stderr.matchAll(/^METAL_WINDOW_PRESENT id=(\d+) time=([0-9.]+)$/gm)]
    .map(match => ({ id: +match[1], time: +match[2] })).sort((a, b) => a.id - b.id);
  if (records.length !== 120 || records.some((record, index) => record.id !== index || !Number.isFinite(record.time)))
    throw Error(`${name}: invalid callback sequence`);
  const skipped = records.filter(record => record.time <= 0).map(record => record.id);
  const shown = records.filter(record => record.time > 0);
  const gaps = shown.slice(1).map((record, index) => (record.time - shown[index].time) * 1000);
  if (gaps.some(gap => gap <= 0)) throw Error(`${name}: presentation times out of order`);
  const final = result.stderr.match(/METAL_WINDOW_END submitted=120 callbacks=120 skipped=(\d+) failed=0/);
  if (!final || +final[1] !== skipped.length || result.status !== (skipped.length ? 5 : 0))
    throw Error(`${name}: inconsistent completion`);
  gaps.sort((a, b) => a - b);
  report.cases.push({ name, skipped, lossless: skipped.length === 0,
    // Intervals between non-skipped callbacks; missing IDs are never erased.
    displayedGapMs: gaps.length ? { p50: gaps[Math.floor((gaps.length - 1) * .5)],
      p95: gaps[Math.floor((gaps.length - 1) * .95)], max: gaps.at(-1) } : null });
  writeFileSync(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(`${name}: skipped=${skipped.length}`);
}
if (report.cases.some(run => !run.lossless)) process.exitCode = 1;
