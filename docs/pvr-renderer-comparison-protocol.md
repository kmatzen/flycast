# Comparing PVR renderer output

The current software/Metal differential capture compares two executions of the
same render context. Separate emulator runs at the same frame number do not
establish that property. Existing OpenGL, Vulkan, and Direct3D screenshots must
not be described as matched inputs until their input identity is established.

## Capture boundaries

Keep two independently labelled comparisons:

* **Raster output:** native accumulation color, depth, and secondary buffers,
  before display conversion. Use identical decoded geometry, texture data,
  registers, region lists, initial buffers, and dimensions.
* **Displayed output:** framebuffer packing, dithering, scaling, field selection,
  and presentation included. Record the exact presentation settings and compare
  the same field at the same output dimensions.

Do not compare a software accumulation capture directly with a host screenshot
and attribute every difference to rasterization.

## Existing screenshot behavior

Source inspection identifies an additional transformation at the screenshot
boundary:

| Implementation | Screenshot behavior |
| --- | --- |
| `core/rend/gles/gldraw.cpp`, `OpenGLRenderer::GetLastFrame` | Selects the output framebuffer, adjusts dimensions for aspect ratio and rotation, draws its texture into a new framebuffer, then reads pixels. |
| `core/rend/vulkan/vulkan_context.cpp`, `VulkanContext::GetLastFrame` | Starts from the last presented image, adjusts dimensions for aspect ratio and rotation, and creates a screenshot attachment. |
| `core/rend/soft/SoftwareRenderer.cpp`, `GetLastFrame` | Selects the prepared presentation buffer or rasterizer color buffer, adjusts dimensions, and samples with integer coordinate mapping. |
| `core/ui/gui.cpp`, `getScreenshot` | Calls the backend screenshot method and encodes RGB PNG; savestate thumbnails request width 640. |

Consequently, equal PNG dimensions alone do not prove equivalent sampling.
Native size, aspect correction, rotation, filtering, and capture stage must be
recorded. A screenshot comparison can establish a displayed-image difference;
it cannot isolate the stage that caused it.

## Required record for each comparison

1. Record source commit, backend and transparency mode, host OS/GPU, and all
   renderer settings. Record whether the Metal path fell back to software.
2. Identify the input using a render-context snapshot or replay with hashes of
   geometry, texture/VRAM inputs, relevant registers, and initial buffers.
   A game name and frame ordinal are insufficient input identity.
3. Record capture boundary, dimensions, pixel layout, row orientation, and field.
   Disable optional enhancements or document them as a separate experiment.
4. Preserve original captures and hashes. Normalize only explicitly documented
   storage differences such as row orientation or channel order; retain the
   originals. Do not resize images to make them match.
5. Report differing-pixel count, maximum channel error, and an absolute diff.
   Report depth and secondary-buffer differences separately when available.
6. Classify each observed difference using a documented PVR rule or hardware
   result. Agreement with the software model establishes model equivalence;
   disagreement with another backend does not establish which is correct.

## Next capture work

`FLYCAST_PVR_INPUT_FINGERPRINT=1` enables a preliminary input diagnostic in
the common render queue, before `Renderer::Process`. It requires
`ThreadedRendering=no`; otherwise it prints a diagnostic and declines to hash
live memory. Each `PVR_INPUT` line records the frame, linked TA context index,
address, command byte count, and separate XXH64 hashes of raw TA bytes, full
VRAM, and the live PVR register bank. It adds no hashing when disabled.

These are diagnostic fingerprints, not cryptographic identities or complete
replay snapshots. Register timing and unrelated VRAM can cause differences;
equal hashes do not cover snapshotted context fields, settings, decoded cache
contents, or retained accumulation/presentation buffers. Use them to screen
candidate captures and investigate mismatches before claiming matched inputs.
Hashing full VRAM also adds CPU work, so disable it for performance runs.

The diagnostic includes the instantiated backend name (including factory and
initialization fallbacks). `software` identifies the software renderer object;
it does not identify whether an individual frame used Metal acceleration.
Set `FLYCAST_PVR_INPUT_PAGE_FRAME` to a decimal frame number to emit separate
XXH64 hashes for every 64 KiB VRAM page at that frame. This requires the main
fingerprint option and single-threaded rendering as above.

### Runtime screening observation (2026-10-12)

At commit `19bfde271`, two muted, single-threaded Seaman USA (`MK-51048`)
REIOS boot runs requested renderer IDs 0 and 7 respectively. Both reached more
than 230 render frames and emitted fingerprints. The requested IDs are launch
settings, not independent confirmation of the active renderer.

