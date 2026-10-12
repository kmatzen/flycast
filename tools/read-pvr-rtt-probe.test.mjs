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

test('schema 2 identifies each supported format without promoting provenance', () => {
  for (const [mode, expected] of [0x573d, 0xae7d, 0xface, 0xd73d].entries()) {
    const lines = fixture();
    lines[0] = lines[0].replace('schema=1', 'schema=2') + ` packmode=${mode}`;
    lines[1] = lines[1].replace('pack=00000001', `pack=0000000${mode}`);
    const result = readProbe(lines.join('\n'));
    assert.equal(result.packmode, mode);
    assert.equal(result.expectedInteriorWord, expected);
    assert.equal(result.expectedConfiguration, true);
    assert.equal(result.hardwareValidated, false);
    assert.equal(result.interiorPixelsDifferentFromRequestedColor, 256);
    assert.throws(() => readProbe(lines.join('\n').replace(`packmode=${mode}`, 'packmode=4')));
  }
});

test('schema 3 distinguishes threshold boundaries and rejects malformed alpha', () => {
  for (const [alpha, expected] of [['7f', 0x573d], ['80', 0xd73d], ['81', 0xd73d]]) {
    const lines = fixture();
    lines[0] = lines[0].replace('schema=1', 'schema=3') + ` packmode=3 alpha=${alpha} threshold=80`;
    lines[1] = lines[1].replace('pack=00000001', 'pack=00800003');
    const result = readProbe(lines.join('\n'));
    assert.equal(result.expectedInteriorWord, expected);
    assert.equal(result.expectedConfiguration, true);
    assert.equal(result.hardwareValidated, false);
    assert.throws(() => readProbe(lines.join('\n').replace(`alpha=${alpha}`, 'alpha=100')));
  }
});

test('schema 4 separates K from alpha and requires the programmed K register', () => {
  for (const k of [0, 1, 127, 128, 255])
    for (const alpha of [0, 255])
      for (let mode = 0; mode < 4; ++mode) {
        const hex = value => value.toString(16).padStart(2, '0');
        const expected = [k < 128 ? 0x573d : 0xd73d, 0xae7d,
          alpha === 0 ? 0x0ace : 0xface, alpha === 0 ? 0x573d : 0xd73d][mode];
        const lines = fixture();
        lines[0] = lines[0].replace('schema=1', 'schema=4')
          + ` packmode=${mode} alpha=${hex(alpha)} threshold=80 k=${hex(k)}`;
        lines[1] = lines[1].replace('pack=00000001',
          `pack=${(0x800000 + k * 256 + mode).toString(16).padStart(8, '0')}`);
        // Synthetic bytes test the reader, not the renderer or hardware.
        const bytes = Buffer.alloc(2368, 0xa5);
        for (let y = 8; y < 24; ++y)
          for (let x = 8; x < 24; ++x)
            bytes.writeUInt16LE(expected, 32 + (y * 36 + x) * 2);
        for (let offset = 0; offset < bytes.length; offset += 32)
          lines[2 + offset / 32] = `PVR_RTT_BYTES offset=${offset.toString(16).padStart(4, '0')} data=${bytes.subarray(offset, offset + 32).toString('hex')}`;
        const log = lines.join('\n');
        const result = readProbe(log);
        assert.equal(result.k, k);
        assert.equal(result.expectedInteriorWord, expected);
        assert.equal(result.interiorPixelsDifferentFromRequestedColor, 0);
        assert.equal(result.expectedConfiguration, true);
        assert.equal(result.hardwareValidated, false);
        assert.equal(readProbe(log.replace(/pack=[0-9a-f]{8}/, 'pack=00000000')).expectedConfiguration, false);
        assert.throws(() => readProbe(log.replace(`k=${hex(k)}`, 'k=100')));
        assert.throws(() => readProbe(log.replace(` k=${hex(k)}`, '')));
      }
});

test('schema 5 retains dither phase observations without manufacturing expected values', () => {
  const lines = fixture();
  lines[0] = lines[0].replace('schema=1', 'schema=5') + ' packmode=1 alpha=ff threshold=00 k=00 dither=1';
  lines[1] = lines[1].replace('pack=00000001', 'pack=00000009');
  const bytes = Buffer.alloc(2368, 0xa5);
  for (let y = 8; y < 24; ++y)
    for (let x = 8; x < 24; ++x)
      bytes.writeUInt16LE((y % 4) * 4 + x % 4, 32 + (y * 36 + x) * 2);
  for (let offset = 0; offset < bytes.length; offset += 32)
    lines[2 + offset / 32] = `PVR_RTT_BYTES offset=${offset.toString(16).padStart(4, '0')} data=${bytes.subarray(offset, offset + 32).toString('hex')}`;
  const log = lines.join('\n');
  const result = readProbe(log);
  assert.equal(result.dither, 1);
  assert.equal(result.expectedConfiguration, true);
  assert.equal(result.expectedInteriorWord, null);
  assert.equal(result.interiorPixelsDifferentFromRequestedColor, null);
  assert.equal(result.hardwareValidated, false);
  assert.match(result.interiorExpectation, /unavailable/);
  for (let phase = 0; phase < 16; ++phase)
    assert.deepEqual(result.interiorPhaseHistograms[phase], { [phase.toString(16).padStart(4, '0')]: 16 });
  assert.throws(() => readProbe(log.replace('dither=1', 'dither=2')));
  assert.equal(readProbe(log.replace('dither=1', 'dither=0')).expectedConfiguration, false);
  const undithered = readProbe(log.replace('dither=1', 'dither=0').replace('pack=00000009', 'pack=00000001'));
  assert.equal(undithered.expectedConfiguration, true);
  assert.equal(undithered.expectedInteriorWord, 0xae7d);
  assert.equal(undithered.interiorPixelsDifferentFromRequestedColor, 256);
});
