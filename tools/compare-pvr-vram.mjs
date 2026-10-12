// Compare physical 8 MiB Dreamcast VRAM captures as a specified RGB565 framebuffer.
// Usage: node tools/compare-pvr-vram.mjs left.vram right.vram base stride width height
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';

const [leftPath, rightPath, ...numbers] = process.argv.slice(2);
const [base, stride, width, height] = numbers.map(Number);
if (!leftPath || !rightPath || numbers.length !== 4
    || ![base, stride, width, height].every(Number.isSafeInteger)
    || base < 0 || base % 2 || stride < width * 2 || stride % 2
    || width < 1 || height < 1 || base + (height - 1) * stride + width * 2 > 0x800000) {
  throw new Error('Usage: node tools/compare-pvr-vram.mjs left.vram right.vram base stride width height (aligned RGB565, no address wrap)');
}
const left = readFileSync(leftPath), right = readFileSync(rightPath);
if (left.length !== 0x800000 || right.length !== left.length)
  throw new Error('Both inputs must be physical 8 MiB Dreamcast VRAM snapshots');

// Inverse of bank interleaving is unnecessary: map each logical pixel address
// to the physical layout used by pvr_map32 and FBPixelWriter.
function physical(address) {
  return ((address & 0x3ffffc) * 2) | ((address & 0x400000) >>> 20) | (address & 3);
}
const rgb = word => [word >>> 11, (word >>> 5) & 63, word & 31];
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
const report = {
  format: 'RGB565', base, stride, width, height,
  leftSha256: sha256(left), rightSha256: sha256(right),
  physicalBytesDifferent: 0, framebufferPixelsDifferent: 0,
  maxNativeChannelError: [0, 0, 0], signedRightMinusLeft: [0, 0, 0],
  differingPixelsByPhase4x4: Array(16).fill(0), rows: {}, samples: [],
};
for (let i = 0; i < left.length; ++i)
  report.physicalBytesDifferent += left[i] !== right[i];
for (let y = 0; y < height; ++y) {
  for (let x = 0; x < width; ++x) {
    const offset = physical(base + y * stride + x * 2);
    const a = left.readUInt16LE(offset), b = right.readUInt16LE(offset);
    if (a === b) continue;
    ++report.framebufferPixelsDifferent;
    ++report.differingPixelsByPhase4x4[(y & 3) * 4 + (x & 3)];
    report.rows[y] = (report.rows[y] || 0) + 1;
    const ca = rgb(a), cb = rgb(b);
    for (let c = 0; c < 3; ++c) {
      const delta = cb[c] - ca[c];
      report.maxNativeChannelError[c] = Math.max(report.maxNativeChannelError[c], Math.abs(delta));
      report.signedRightMinusLeft[c] += delta;
    }
    if (report.samples.length < 64)
      report.samples.push({ x, y, physicalOffset: offset, left: a, right: b });
  }
}
console.log(JSON.stringify(report, null, 2));