At frame 4 both emitted TA `882e3566018b73eb`, VRAM `fef1cd3eefa6af1f`,
and registers `f084342605d4c551`. At frame 5 both emitted TA
`44a4f4b9ed7f2744` and registers `95ec11a30f3a0424`, but VRAM differed:
`2515ffd693a5dc2f` versus `cfefd4d7db5aa67e`. These observations validate
the diagnostic path and reject full-input equality at frame 5. They do not
identify the differing bytes, establish their relevance to texture reads, or
attribute the divergence to rendering rather than other emulated state.

Next, record active backend identity and capture differing VRAM ranges before
making a visual correctness claim from these runs. The earlier equal hashes
also remain subject to the incomplete-state limitations above.

A subsequent run with backend identification confirmed `opengl` and `software`
and reproduced both frame-5 whole-VRAM hashes above. Of 128 pages, nine differed:
offsets `00440000`, `00450000`, `00460000`, `00480000`, `00490000`,
`004a0000`, `004d0000`, `004e0000`, and `004f0000`. All other page hashes
matched. The next investigation can therefore focus on these ranges and the
writes that precede frame 5. This identifies differing pages, not the exact
bytes or the cause. Local run logs are `/tmp/pvr-pages-gl.log` and
`/tmp/pvr-pages-software.log`.

Stage fingerprints subsequently localized the change to frame 4's `Render()`
call. Both backends started that frame and finished `Process()` with VRAM hash
`fef1cd3eefa6af1f`. After `Render()`, OpenGL produced `2515ffd693a5dc2f`
and software produced `cfefd4d7db5aa67e`, exactly the next frame's inputs.
Both reported `rtt=0`, `efb=1`, and framebuffer write address `00200000`.
Thus full framebuffer emulation was enabled, and the observed divergence is
already present in renderer output before the next emulation interval. The
next comparison should inspect framebuffer pixels and conversion settings at
frame 4; these hashes alone do not establish which output is correct.
Local stage logs are `/tmp/pvr-stages-gl.log` and
`/tmp/pvr-stages-software.log`.

With fingerprinting enabled, `PVR_STAGE` records the whole-VRAM hash after
`Process()` and `Render()`, the instantiated backend, RTT/full-framebuffer
flags, and framebuffer write address. It has the same single-threaded
restriction as the input fingerprints.

### Raw framebuffer comparison

Set `FLYCAST_PVR_SNAPSHOT_PREFIX` alongside the fingerprint option and selected
page frame to save `<prefix>.processed.vram` and `<prefix>.rendered.vram`.
These contain the full physical VRAM bytes after each stage. The parent
directory must exist; files are created exclusively and existing files are
never overwritten. Check `PVR_SNAPSHOT saved=1` for each completed write.
The diagnostic also reports packing mode, dithering, scaler register, and stride.
Snapshots are local investigation artifacts and may contain game data.

Seaman frame 4 captures in `/tmp/pvr-frame4.NgB49X/` confirmed byte-for-byte
equality of the two `processed.vram` inputs. Both backends reported packing
mode 1 (RGB565), dithering enabled, scaler `00000401`, and stride 1280 bytes.
The rendered VRAM files differ in 1,732 aligned 16-bit words. The first is at
physical offset `00444f02`, the last at `004f0e6a`. Interpreting these words
as RGB565, maximum absolute error is one native code value in each channel.
The signed software-minus-OpenGL channel sums are R=410, G=603, B=548.
This makes dithering and rounding useful next candidates to isolate, without
establishing either implementation as the hardware-correct result.

Source inspection identifies an ordering difference worth isolating:
`core/rend/gles/gles.cpp` adds dither to fragment color before returning it
for host blending. `SoftwareRenderer::writeDisplayFramebufferToVram` applies
it to completed frame-store pixels before packing. The reference manual's
`FB_W_CTRL.fb_dither` description places dithering upon framebuffer writing,
but does not by itself settle every intermediate precision detail.
The strengthened `DithersFinalFramebufferAfterBlending` test now actually draws
two additive layers: at (0,0), RGB (2,2,2) + (2,2,2), then RGB565 dither
(2,1,2), packs to `0020`. Dithering each layer before blending would pack to
`0821`. This tests the software model's intended stage ordering; it does not
yet prove that this difference explains all 1,732 Seaman words.

### Dithering isolation experiment

