import { test } from 'node:test';
import assert from 'node:assert/strict';
import { analyzeStages } from './read-pvr-stages.mjs';

const line = (stage, sequence, id, time) => `PVR_STAGE stage=${stage} sequence=${sequence} id=${id} time_us=${time}\n`;
test('uses timestamps, not inter-thread log ordering', () => {
  const result = analyzeStages(line('swap', 1, 60, 150) + line('publish', 1, 60, 100)
    + line('select', 1, 60, 120));
  assert.equal(result.publishToSelect.p50_us, 20);
  assert.equal(result.selectToSwap.p95_us, 30);
  assert.equal(result.measuredMissing.length, 599);
});
test('distinguishes absent, unselected, and selected but unswapped IDs', () => {
  const result = analyzeStages(line('publish', 1, 60, 100) + line('publish', 2, 61, 200)
    + line('select', 2, 61, 210));
  assert.deepEqual(result.measuredMissing.slice(0, 3), [
    {id: 60, published: 1, selected: 0}, {id: 61, published: 1, selected: 1},
    {id: 62, published: 0, selected: 0}]);
});
test('rejects damaged or inconsistent traces', () => {
  for (const text of ['', 'PVR_STAGE broken', line('swap', 1, 60, 100),
    line('publish', 1, 60, 100) + line('select', 1, 60, 90),
    line('publish', 1, 60, 100) + line('select', 1, 61, 110),
    line('publish', 1, 60, 100).repeat(2)])
    assert.throws(() => analyzeStages(text));
});
