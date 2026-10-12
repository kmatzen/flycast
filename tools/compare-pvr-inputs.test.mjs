import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { compareInputs, numericFields, configFields } from './compare-pvr-inputs.mjs';

test('capture comparison rejects mismatches and incomplete evidence', () => {
  const directory = mkdtempSync(join(tmpdir(), 'pvr-input-test-'));
  try {
    const left = join(directory, 'left'), right = join(directory, 'right');
    const state = { schema: 1, kind: 'latched-pvr-state',
      ...Object.fromEntries(numericFields.map(key => [key, 0])),
      fog_table: Array(128).fill(0), region_array: [Array(6).fill(0)] };
    for (const prefix of [left, right]) {
      writeFileSync(prefix + '.processed.vram', Buffer.alloc(0x800000));
      writeFileSync(prefix + '.processed.registers', Buffer.alloc(0x8000));
      writeFileSync(prefix + '.processed.context.json', JSON.stringify(state));
      writeFileSync(prefix + '.input.context-0.ta', Buffer.alloc(32));
    }
    assert.equal(compareInputs(left, right).capturedInputsMatch, true);
	state.schema = 2;
	writeFileSync(right + '.processed.context.json', JSON.stringify(state));
	assert.throws(() => compareInputs(left, right), /schema/);
	Object.assign(state, Object.fromEntries(configFields.map(key => [key, 0])));
	for (const prefix of [left, right])
	  writeFileSync(prefix + '.processed.context.json', JSON.stringify(state));
	assert.equal(compareInputs(left, right).configurationCaptured, true);
	state.config_TextureFiltering = 1;
	writeFileSync(right + '.processed.context.json', JSON.stringify(state));
	assert.deepEqual(compareInputs(left, right).differingContextFields, ['config_TextureFiltering']);
	state.config_TextureFiltering = 0;
	writeFileSync(right + '.processed.context.json', JSON.stringify(state));
    writeFileSync(right + '.input.context-0.ta', Buffer.alloc(32, 1));
    assert.equal(compareInputs(left, right).capturedInputsMatch, false);
    state.half_offset = 7;
    writeFileSync(right + '.processed.context.json', JSON.stringify(state));
    assert.deepEqual(compareInputs(left, right).differingContextFields, ['half_offset']);
    delete state.half_offset;
    writeFileSync(right + '.processed.context.json', JSON.stringify(state));
    assert.throws(() => compareInputs(left, right), /schema/);
    state.half_offset = 0;
    writeFileSync(right + '.processed.context.json', JSON.stringify(state));
    writeFileSync(right + '.input.context-2.ta', Buffer.alloc(32));
    assert.throws(() => compareInputs(left, right), /contiguous/);
    rmSync(right + '.input.context-2.ta');
    writeFileSync(right + '.processed.registers', Buffer.alloc(31));
    assert.throws(() => compareInputs(left, right), /expected 32768/);
  } finally {
    rmSync(directory, { recursive: true });
  }
});
