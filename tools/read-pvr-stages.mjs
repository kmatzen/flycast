import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

// Timestamps use one process-wide steady clock. Log-line ordering across
// producer/UI threads is deliberately not used as a timing source.
export function analyzeStages(text) {
const frames = new Map();
for (const line of text.split(/\r?\n/)) {
  if (!line.includes('PVR_STAGE')) continue;
  const match = line.match(/^PVR_STAGE stage=(publish|select|swap) sequence=(\d+) id=(-?\d+) time_us=(\d+)$/);
  if (!match) throw Error('Malformed stage record');
  const [, stage, sequence, id, time] = match;
  if (![sequence, id, time].every(value => Number.isSafeInteger(+value))) throw Error('Unsafe integer');
  const frame = frames.get(sequence) ?? { sequence: +sequence, id: +id };
  if (frame[stage] !== undefined || frame.id !== +id) throw Error(`Duplicate or inconsistent stage for ${sequence}`);
  frame[stage] = +time;
  frames.set(sequence, frame);
}
if (!frames.size) throw Error('No stage records');
const selected = [], swapped = [], missing = [];
for (const frame of frames.values()) {
  if (frame.publish === undefined || (frame.select !== undefined && frame.select < frame.publish)
      || (frame.swap !== undefined && (frame.select === undefined || frame.swap < frame.select)))
    throw Error(`Invalid timing chain for ${frame.sequence}`);
  if (frame.id < 60 || frame.id >= 660) continue;
  if (frame.select !== undefined) selected.push(frame.select - frame.publish);
  if (frame.swap !== undefined) swapped.push(frame.swap - frame.select);
}
for (let id = 60; id < 660; ++id) {
  const matches = [...frames.values()].filter(frame => frame.id === id);
  if (!matches.some(frame => frame.swap !== undefined))
    missing.push({ id, published: matches.length, selected: matches.filter(frame => frame.select !== undefined).length });
}
const stats = values => {
  values.sort((a, b) => a - b);
  return values.length ? { count: values.length, p50_us: values[Math.floor((values.length - 1) * .5)],
    p95_us: values[Math.floor((values.length - 1) * .95)], max_us: values.at(-1) } : null;
};
return { measuredMissing: missing, publishToSelect: stats(selected),
  selectToSwap: stats(swapped), physicalScanoutMeasured: false };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const path = process.argv[2];
  if (!path || process.argv.length !== 3) throw Error('Usage: node tools/read-pvr-stages.mjs capture.stderr.log');
  console.log(JSON.stringify({ path, ...analyzeStages(readFileSync(path, 'utf8')) }, null, 2));
}
