import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { pathToFileURL } from 'node:url';

// Strictly read this probe's current fixed case. A complete dump is evidence,
// not a correctness verdict and never implicitly a hardware golden.
export function readProbe(log) {
  const lines = log.split(/\r?\n/).filter(line => line.startsWith('PVR_RTT_'));
  const header = 'PVR_RTT_PROBE schema=1 execution=unclassified width=32 height=32 stride=36 guard=32 bytes=2368';
  if (lines.shift() !== header) throw Error('Missing or unsupported probe header');
  const registers = /^PVR_RTT_REG pack=([0-9a-f]{8}) base=([0-9a-f]{8}) stride=([0-9a-f]{8}) clipx=([0-9a-f]{8}) clipy=([0-9a-f]{8})$/.exec(lines.shift() ?? '');
  if (!registers) throw Error('Missing or malformed register record');
  const bytes = Buffer.alloc(2368);
  for (let offset = 0; offset < bytes.length; offset += 32) {
    const record = /^PVR_RTT_BYTES offset=([0-9a-f]{4}) data=([0-9a-f]{64})$/.exec(lines.shift() ?? '');
    if (!record || parseInt(record[1], 16) !== offset)
      throw Error(`Missing, reordered, or malformed byte record at ${offset}`);
    Buffer.from(record[2], 'hex').copy(bytes, offset);
  }
  if (lines.shift() !== 'PVR_RTT_END' || lines.length)
    throw Error('Missing end marker or duplicate/trailing probe records');
  const changes = range => [...range].filter(value => value !== 0xa5).length;
  const registerValues = Object.fromEntries(['pack', 'base', 'stride', 'clipx', 'clipy']
    .map((key, i) => [key, parseInt(registers[i + 1], 16)]));
  return {
    execution: 'unclassified', hardwareValidated: false,
    sha256: createHash('sha256').update(bytes).digest('hex'),
    registers: registerValues,
    expectedConfiguration: registerValues.pack === 1 && registerValues.stride === 9
      && registerValues.clipx === 0x001f0000 && registerValues.clipy === 0x001f0000
      && (registerValues.base & 0x01000000) !== 0,
    prefixGuardChangedBytes: changes(bytes.subarray(0, 32)),
    suffixGuardChangedBytes: changes(bytes.subarray(2336)),
    centerWord: bytes.readUInt16LE(32 + (16 * 36 + 16) * 2),
    limitation: 'Raw observations only; execution provenance and hardware correctness are not inferred.',
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  if (process.argv.length !== 3) throw Error('Usage: node tools/read-pvr-rtt-probe.mjs log');
  console.log(JSON.stringify(readProbe(readFileSync(process.argv[2], 'utf8')), null, 2));
}
