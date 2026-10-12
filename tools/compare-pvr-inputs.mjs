// Compare captured input evidence, not complete renderer/replay equivalence.
import { readFileSync, readdirSync } from 'node:fs';
import { basename, dirname } from 'node:path';
import { createHash } from 'node:crypto';
import { isDeepStrictEqual } from 'node:util';
import { pathToFileURL } from 'node:url';

const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const uint32 = value => Number.isInteger(value) && value >= 0 && value <= 0xffffffff;
export const numericFields = ('frame isRTT clearFramebuffer globClipX globClipY fbClipX fbClipY '
  + 'fbClipWidth fbClipHeight framebufferWidth framebufferHeight fpu_cull_val_bits '
  + 'fb_W_SOF1 fb_W_CTRL fb_W_LINESTRIDE fb_R_CTRL fb_R_SIZE fb_R_SOF1 fb_R_SOF2 '
  + 'spg_control spg_status vo_control scaler_ctl y_coeff fog_clamp_min fog_clamp_max '
  + 'fog_col_ram fog_col_vert fog_density pt_alpha_ref half_offset fpu_shad_scale '
  + 'region_base fpu_param_cfg isp_feed_cfg vo_border_col region_tile_size').split(' ');

function capture(prefix) {
  const read = (suffix, size) => {
    const bytes = readFileSync(prefix + suffix);
    if (size !== undefined && bytes.length !== size)
      throw new Error(`${prefix}${suffix}: expected ${size} bytes, got ${bytes.length}`);
    return bytes;
  };
  const files = new Map([
    ['processed.vram', read('.processed.vram', 0x800000)],
    ['processed.registers', read('.processed.registers', 0x8000)],
  ]);
  const context = JSON.parse(read('.processed.context.json'));
  if (context.schema !== 1 || context.kind !== 'latched-pvr-state'
      || !numericFields.every(key => Number.isSafeInteger(context[key]))
      || !Array.isArray(context.fog_table) || context.fog_table.length !== 128
      || !context.fog_table.every(uint32)
      || !Array.isArray(context.region_array)
      || !context.region_array.every(row => Array.isArray(row) && row.length === 6 && row.every(uint32)))
    throw new Error(`${prefix}: invalid latched-state schema or arrays`);
  const start = basename(prefix) + '.input.context-';
  const indices = readdirSync(dirname(prefix)).filter(name => name.startsWith(start))
    .map(name => {
      const match = /^(0|[1-9][0-9]*)\.ta$/.exec(name.slice(start.length));
      if (!match) throw new Error(`${prefix}: malformed TA capture filename ${name}`);
      return Number(match[1]);
    }).sort((a, b) => a - b);
  if (!indices.length || indices.some((index, position) => index !== position))
    throw new Error(`${prefix}: TA context indices must be contiguous from zero`);
  for (const index of indices) {
    const suffix = `input.context-${index}.ta`;
    const bytes = read('.' + suffix);
    if (bytes.length > 0x800000 || bytes.length % 32 !== 0)
      throw new Error(`${prefix}.${suffix}: invalid TA byte count`);
    files.set(suffix, bytes);
  }
  return { files, context };
}

export function compareInputs(leftPrefix, rightPrefix) {
  const left = capture(leftPrefix), right = capture(rightPrefix);
  const files = [...new Set([...left.files.keys(), ...right.files.keys()])].map(name => {
    const a = left.files.get(name), b = right.files.get(name);
    return { name, equal: !!a && !!b && a.equals(b),
      leftSha256: a ? hash(a) : null, rightSha256: b ? hash(b) : null };
  });
  const differingContextFields = [...new Set([
    ...Object.keys(left.context), ...Object.keys(right.context),
  ])].filter(key => !isDeepStrictEqual(left.context[key], right.context[key]));
  return {
    capturedInputsMatch: files.every(file => file.equal) && differingContextFields.length === 0,
    limitation: 'Captured subset only; does not establish complete input equivalence or hardware accuracy.',
    files, differingContextFields,
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    if (process.argv.length !== 4)
      throw new Error('Usage: node tools/compare-pvr-inputs.mjs left-prefix right-prefix');
    const report = compareInputs(...process.argv.slice(2));
    console.log(JSON.stringify(report, null, 2));
    process.exitCode = report.capturedInputsMatch ? 0 : 1;
  } catch (error) {
    console.error(error.message);
    process.exitCode = 2;
  }
}