A temporary diagnostic build from `0adf9fd14` bypassed two conditions: the
OpenGL shader dithering enable in `gles.cpp` and the display-framebuffer dither
call in `SoftwareRenderer.cpp`. No PVR register value was changed, so snapshot
metadata still reports the game's `dither=1`; the experimental code bypassed
its implementation. Both temporary changes were reverted after capture.

Muted single-threaded Seaman frame-4 captures in
`/tmp/pvr-nodither.aT0Hwe/` again have byte-identical processed VRAM. With
dithering bypassed, only 50 rendered 16-bit words differ, versus 1,732 in the
normal build. Maximum native RGB565 channel error remains one code value.
This intervention substantially reduces the mismatch and establishes that
dithering behavior contributes to this comparison. It does not distinguish
stage ordering from fractional precision or spatial phase within dithering,
nor explain the residual 50 words. Hardware correctness remains a separate
question from agreement between backends.

### Reproducing the pixel analysis

`tools/compare-pvr-vram.mjs` compares physical 8 MiB VRAM files using the
Dreamcast bank mapping and an explicit RGB565 framebuffer layout. It reports
whole-file SHA-256 hashes, physical byte differences, framebuffer pixel
differences, native channel errors, affected rows, 4×4 phase counts, and up
to 64 differing pixel coordinates. It rejects unsupported sizes, odd alignment,
and framebuffer layouts that wrap VRAM. It does not rescale or filter pixels.

For the frame-4 captures:

```sh
node tools/compare-pvr-vram.mjs \
  /tmp/pvr-frame4.NgB49X/gl.rendered.vram \
  /tmp/pvr-frame4.NgB49X/sw.rendered.vram 0x200000 1280 640 480
```

The tool reproduces 1,732 differing framebuffer pixels for normal rendering
and 50 for the no-dither experiment. The latter occupy 15 of 16 pixel phases,
at rows 111 through 382; they are not confined to one dither-table position.
This spatial observation does not yet isolate sampling or interpolation as
the cause. JSON reports are saved beside both capture sets as `comparison.json`.

### Texture filtering isolation

Repeating the no-dither build with the existing
`-config config:rend.TextureFiltering=1` override in both backends reduces
the frame-4 difference from 50 pixels to one. Processed VRAM remains
byte-identical. The remaining pixel differs only in green, by one RGB565 code
value. Captures, run logs, and the JSON report are in
`/tmp/pvr-nearest.gC5JR1/`. Normal dithering code was restored and rebuilt
after the experiment; forced-nearest sampling is not an accuracy fix.

Together the interventions narrow this comparison from 1,732 differences
(normal), to 50 (dither bypass), to one (dither bypass plus forced nearest).
They establish sensitivity to dithering and texture filtering, but do not
prove a particular rounding rule or which implementation matches hardware.

### Hybrid framebuffer verification

The normal build at `60fb394dc` was run with `FLYCAST_HYBRID_SOFT=1`,
full framebuffer emulation, default texture filtering, single-threaded
rendering, and muted audio. Frame-4 hybrid output matched the earlier software
capture in every byte of physical VRAM and every framebuffer pixel. Processed
VRAM inputs also matched byte-for-byte. The rendered files share SHA-256
`075ce6bd0f823361921139f4d5e374900ebd4017aeb3228884785988684ecae2`.

This was checked both with live differential verification and, separately,
with verification disabled. The latter avoids attributing a software verifier
write to the hybrid. Its trace confirms Metal execution for the frame, with
four triangles, one texture, and no CPU translucent completion; direct
presentation is ineligible because full framebuffer emulation is enabled.
Artifacts and logs are in `/tmp/pvr-hybrid-fb.qeCM6m/`; the verifier-disabled
captures use the `direct` prefix and the comparison is `comparison.json`.
This establishes agreement through framebuffer output for this input, including
the normal filtering and dithering path. It does not establish hardware truth
or generalize to other scenes.

The existing screenshot interface is useful for presentation evidence. A shared
render-context replay/capture boundary is still required for controlled raw
comparisons across the established backends. First preserve and identify one
input, then run software, Metal, and available host backends against it. Expand
to filtered punch-through, intersecting translucency, modifier volumes, mip/fog
boundaries, render-to-texture, and framebuffer scaling/field selection.

Performance results should use the same inputs and report CPU preparation,
GPU execution, synchronization/readback, presentation, and total frame time.
Capture/verification overhead must be identified separately from normal play.
