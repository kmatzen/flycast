import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { createHash } from 'node:crypto';

const acknowledged = process.argv[4] === '--acknowledged';
if (process.argv.length !== 4 && !(process.argv.length === 5 && acknowledged))
  throw Error('Usage: node tools/run-metal-window-probe.mjs /path/to/probe /new/output/directory [--acknowledged]');
const app = resolve(process.argv[2]), output = resolve(process.argv[3]);
mkdirSync(output);
const hash = () => createHash('sha256').update(readFileSync(app)).digest('hex');
const report = { app, hash: hash(), hardwareValidated: false, cases: [] };
for (let run = 0; run < 4; ++run) {
  const paced = run % 2 === 1;
  const name = `${paced ? acknowledged ? 'acknowledged' : 'paced' : 'immediate'}-${Math.floor(run / 2) + 1}`;
  if (hash() !== report.hash) throw Error('Binary changed');
  const result = spawnSync(app, paced ? [acknowledged ? '--acknowledged' : '--paced'] : [], {
    env: { ...process.env, MTL_DEBUG_LAYER: '1' }, encoding: 'utf8', timeout: 30000,
    killSignal: 'SIGKILL', maxBuffer: 8 * 1024 * 1024 });
  writeFileSync(resolve(output, `${name}.stdout.log`), result.stdout ?? '');
  writeFileSync(resolve(output, `${name}.stderr.log`), result.stderr ?? '');
  if (hash() !== report.hash || result.error || result.signal || ![0, 5].includes(result.status))
    throw Error(`${name}: capture failed status=${result.status} signal=${result.signal} error=${result.error}`);
  const records = [...result.stderr.matchAll(/^METAL_WINDOW_PRESENT id=(\d+) time=([0-9.]+)$/gm)]
    .map(match => ({ id: +match[1], time: +match[2] })).sort((a, b) => a.id - b.id);
  const retryMode = paced && acknowledged;
  if ((!retryMode && records.length !== 120) || records.some(record => record.id < 0 || record.id >= 120 || !Number.isFinite(record.time)))
    throw Error(`${name}: invalid callback sequence`);
  const skipped = records.filter(record => record.time <= 0).map(record => record.id);
  const shown = records.filter(record => record.time > 0);
  const missing = Array.from({length: 120}, (_, id) => id).filter(id => !shown.some(record => record.id === id));
  if (new Set(shown.map(record => record.id)).size !== shown.length
      || (!retryMode && new Set(records.map(record => record.id)).size !== 120))
    throw Error(`${name}: duplicate delivery`);
  const gaps = shown.slice(1).map((record, index) => (record.time - shown[index].time) * 1000);
  if (gaps.some(gap => gap <= 0)) throw Error(`${name}: presentation times out of order`);
  const final = result.stderr.match(/METAL_WINDOW_END submitted=(\d+) callbacks=(\d+) skipped=(\d+) failed=0/);
  if (!final || +final[1] !== records.length || +final[2] !== records.length || +final[3] !== skipped.length
      || result.status !== (missing.length ? 5 : 0))
    throw Error(`${name}: inconsistent completion`);
  gaps.sort((a, b) => a - b);
  report.cases.push({ name, skippedAttempts: skipped, missing, lossless: missing.length === 0,
    // Intervals between non-skipped callbacks; missing IDs are never erased.
    displayedGapMs: gaps.length ? { p50: gaps[Math.floor((gaps.length - 1) * .5)],
      p95: gaps[Math.floor((gaps.length - 1) * .95)], max: gaps.at(-1) } : null });
  writeFileSync(resolve(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(`${name}: skipped=${skipped.length}`);
}
if (report.cases.some(run => !run.lossless)) process.exitCode = 1;
