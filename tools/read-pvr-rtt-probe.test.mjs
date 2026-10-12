import test from 'node:test';
import assert from 'node:assert/strict';
import { readProbe } from './read-pvr-rtt-probe.mjs';

function fixture() {
  const lines = [
    'PVR_RTT_PROBE schema=1 execution=unclassified width=32 height=32 stride=36 guard=32 bytes=2368',
    'PVR_RTT_REG pack=00000001 base=01010000 stride=00000009 clipx=001f0000 clipy=001f0000',
  ];
  for (let offset = 0; offset < 2368; offset += 32)
    lines.push(`PVR_RTT_BYTES offset=${offset.toString(16).padStart(4, '0')} data=${'a5'.repeat(32)}`);
  lines.push('PVR_RTT_END');
  return lines;
}
test('complete data remains unclassified, even with expected registers', () => {
  const result = readProbe(fixture().join('\n'));
  assert.equal(result.hardwareValidated, false);
  assert.equal(result.expectedConfiguration, true);
  assert.equal(result.centerWord, 0xa5a5); // No rendered output was fabricated.
  assert.equal(result.prefixGuardChangedBytes, 0);
  assert.equal(result.suffixGuardChangedBytes, 0);
  assert.equal(result.paddingChangedBytes, 0);
  assert.deepEqual(result.pixelWordHistogram, { a5a5: 1024 });
  assert.equal(result.interiorPixelsDifferentFromRequestedColor, 256);
});
test('report changed padding and interior independently of provenance', () => {
  const lines = fixture();
  const bytes = Buffer.alloc(2368, 0xa5);
  for (let y = 8; y < 24; ++y)
    for (let x = 8; x < 24; ++x)
      bytes.writeUInt16LE(0xae7d, 32 + (y * 36 + x) * 2);
  bytes[32 + 64] = 0;
  bytes[0] = 0;
  bytes[2367] = 0;
  for (let offset = 0; offset < bytes.length; offset += 32)
    lines[2 + offset / 32] = `PVR_RTT_BYTES offset=${offset.toString(16).padStart(4, '0')} data=${bytes.subarray(offset, offset + 32).toString('hex')}`;
  const result = readProbe(lines.join('\n'));
  assert.equal(result.interiorPixelsDifferentFromRequestedColor, 0);
  assert.equal(result.paddingChangedBytes, 1);
  assert.equal(result.prefixGuardChangedBytes, 1);
  assert.equal(result.suffixGuardChangedBytes, 1);
  assert.equal(result.hardwareValidated, false);
});
test('reject incomplete, reordered, duplicate, and malformed evidence', () => {
  const lines = fixture();
  assert.throws(() => readProbe(lines.slice(0, -1).join('\n')));
  assert.throws(() => readProbe(lines.filter((_, i) => i !== 3).join('\n')));
  assert.throws(() => readProbe([...lines, ...lines].join('\n')));
  assert.throws(() => readProbe(lines.join('\n').replace('offset=0020', 'offset=0000')));
  assert.throws(() => readProbe(lines.join('\n').replace('data=a5', 'data=zz')));
  assert.throws(() => readProbe(lines.join('\n').replace('execution=unclassified', 'execution=hardware')));
});
