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

### Bilinear arithmetic portability

The software NEON path explicitly uses fused multiply-add for its three
bilinear interpolation steps. The scalar and Metal paths now request the same
operations explicitly instead of depending on compiler contraction. This is
reference consistency, not a determination of PVR filtering precision.

A regression uses channel values 193, 166, 219, and 12, with interpolation
fractions `0x1.8cfee4p-2` and `0x1.8244eep-1`. Fused interpolation rounds to
149; separate multiply/add rounds to 150. The software test samples this
boundary directly; the hybrid test renders it through Metal and checks color
and depth buffers against software, including an explicit expected pixel.

All 125 renderer tests passed both natively on ARM64 and using the universal
binary's x86_64 slice under Rosetta. The latter exercises scalar software
interpolation; it is not native Intel or non-Apple platform validation.
Commands:

```sh
./build-renderer-tests/tests/flycast-renderer-tests
arch -x86_64 ./build-renderer-tests/tests/flycast-renderer-tests
```

### Extended mixed-state geometry sweep

The subpixel geometry/state corpus accepts `FLYCAST_HYBRID_GEOMETRY_SAMPLES`
between 256 and 1000000; the default remains 256. It uses the same fixed random
seed and reports the sample index in failures, so extending the run preserves
the existing prefix and reproduces later failures. A 16384-sample ARM64/Metal
run passed after the explicit fused-interpolation change (10.484 seconds):

```sh
FLYCAST_HYBRID_GEOMETRY_SAMPLES=16384 \
  ./build-renderer-tests/tests/flycast-renderer-tests \
  --gtest_filter=HybridTileRenderer.MatchesOracleForSubpixelGeometryStateCorpus
```

This extends differential coverage of mixed geometry and pixel states, not
hardware conformance coverage. Each sample is an independent triangle; use
the separate overlapping-scene corpus to exercise interactions among polygons.

The overlapping-scene corpus now accepts `FLYCAST_HYBRID_OVERLAP_SCENES`
(64 through 1000000; default 64). Its fixed-seed sequence preserves the default
prefix when extended. Each scene draws 12 overlapping triangles with mixed
depth, texture, blending, and accumulation-buffer states, comparing color,
depth, and secondary accumulation against software. The scope remains the
states generated by that test, not all possible PVR state combinations.

```sh
FLYCAST_HYBRID_OVERLAP_SCENES=4096 \
  ./build-renderer-tests/tests/flycast-renderer-tests \
  --gtest_filter=HybridTileRenderer.MatchesOracleForOverlappingSceneCorpus
```

After the mip overflow fixes, the ARM64/Metal 16384-triangle geometry sweep
passed in 5.444 seconds and the 4096 overlapping-scene sweep passed in 3.412
seconds. These are test-run durations, not renderer performance measurements.
A further 65536-scene run (786432 submitted triangles) passed in 52.453
seconds; its log is `/tmp/pvr-extended-overlap-65536.log`.

### Synchronous renderer performance boundary

The six-layer benchmark now records 20 warmed synchronous `hybrid::render`
calls, with median, nearest-rank p95, and maximum wall-clock duration, plus GPU
p95. Timing ends after readback and before the test checks that the returned
color and depth buffers match the warm-up output. This checks repeatability,
not hardware accuracy. The warm-up output is also checked across the full
640x480 buffers against software rendering of the same six layers, including
bilinear texture modulation, lookup fog, deferred sorting, and alpha blending.
Software reference generation and comparison are outside the timed calls.
`FLYCAST_HYBRID_ENFORCE_60HZ=1` requires both GPU and
whole-render-call p95 to stay within 16.67 ms, rather than checking a single GPU
timestamp alone.

An initial M1 run measured render-call median 6.45 ms, p95 9.13 ms, maximum
9.16 ms, and GPU p95 7.20 ms. These are one-run observations, not guaranteed
limits. The boundary includes renderer preparation, synchronization, and
readback, but excludes emulator CPU/audio work, upstream command generation,
and display presentation. It is therefore not an end-to-end 60 fps claim.

```sh
FLYCAST_HYBRID_ENFORCE_60HZ=1 \
  ./build-renderer-tests/tests/flycast-renderer-tests \
  --gtest_filter=HybridTileRenderer.BenchmarksDreamcastFillRateAnd72LayerStress \
  --gtest_output=xml:/tmp/pvr-render-boundary-benchmark.xml
```

### Independent hardware evidence: source audit

Inspected lxdream at commit `b3575f0af9f8251ab37d08c4206d97187c8a0fcd`:

- [test/rendload.c](https://github.com/lxdream/lxdream/blob/b3575f0af9f8251ab37d08c4206d97187c8a0fcd/test/rendload.c)
  implements a Dreamcast-side scene loader. Its input includes PVR registers,
  palette words, and VRAM sections. It restores selected registers, starts
  rendering, waits for completion, and points display scanout at the result.
- [test/testrend.c](https://github.com/lxdream/lxdream/blob/b3575f0af9f8251ab37d08c4206d97187c8a0fcd/test/testrend.c)
  submits TA test data and checks events/render completion. Its success result
  does not compare rendered pixel values. It is a useful starting point for
  hardware probes, not an existing pixel-conformance oracle.
- [test/README](https://github.com/lxdream/lxdream/blob/b3575f0af9f8251ab37d08c4206d97187c8a0fcd/test/README)
  requires an SH4 toolchain; the support library uses dc-load. Neither
  `sh-elf-gcc` nor `sh4-elf-gcc` was on this workstation's PATH at inspection.

The [ElectronAsh DC_MiSTer project](https://github.com/ElectronAsh/DC_MiSTer)
explicitly describes its input VRAM dumps as coming from reicast. Those inputs
may help exercise a renderer, but are not independent Dreamcast pixel outputs.
No hardware golden framebuffer corpus was established by this audit.

Before adopting a hardware scene replay workflow:

1. Establish a console execution and raw-data retrieval route. A screenshot
   through analog capture is not a substitute for exact framebuffer bytes.
2. Start with small synthetic scenes whose registers, texture data, geometry,
   framebuffer format, and addresses are controlled. Capture both inputs and
   output bytes, with console identity and tool/source revisions.
3. Confirm that the hardware's input is equivalent to the emulator's input.
   Flycast's `.processed.vram` dump alone is insufficient: the accompanying
   live register bank is not the latched render context, and the TA
   representation must be checked before attempting a low-level replay.
4. Compare native packed framebuffer data first. Filtering precision, dither
   phase/order, and blend rounding are the initial probes because current
   OpenGL/software experiments already isolate differences in those areas.

Do not import lxdream's scene loader unchanged as a general file loader: the
inspected implementation trusts section addresses and lengths, and does not
propagate all load failures before its render call. Any adaptation needs
bounds/truncation validation and a reviewed register-write allowlist.

### Live register capture

The selected-frame snapshot now writes `<prefix>.<stage>.registers` alongside
each `.vram` file. This is the complete 32768-byte `pvr_regs` byte array,
including fog and palette storage, with register offsets matching the emulated
bank. It is diagnostic data, not a list of registers safe to write to hardware.
The same single-threaded requirement applies. Both files are created exclusively
and their independent success flags are logged; an existing file is not
overwritten. A failed write may leave a partial file and must not be accepted
as a complete snapshot.

A muted Seaman frame-4 run verified both files at each stage. The log reports
`registers_saved=1`, `registers_bytes=32768`, and register XXH64
`f084342605d4c551`, matching the input fingerprint. Both stage register files
have SHA256 `b0b22b67a8bccb306e034225c02e1ff47de25c8665d965f41bb35f5bef905892`.
Artifacts: `/tmp/pvr-register-capture.FtdBJw/`. The owned emulator session was
stopped after capture. Latched context capture remains separate work; these
files do not yet constitute a standalone scene replay.

### Raw TA capture

The same selected-frame snapshot settings also save each linked context's
raw byte stream before backend `Process`, as
`<prefix>.input.context-<index>.ta`. The zero-based index preserves linked-list
order. Retain the accompanying `PVR_INPUT` and `PVR_TA_SNAPSHOT` log records:
they identify context address, byte count, hash, and write success. Files use
exclusive creation, so a repeated prefix cannot silently replace evidence.

A muted Seaman frame-4 capture saved context 0 at address `00555780`, with
256 bytes and SHA256
`60b87321a3d3b520b746310bd850a341da217f380cdc3deab4f279784b44ee4e`.
Artifacts are in `/tmp/pvr-ta-capture.WgaMqf/`; the owned emulator session was
stopped afterward. This live check exercised a single-context frame, not a
multi-context capture. Raw TA is upstream input, not a substitute for the
latched context state or the hardware's post-TA parameter representation.

### Latched-state diagnostics

Selected-frame capture also writes `<prefix>.<stage>.context.json`, schema 1,
kind `latched-pvr-state`. Named integer fields preserve latched framebuffer,
scanout, fog, clipping, shadow, alpha-test, and region configuration. The
culling threshold is represented by its IEEE-754 bits; booleans are numeric
0/1. The fog table and six-word region entries are explicit arrays, not C++
object dumps. Register values and framebuffer stride retain their native
units (the stride field is in eight-byte units).

This is a diagnostic subset, not a complete serialized `rend_context`.
Decoded vertices, polygon parameters, background geometry, renderer settings,
and retained buffers are not included. It must not be advertised as replayable
until those dependencies are handled and replayed results are verified.

The muted Seaman frame-4 check in `/tmp/pvr-context-capture.c8w16g/` produced
two 18249-byte JSON files. Both parsed successfully and matched field-for-field:
41 top-level fields, 128 fog words, 300 six-word region entries, and 640x480
dimensions. Both write-success records were checked and the emulator stopped.

### OpenGL/software recheck with retained inputs and latched state

Using the same diagnostic build, a separate muted OpenGL run captured frame 4
with `pvr.rend=0`, single-threaded rendering, and full framebuffer emulation.
The instantiated backend logged `opengl`. Its artifacts use the `gl` prefix
in `/tmp/pvr-context-capture.c8w16g/`; software uses `sw`.

Byte comparisons were identical for:

- `input.context-0.ta` (256 bytes);
- `processed.vram` (8 MiB);
- `processed.registers` and `rendered.registers` (32 KiB each);
- `processed.context.json` and `rendered.context.json` (18249 bytes each).

The JSON was also parsed and every named field compared. The rendered VRAM
still differs at 1732 framebuffer pixels (1905 physical bytes), with maximum
native RGB565 channel differences of `[1, 1, 1]`. Signed software-minus-OpenGL
channel sums remain `[410, 603, 548]`. The pixel report is `comparison.json`.
The owned OpenGL process was stopped after capture.

This reproduces the earlier pixel difference while ruling out differences in
the additional captured fields for this run. It does not rule out uncaptured
geometry/configuration/cache state and does not determine which renderer agrees
with hardware. It is stronger input-equivalence evidence, not full replay or a
hardware correctness result.

Run the captured-input gate before interpreting a pixel diff:

```sh
node tools/compare-pvr-inputs.mjs /path/to/gl /path/to/sw
node --test tools/compare-pvr-inputs.test.mjs
```

Prefixes identify the `.processed.vram`, `.processed.registers`,
`.processed.context.json`, and contiguous `.input.context-N.ta` files.
Exit status is 0 for matching captured evidence, 1 for differences, and 2
for invalid/missing artifacts or arguments. JSON output includes binary-file
SHA256 hashes and differing context field names. The tool validates fixed
buffer sizes, required numeric context fields, fog/region array shapes, and
TA context numbering and byte alignment. It does not compare rendered outputs
or prove that a capture represents all renderer state. Context addresses and
capture write-success records still require the accompanying log.

The captured OpenGL/software pair above passes this gate; the retained report
is `/tmp/pvr-context-capture.c8w16g/input-comparison.json`.

### Live regression after mip overflow fixes

The rebuilt ARM64 app was run with Seaman, audio disabled, software renderer
selection (`pvr.rend=7`), single-threaded rendering, full framebuffer emulation,
and `FLYCAST_HYBRID_SOFT=1 FLYCAST_HYBRID_VERIFY=1 FLYCAST_HYBRID_TRACE=1`.
The run was stopped after 458 verifier records: 140697600 compared pixel
instances, zero mismatches, and no logged hybrid rejection. Trace records
showed up to 16 triangles per frame. Log:
`/tmp/pvr-live-regression.hHIYrG/run.log`.

This was a short boot/intro regression, not a complete playthrough. Repeated
frames are included in the count. The live verifier compares the internal
frame-store output; this run does not independently verify every VRAM packing
or scanout operation. The earlier raw-VRAM comparison covers a separate frame.
Software verification and trace logging add overhead, so timings from this run
must not be reported as uninstrumented gameplay performance.

### Platform limits on existing-renderer comparisons

Selecting `pvr.rend=3` in the Apple build does not test OpenGL OIT. The
`OpenGL_OIT` factory case is excluded by `!defined(__APPLE__)`; a muted test
run confirmed `backend=opengl-fallback` in the frame-4 input record. Its
artifacts are under `/tmp/pvr-context-capture.c8w16g/oit*`, but the prefix is
only the requested configuration, not evidence of the instantiated backend.
Do not label these captures as OIT results.

For a separate Vulkan comparison build, the repository macOS setup script
specifies MoltenVK `v1.4.2-pre.973bb08`. The downloaded `MoltenVK-macos.tar`
has SHA256 `1d9975f918418176aa7f304ab7f3137a792d8fdeca3c2423b87605614df3abf3`.
Its dylib includes ARM64 and x86_64 slices. The isolated build directory is
`/tmp/pvr-moltenvk.RvPkNw/build`; the existing software/Metal build is unchanged.
Configuration must select `/opt/homebrew/lib/libpng.dylib` and the corresponding
Homebrew include directory: default discovery selected an x86_64-only library
under `/usr/local/lib` on this host. Configuration success alone is not proof
that the Vulkan renderer can initialize or capture a frame.

The isolated build subsequently completed after also setting
`ZLIB_LIBRARY=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib/libz.tbd`.
Default detection returned `-lz`, which Make incorrectly treated as a file
dependency. These are local build-cache corrections, not renderer changes.

### Vulkan via MoltenVK: first matched capture

A muted Seaman frame-4 run of the isolated app with `pvr.rend=4`, single-threaded
rendering, and full framebuffer emulation logged `backend=vulkan` and successful
VRAM, register, TA, and context snapshots. Artifacts use the `vk` prefix in
`/tmp/pvr-context-capture.c8w16g/`. The captured-input gate passes against `sw`.
The output VRAM is byte-identical to conventional OpenGL's `gl.rendered.vram`
(confirmed by `cmp`), SHA256
`567a4fa052e0cfddfa87ea0369bf633d084fb3c0b51c9366273fe0c41f3f15e6`.
It therefore reproduces the same 1732 framebuffer-pixel differences from
software, with maximum native RGB565 channel errors `[1,1,1]`.

Reports are `vk-input-comparison.json` and `vk-comparison.json`; runtime log
is `vk.log`. MoltenVK logged warnings about unsupported disabling of primitive
restart; retain those warnings with the evidence. The owned process was stopped
after capture. This establishes a conventional Vulkan/MoltenVK result on this
M1 for one input, not Vulkan OIT, other Vulkan drivers, or hardware accuracy.

### Vulkan OIT via MoltenVK

The same isolated app with `pvr.rend=5` successfully instantiated
`backend=vulkan-oit`. Frame 4 passed the captured-input comparison against
software. Artifacts use prefix `vkoit` in `/tmp/pvr-context-capture.c8w16g/`;
reports are `vkoit-input-comparison.json` and `vkoit-comparison.json`.
The muted run was stopped after capture.

OIT rendered VRAM SHA256 is
`8b690034ab85f9e9eeacbea87a4f67ff5712f4933e2862ee10b2db39bfcb39de`.
It differs from software at 77 RGB565 pixels / 81 physical bytes, with maximum
native channel error `[1,1,1]` and signed software-minus-OIT sums `[4,20,-3]`.
This is substantially fewer differing pixels than the 1732 observed for
conventional Vulkan/OpenGL, but similarity to software is not hardware truth.

Source inspection provides a candidate explanation for part of the difference:
`core/rend/vulkan/oit/oit_shaders.cpp` applies dithering to `finalColor` after
its explicit fragment blending loop, whereas the conventional fragment shader
in `core/rend/vulkan/shaders.cpp` dithers `color` before returning it for host
blending. Software also dithers the completed framebuffer. This comparison
does not isolate stage ordering from other OIT/conventional differences, and
the remaining 77 pixels have not yet been attributed to a specific operation.

### OIT filtering isolation with dithering retained

Both software and Vulkan OIT were rerun from the same Vulkan-enabled executable
with `rend.TextureFiltering=1` (force nearest), leaving framebuffer dithering
enabled. Instantiated backend records confirmed software and vulkan-oit. The
captured-input gate passes. Prefixes are `sw-nearest` and `vkoit-nearest` in
`/tmp/pvr-context-capture.c8w16g/`; both owned muted sessions were stopped.

Differences fell from 77 pixels to 28. All 28 differ only in green by one native
RGB565 step, with software higher; signed sums are `[0,28,0]`. The differences
occur only at even x / odd y, distributed over 4x4 phase indices 4, 6, 12, and
14 with counts 12, 7, 3, and 6 respectively. Those phases have dither-table
values 9, 11, 10, and 8. This phase correlation is consistent with a rounding
boundary interacting with dithering, but does not identify which operation
first differs or which output matches hardware.

The reports are `nearest-input-comparison.json` and
`nearest-oit-comparison.json`. This configuration intervention changes requested
texture sampling and is diagnostic only; it is not an accuracy fix. The input
gate does not encode emulator configuration, so the matched override is
recorded explicitly here and in the run commands.

### Configuration-aware captures (schema 2)

New context snapshots use schema 2 and include the software renderer's
configuration inputs: texture filtering, full framebuffer emulation, per-strip
sorting, modifier volumes, fog, linear interpolation, rotation, and extra depth
scale. Depth scale is stored as float bits. The selected backend is deliberately
not part of these equality fields because cross-backend comparisons require it
to differ; use the runtime backend log to identify actual execution.

The input comparator accepts legacy schema 1 but reports
`configurationCaptured: false`. Schema 2 requires all eight named settings;
different settings produce a failed captured-input match. Tests cover missing
settings and a forced-filtering mismatch. This does not claim that every
backend-specific setting or environment override is now captured; retain the
full run command and backend log. The app build and comparator tests pass.

Live schema-2 verification used two muted Seaman frame-4 runs in
`/tmp/pvr-schema2.wMxwxk/`: prefix `sw` with forced nearest (1), and `default`
with game-selected filtering (0). Both wrote valid 18468-byte context files.
The nearest capture reports framebuffer emulation=1, per-strip sorting=0,
modifiers=1, fog=1, linear interpolation=1, rotation=0, and extra-depth-scale
bits `1065353216` (1.0f). Its identity comparison passes with configuration
present. Comparing the two runs exits 1 and identifies exactly
`config_TextureFiltering` as the differing context field. Both owned emulator
sessions were stopped after capture. Reports are `identity.json` and
`filter-mismatch.json` in that directory.

### Render cadence diagnostic

`FLYCAST_PVR_TRACE_CADENCE=1` reports each window of 120 successful non-RTT
render calls. Unlike presentation counts, this does not count repeated display
presentation as new rendering. It measures wall-clock render-call throughput
and mean time inside the render routine; async GPU completion is not implied.
Use a fresh process for a measurement and avoid pauses, state loads, or backend
changes within a window. The diagnostic does not yet reset windows for those
events or measure audio underruns or display frame pacing. Emulated-time
reporting was subsequently added as described below.

A muted Seaman boot/intro run with `FLYCAST_HYBRID_SOFT=1`, framebuffer
emulation enabled, single-threaded rendering, and verifier/trace/capture
disabled reported four windows at 29.748, 15.108, 19.241, and 19.149 renders/s.
Mean render-routine times were 9.948, 10.560, 19.770, and 20.139 ms. Log:
`/tmp/pvr-cadence-seaman.log`. The owned process was stopped afterward.
The backend label is software; the environment requests hybrid acceleration
but this low-overhead run does not record per-frame fallback. These observations
do not distinguish variable game render cadence from emulator slowdown.
Emulated-time progress must be measured before claiming full-speed play.

The counter now also reports SH4 scheduler progress (`emulated_seconds`) and
its ratio to wall time (`emulation_speed`). It only runs with threaded
rendering disabled to avoid a concurrent scheduler read. A backward jump
relative to the window start resets the window; pauses and arbitrary state
loads still require a fresh measurement process.

The same muted configuration produced a first window at 0.9923 speed, followed
by a transition window at 0.9406, then three windows at 0.6440, 0.6441, and
0.6439. Each latter window advanced about 4.00365 emulated seconds over about
6.216 wall-clock seconds, at about 19.3 render calls/s. Mean render-routine
times were 19.860, 19.715, and 19.370 ms. This establishes slowdown in that
single-threaded full-framebuffer run, not merely fewer game-submitted frames.
It does not yet attribute the entire slowdown to the renderer or establish
the behavior of the threaded/async configurations. Log:
`/tmp/pvr-emulated-cadence-seaman.log`; the owned process was stopped afterward.

### Isolating display synchronization from rendering cost

With the same muted, single-threaded, full-framebuffer Seaman configuration,
OpenGL reported later 120-render windows at 0.9985–1.0014 emulation speed
(`/tmp/pvr-cadence-opengl.log`). A hybrid run with detailed tracing still
reported 0.6429 speed; sampled GPU times were 12.831–14.557 ms, encoding
1.372–1.779 ms, readback about 0.21 ms, and no CPU translucent completion
(`/tmp/pvr-cadence-hybrid-breakdown.log`). These samples do not constitute a
complete cost attribution.

Changing only the requested VSync setting to
`-config config:rend.vsync=no` restored approximately full speed in the hybrid
run. The first five later windows reported 0.9992, 0.9999, 0.9999, 0.9993,
and 1.0019 emulation speed, with mean render-call times of 15.831–16.093 ms
(`/tmp/pvr-cadence-hybrid-novsync.log`). Full framebuffer emulation remained
enabled. All three owned processes were stopped after measurement.

This isolates synchronization as a substantial performance factor in this
sequence; it is not a validated synchronized-presentation fix, a 60 fps claim,
or proof of good audio (the audio backend was null). Source inspection shows
the bundled SDL Cocoa positive swap-interval path unconditionally waits for a
display callback before flushing. The software renderer also does not forward
the detected game swap interval, unlike OpenGL. Neither observation alone
establishes the cause of the whole slowdown. Measure presentation waits and
emulator pacing before changing their policy; do not remove framebuffer
operations to improve this result.

`FLYCAST_PVR_TRACE_SWAP=1` now measures only `SDL_GL_SwapWindow`, reporting
mean and maximum wall time per 120 host presentation calls and the current SDL
swap interval. These windows include repeated game frames and must not be
interpreted as game FPS. Use a fresh process without backend or interval
changes inside a window. Timing is opt-in and does not change swap policy.

A VSync-enabled hybrid run reproduced 0.6422–0.6489 emulation speed in three
later render windows. Nearby swap windows reported means of 10.048–11.350 ms
and maxima of 16.190–16.756 ms, at interval 1. Log:
`/tmp/pvr-swap-hybrid.log`; the owned process was stopped afterward. Source
inspection also narrows the interval hypothesis: `rend_start_render` assigns
interval 1 when full framebuffer emulation is enabled, so the software path's
missing game-interval forwarding does not explain this configuration.
`rend_vblank` requests framebuffer reconstruction and presentation on every
emulated VBlank, even if the game has not submitted another render. In the
single-threaded configuration these presentation waits serialize with
emulation. Next isolate the threaded configuration and measure its emulated
progress safely, rather than assuming the same result applies to it.

### Threaded emulation progress

`FLYCAST_PVR_TRACE_EMULATION=1` samples scheduler cycles on the emulation
thread at VBlank, reporting progress every 240 VBlanks. It works with threaded
rendering without reading scheduler state from the render thread. It is not a
presentation, audio, or rendered-frame counter. Use a fresh process without
pauses or state loads; backward scheduler movement resets the window.

With hybrid rendering, VSync, full framebuffer emulation, null audio, and
`-config config:rend.ThreadedRendering=yes`, later Seaman windows measured
0.6635, 0.6671, and 0.6673 speed. Nearby swap windows still averaged
10.015–11.140 ms. Thus threading alone did not eliminate the slowdown.
Log: `/tmp/pvr-threaded-speed.log`; the owned process was stopped afterward.
The 125-test renderer suite passed during startup, so early timing windows
are excluded from this comparison. The app build and diff whitespace check
also passed. The test suite does not establish correctness of threaded live
framebuffer synchronization.

Full framebuffer rendering intentionally retains the render-completion wait
until `Renderer::Render` finishes. The queue also contains an existing FIXME
about asynchronous framebuffer reads and blinking. These constraints require
preserving ordering and VRAM visibility while investigating presentation
scheduling; simply removing synchronization is not an accuracy-preserving fix.

### Experimental Cocoa late-swap pacing

On macOS SDL/OpenGL, `FLYCAST_PVR_LATE_SWAP=1` changes a requested swap
interval of 1 to -1. It does not change interval 0 (VSync off/fast-forward)
or intervals greater than 1, and has no effect on other platforms. This is
strictly opt-in: normal VSync behavior remains unchanged. The bundled SDL
Cocoa negative-interval path waits only when no display callback has occurred
since the preceding swap; its positive path always waits for another callback.
Late swaps can tear. This experiment is not a tear-free presentation guarantee
or a recommended default.

A single-threaded hybrid Seaman run with full framebuffer emulation, VSync
requested, null audio, and this opt-in reported SDL interval -1. Three later
240-VBlank windows measured speeds 1.0006, 1.0012, and 0.9974, compared with
roughly 0.64 under positive interval 1. Nearby mean swap times were
5.042–5.393 ms. Log: `/tmp/pvr-late-swap.log`; the owned process was stopped
after measurement. The app build and whitespace check passed. No pixel
operations, framebuffer ordering, or render-completion waits were removed.
The measurement does not prove live pixel equivalence, stable audio, good
display pacing, or sustained performance across other scenes and games.

### Real audio delivery with late-swap pacing

`FLYCAST_AUDIO_TRACE_DELIVERY=1` enables cumulative SDL2 audio counters.
The callback counts invocations, underruns, and output stereo frames replaced
with silence under its existing stream mutex. The producer snapshots those
counters and reports outside that mutex every roughly four seconds of
submitted 44.1 kHz input. It also counts dropped input frames. No logging is
performed on the audio callback thread. Counters reset on backend initialization;
they are not listening tests or host-device deadline measurements.

With the computer output muted, the real `audio:backend=sdl2` backend and a
2822-frame buffer, the single-threaded full-framebuffer hybrid/late-swap run
recorded six startup/early underruns (6144 output frames replaced by silence)
by 353280 submitted input frames. The count remained six through 1413120
submitted frames and 1384 callbacks, with zero dropped input frames. Thus
there were no additional reported underruns across approximately 24 seconds
of subsequent submitted audio. Three later emulation windows measured
1.0016, 0.9995, and 0.9978 speed. This is stronger than the null-backend timing
result, but not evidence of glitch-free startup or long-session audio stability.
Log: `/tmp/pvr-late-swap-audio.log`; the owned emulator was stopped after
measurement, and computer output remains muted. Build and whitespace checks
passed. No audio buffer policy or renderer pixel operations were changed.

### Live agreement with late-swap enabled

A separate correctness run enabled `FLYCAST_HYBRID_VERIFY=1` and
`FLYCAST_HYBRID_TRACE=1` alongside `FLYCAST_HYBRID_SOFT=1` and
`FLYCAST_PVR_LATE_SWAP=1`, with single-threaded rendering, full framebuffer
emulation, requested VSync, and null audio. It verified 706 frames containing
216883200 pixel instances with zero mismatches and zero logged hybrid
rejections. The sequence reached the 16-triangle, six-texture scene; sampled
trace entries used ordered autosort and no CPU translucent completion.
Log: `/tmp/pvr-late-swap-verify.log`; the owned process was stopped afterward.

This check compares packed color values in the software and hybrid frame
stores. The verifier restores initial VRAM before running the software path,
and verification runs ultimately use the software result. Therefore this is
not an independent comparison of final hybrid-only VRAM writes, display
scanout, tearing, or hardware output. Its added reference-rendering cost also
makes this run unsuitable for performance or audio conclusions. Independent
hybrid-only framebuffer captures remain necessary for the final write stage.

An independent frame-4 write-stage check subsequently ran software-only and
hybrid-only processes with late-swap enabled, full framebuffer emulation,
single-threaded rendering, null audio, and the verifier disabled. Captures are
under `/tmp/pvr-lateswap-final.WoNnce/{sw,hybrid}`. The input comparator passed:
raw TA, processed VRAM, live registers, and schema-2 latched/configuration
fields matched. The hybrid trace between the processed and rendered markers
shows four triangles, one texture, no CPU translucent completion, and no
rejection. Both owned processes were stopped after capture.

The complete rendered 8 MiB VRAM images are byte-identical, SHA-256
`075ce6bd0f823361921139f4d5e374900ebd4017aeb3228884785988684ecae2`.
The RGB565 comparison at base `0x200000`, stride 1280, 640 × 480 reports zero
differing pixels and zero native-channel error. Input-comparator fixture tests
also passed. This closes the independent framebuffer-write check for this
early frame only; it does not extend that proof to the later translucent
sequence, every captured/uncaptured state, host scanout, or Dreamcast hardware.

The independent comparison was extended to frame 450 under
`/tmp/pvr-lateswap-translucent.Nv9Phg/{sw,hybrid}` using the same settings and
the verifier disabled. Both captures use RGB565 with dithering, base
`0x200000`, stride 1280, and dimensions 640 × 480. The captured inputs **do
not match**: registers and schema-2 context/configuration fields agree, but
raw TA and processed VRAM differ. Consequently the 70121 differing output
pixels (245338 bytes across full VRAM) cannot be attributed to rasterizer
behavior. Both owned processes were stopped after capture.

Software processed VRAM SHA-256:
`aacf66d7cdd33829690658c474b88dab7236284d20c81fda21627514fe264294`;
hybrid processed VRAM SHA-256:
`57ae514311d46fb9546c5ae90961fa69e3b1ec2abb49bd2dd10bcb3974428ca4`.
Raw TA hashes are respectively
`9c9ea2818f20cc4cc716b005b5570c45dbd99d5b60c48a6e39fbaf7bbae0a36b`
and `3b96630b2c142dd1974f589f124c7cee4a6348250f46e1b87345207c729735fd`.
Do not publish this as a matched-input renderer diff. A next useful check is
to extend same-run verification through each path's final VRAM commit, with
the initial VRAM restored between them, rather than relying on equal frame
numbers from separate game runs.

### Same-run final VRAM verification

`FLYCAST_HYBRID_VERIFY_VRAM=1` extends `FLYCAST_HYBRID_VERIFY=1` when
threaded rendering is disabled and either RTT or full framebuffer emulation
is active. Other configurations do not emit VRAM verification results.
Successful hybrid rendering performs the production framebuffer commit,
captures the entire 8 MiB VRAM image, restores initial VRAM (invalidating
locked texture pages), then runs and commits the software reference. The
diagnostic reports byte differences between the resulting images. Single-pass
commit code is shared with the normal hybrid path; multi-pass commits remain
inside the existing hybrid pass loop. Verification leaves the software result
active and adds substantial memory traffic, so it is not a performance test.

A muted Seaman run with late-swap enabled verified 398 full VRAM images with
zero byte mismatches, including the later 16-triangle, six-texture translucent
scene. Log: `/tmp/pvr-vram-verifier.log`; the owned process was stopped.
The app build, whitespace check, and existing 125 renderer tests passed.
This exercises the new integration check for screen framebuffer writes; RTT
and multipass coverage must be established separately. Both paths still share
framebuffer conversion code, so agreement does not independently prove that
conversion matches Dreamcast hardware, nor does it measure host scanout.

The software display-conversion tests now independently check two explicit
FB_W_CTRL rules from Sega's Dreamcast/Dev.Box System Architecture, printed
page 343: ARGB1555 uses an inclusive alpha threshold, and KRGB0555 uses K[7].
All 65536 alpha/threshold byte pairs and all 256 K values with both zero and
full pixel alpha are covered. Tests reinterpret the written word as RGB565
to expose bit 15, which normal 0555 scanout discards. All 127 renderer tests
pass on ARM64 and under x86_64 Rosetta. This covers the software presentation
conversion helper, not direct execution of the separate TexCache VRAM writer;
it is a documented-rule regression check, not a hardware measurement.

The framebuffer writer audit found and fixed a clipped-row address error for
packed RGB888 with padding not divisible by three. The previous starting-row
calculation divided padding by bytes per pixel, discarding the remainder;
later row advances correctly retained the full byte padding. For a 640-pixel
RGB888 row, stride 1928, and clipped starting row 32, the old code started 64
bytes early. The production writer now uses a tested byte-based row-address
helper. FB_W_LINESTRIDE is specified in 64-bit units (Sega architecture,
printed page 343), so an eight-byte padding amount is valid.

Regression coverage includes that counterexample, 16/24/32-bit rows, several
eight-byte-aligned strides, and zero/nonzero clipped starting rows. The
existing zero/short-stride fallback policy is preserved, not newly validated.
The app builds and all 128 tests pass on ARM64 and x86_64 Rosetta. This checks
the exact address helper used by the production writer; full VRAM-write
integration cases with these padded RGB888 layouts remain to be added.

The production framebuffer row traversal was subsequently extracted into
`forEachFramebufferWriteRow`, which is called directly by `writeFramebufferLW`.
A byte-level regression executes that traversal with a test RGB888 storage
callback, nonzero X/Y clipping, multiple rows, a one-byte row padding, and
guard bytes. It compares the entire destination buffer against independently
indexed expected bytes, including untouched pixels and padding. All 129 tests
pass on ARM64 and x86_64 Rosetta, and the app builds. This covers production
source/destination row traversal, but the test callback does not exercise
`FBPixelWriter`'s physical VRAM mapping or the production color packer.

A follow-up failing regression exposed missing clip intersection in that
traversal: negative top-left coordinates skipped visible rows, and empty or
fully outside rectangles still invoked the row callback. The traversal now
intersects signed clip bounds with image dimensions before constructing source
pointers, rejects nonpositive extents, and uses 64-bit endpoint arithmetic.
The new test failed before the fix and passes afterward. The app builds and
all 130 tests pass on ARM64 and x86_64 Rosetta. This hardens the writer's input
contract; it does not imply a demonstrated game-visible defect for the normal
software-renderer path, which generally supplies preclipped rectangles.

The production `pvr_map32` arithmetic now delegates to the pure
`pvrMap32Address` helper with unchanged platform mask/bank parameters. A test
checks all 8388608 Dreamcast byte addresses against a quotient/remainder
bank-interleaving reference, plus explicit bank-transition and aperture-wrap
cases. The app builds and all 131 tests pass on ARM64 and x86_64 Rosetta.
This establishes agreement between the production mapping and the modeled
bank layout; it is not a physical-hardware observation and does not yet test
write side effects, dirty tracking, RTT pointer wrapping, or Naomi geometry.

The RTT writer audit found raw destination-pointer increments with no VRAM
boundary handling. `TexPixelWriter` now uses the tested `TexturePixelWriter`:
destinations within VRAM are represented as offsets masked to its power-of-two
size, while external staging-buffer destinations retain linear addressing.
The initial clipped-row displacement is applied after selecting the storage
mode, so it cannot first construct a pointer beyond VRAM. Byte writes and row
advances wrap without integer division. The regression checks end-of-VRAM
pixel continuation, a row advance crossing the boundary, and linear staging.
The app builds and all 132 tests pass on ARM64 and x86_64 Rosetta. This removes
an unbounded host-pointer path and implements the modeled VRAM address wrap;
real-hardware RTT boundary behavior and live RTT scenes remain unverified.

Post-writer-change live regression (`12bb2560c` app build) repeated muted
Seaman verification with hybrid, color/VRAM verification, full framebuffer
emulation, single-threaded rendering, and experimental late-swap enabled.
All 505 compared frames matched in both color buffers and complete 8 MiB
VRAM images, with zero logged hybrid rejections. The run reached the
16-triangle, six-texture translucent sequence. Log:
`/tmp/pvr-write-regression-live.log`; the owned emulator was stopped.
All verified frames were screen writes (`rtt=0`), so this is a live regression
check for the common framebuffer path, not live coverage of the RTT wrap fix.
No performance conclusion is drawn from a verifier-enabled run.

Production `FBLineWriter` color packers and their existing rounding helper
were extracted unchanged into `FramebufferPack.h`, shared by `TexCache.cpp`
and the standalone tests. A byte-exact regression now invokes those actual
packers for all seven supported write formats with explicit expected bytes,
including alpha equal to/below threshold and K substitution. It also verifies
source cursor advancement. All 133 tests pass on ARM64 and x86_64 Rosetta,
and the app builds. The storage sink is test-owned, so these checks cover
production packing but not mapped VRAM side effects or hardware output.

A combined regression now connects each of the four production 16-bit RTT
packers directly to the production wrapping texture writer. It writes across
the end of a guarded test VRAM allocation, advances across row padding, and
checks every resulting byte against explicit packed words while ensuring the
surrounding guards and untouched memory remain intact. All 134 renderer tests
pass on ARM64 and x86_64 Rosetta. This validates packer/writer composition;
it still does not execute a complete game RTT render or establish physical
hardware boundary behavior.

A post-writer-change real-audio run used the app built after production packer
extraction, with SDL2 audio, a 2822-frame buffer, single-threaded full-framebuffer
hybrid rendering, late-swap enabled, and verification disabled. Computer output
mute was confirmed. Six early underruns were recorded by 353280 submitted
input frames; the count remained six through 3002880 submitted frames and
2937 callbacks, with zero dropped frames. This is approximately 60 subsequent
seconds of submitted audio without another reported underrun. The final three
emulation windows measured 1.0026, 1.0006, and 0.9976 speed. Log:
`/tmp/pvr-postwrite-audio-soak.log`; the owned emulator was stopped. This
establishes no observed regression in this repeated scene, not broad game,
long-session, tear-free presentation, or glitch-free startup coverage.

### Hardware probe substrate follow-up

KallistiOS HEAD resolved to `568c3404c292a941bcf550465bd968f4fa249aa2`
during this audit. Its [RTT example](https://github.com/KallistiOS/KallistiOS/blob/568c3404c292a941bcf550465bd968f4fa249aa2/examples/dreamcast/pvr/texture_render/texture_render.c)
renders polygons to a texture and displays that texture, but does not compare
readback bytes. It is a candidate execution substrate, not a golden corpus.
The [PVR API](https://github.com/KallistiOS/KallistiOS/blob/568c3404c292a941bcf550465bd968f4fa249aa2/kernel/arch/dreamcast/include/dc/pvr.h)
provides explicit-size RTT and a render-completion wait. Initial RTT support
is 16-bit; pitch must be at least the render width and a multiple of four
pixels. Primitive submissions require aligned storage. These constraints
must be honored in a hardware probe rather than inferred from emulator-only
success. Neither `sh-elf-gcc` nor `sh4-elf-gcc` was on PATH in this audit.

Before treating such a probe as evidence, build a pinned executable, wait
for actual render completion, read the target through the uncached physical
VRAM view, and export raw bytes plus programmed registers and input vertices.
Record whether execution was on a console or emulator; emulator output must
never populate the hardware-golden category. Start with an allocated interior
RGB565 target and guards. End-of-VRAM tests require dedicated memory ownership
so wrapped output cannot overwrite live SDK lists or framebuffers. Dithering
and alternate pack modes also require auditing SDK register programming, since
writing registers before submission may be overwritten when rendering starts.
No probe has yet been built or run, and no new hardware result is claimed.

The pinned SDK was cloned to `/tmp/pvr-kos-probe.V8emEs/KallistiOS` and its
HEAD verified. The toolchain builder is now `utils/kos-chain` (not the older
`utils/dc-chain` path). A C-only stable-profile build was started with four
jobs, temporary build cleanup enabled, and installation confined to
`/tmp/pvr-kos-probe.V8emEs/sh-elf`. Its configuration disables C++, Objective-C,
and Objective-C++; it retains the SDK's normal SH precision modes and Newlib.
Log: `/tmp/pvr-kos-probe.V8emEs/toolchain.log`. The first observed stage was
downloading GCC 15.2.0; no compiler completion is claimed. Initial free disk
space was about 11 GiB, so inspect usage before allowing later build stages
to continue. This preparation does not install over system toolchains.

`tests/manual/pvr-rtt-probe` now contains an initial SDK probe source and
Makefile, not yet cross-compiled or run. It allocates an interior 32 × 32
RGB565 target with 36-pixel stride and 32-byte surrounding guards, submits a
solid quad, waits for TA readiness and render completion, and prints final
registers plus every allocation byte. It requests no dithering and labels
execution `unclassified`. It intentionally does not test end-of-VRAM wrapping
before ordinary allocated-target behavior is established. After configuring
the pinned SDK environment, build with `make -C tests/manual/pvr-rtt-probe`.
The toolchain build has advanced through GCC download/unpacking/patching;
free disk space at this point was about 9.5 GiB. Compiler and probe execution
remain pending, so source inspection is not counted as a test pass.

`node tools/read-pvr-rtt-probe.mjs capture.log` validates the current fixed
probe record format, requires all sequential byte records and the end marker,
and reports a content hash, actual registers, guard changes, and center word.
It keeps every capture unclassified and does not infer rendering success from
expected configuration. Two Node fixture tests pass, covering complete data
and rejection of truncated, reordered, duplicate, malformed, or spuriously
hardware-labeled records. These are parser tests only. The existing toolchain
process was checked live and had reached Binutils compilation; no probe
execution or compiler completion is claimed.

The probe reader additionally reports changed row-padding bytes and all 256
pixels in a 16 × 16 interior window, away from quad edges. Its expected
interior word `0xae7d` comes from the requested undithered RGB565 color, not
from hardware data. A no-render sentinel dump therefore reports 256 differing
interior pixels even if registers look correct. Three parser tests pass,
including independent detection of prefix/suffix guard and padding changes.
Matching interior pixels still never changes the unclassified provenance.

The isolated toolchain completed Binutils and GCC pass 1 (`sh-elf-gcc
--version` reports 15.2.0), then terminated with exit 2 while building newlib.
The failing command was the host documentation generator: Homebrew's
`makeinfo` script names an unavailable `/usr/bin/perl5.30`. Invoking the same
script with `/usr/bin/perl` successfully reports Texinfo 7.1. The build was
resumed with `make -o build-gcc-pass1
MAKEINFO='/usr/bin/perl /opt/homebrew/opt/texinfo/bin/makeinfo'`, preserving
completed compiler work and leaving the system installation unchanged.
The resumed process is compiling newlib math objects; full toolchain, SDK,
and probe completion remain pending. This is build progress, not renderer
validation or hardware evidence.

Newlib subsequently completed installation and the build advanced to GCC
pass 2. With the pinned SDK environment sourced in a fresh shell,
`make -C tests/manual/pvr-rtt-probe probe.o` completed successfully using
`kos-cc`. This validates compilation against those headers only; no linked
probe or runtime result exists yet.

For the forthcoming emulator run, Flycast's existing SCIF console can
capture probe text without changes to SH4 code: set transient
`config:Debug.SerialConsoleEnabled=yes` and
`config:Debug.SerialPTY=false`, and redirect stdout to a fresh log. The
SCIF pipe writes bytes to stdout when no PTY is selected. The pinned KOS
initialization registers its SCIF debug handler after DC-load detection,
and Flycast accepts ELF content directly. This source inspection identifies
the intended transport; successful receipt of the complete probe record
still needs runtime verification. Emulator output must remain labeled as
emulator evidence, never hardware ground truth.

The SDK kernel library and RTT probe subsequently linked successfully with
the installed pass-1 compiler and completed newlib. `file` identifies the
probe as a statically linked 32-bit little-endian Renesas SH ELF. The full
SDK make returned exit 2 because its unrelated host `dcbumpgen` utility
could not link PNG symbols; this did not prevent `libkallisti.a` or the
probe ELF from being built. GCC pass 2 continues separately.

An initial software-renderer run with null audio, single-thread rendering,
and the SCIF console enabled successfully emitted the KallistiOS banner,
including pinned revision `568c340`. This confirms ELF boot and the serial
output transport. At the first observation it had not emitted any
`PVR_RTT_` records, so no RTT rendering result is claimed. The run log is
`/tmp/pvr-kos-probe.V8emEs/software-run.log`; initialization and probe
completion still require investigation.

Temporary `dbgio_write_str` markers in the isolated SDK established that
initialization completes. Probe markers then reached readback, despite no
standard-output records appearing. The probe now formats complete records
with `snprintf` and sends them through `dbgio_write_str`, the already verified
debug transport, instead of relying on stdio routing. Its stage markers use
the separate `PVR_PROBE_STAGE` prefix and do not alter the capture schema.

The first complete RTT capture (backend attribution corrected below) is
`/tmp/pvr-kos-probe.V8emEs/software-dbgio-run.log`. The strict reader reports
matching requested configuration, zero changed prefix/suffix guard bytes,
zero changed row-padding bytes, and all 256 sampled interior pixels equal
to `0xae7d`. Allocation SHA-256 is
`dcfb46ac55592ed049272f1d8e73296df4518b7e40e093ed0b9876e4b9f6cf1b`.
This is Flycast execution with an instrumented SDK, not
Dreamcast hardware evidence. It covers one interior, undithered RGB565 RTT
allocation; it does not validate other formats, edge wrapping, or hardware
equivalence. The parser's three synthetic tests still pass.

Backend attribution correction: the first command set
`FLYCAST_HYBRID_SOFT=0`, but `tryRenderHybrid` checks for the variable's
presence, not its numeric value. That run must not be described as
software-only evidence. The corrected independent runs are:

- `software-only-run.log`: explicitly unset `FLYCAST_HYBRID_SOFT`,
  `FLYCAST_HYBRID_VERIFY`, and `FLYCAST_HYBRID_VERIFY_VRAM` with `env -u`.
- `hybrid-traced-run.log`: set `FLYCAST_HYBRID_SOFT=1` and
  `FLYCAST_HYBRID_TRACE=1`, with both verification variables unset.
  Trace reports `rtt=1`, four triangles, zero textures, and `reject=0`.
- `rtt-verified-run.log`: enable both verification variables as well.
  Same-frame verification reports `bytes=8388608 mismatches=0 rtt=1`
  and `pixels=307200 mismatches=0`.

All logs are under `/tmp/pvr-kos-probe.V8emEs/`. Both independent runs
produce the allocation hash above, matching registers, unchanged guards
and padding, and zero interior pixel mismatches. This establishes an actual
RTT case for software/hybrid agreement, supplementing earlier game runs
that contained no RTT frames. Same-frame verification leaves software's
result in VRAM and is not an independent accelerated-only capture; the
separate traced run supplies that evidence. Shared packing code and an
instrumented SDK remain limitations, and no original-hardware result exists.

The temporary SDK initialization markers were then removed with a targeted
patch; its tracked worktree is clean at the pinned revision. After rebuilding
the kernel and relinking with completed GCC 15.2.0, probe ELF SHA-256 is
`2dd9b68ff6c354e7babab2a78802e6fa96b17fa2baac30e3e283690749e4a8cc`.
`rtt-clean-sdk-run.log` reproduces the allocation hash and zero pixel/full
VRAM verifier mismatches without SDK instrumentation.

An existing-OpenGL baseline uses the same ELF, renderer 0, single-thread
rendering, and full framebuffer emulation. Without explicitly enabling
`rend.RenderToTextureBuffer`, its center remained the `0xa5a5` sentinel;
that run is not a valid rendered-color comparison. With
`rend.RenderToTextureBuffer=yes`, `rtt-opengl-readback-run.log` reports
matching requested registers, unchanged guards/padding, and center word
`0xb69e` (all 256 interior pixels differ from requested `0xae7d`). Its
allocation hash is
`685917cfd1171c864fd6d42887de7d518462605953087fdc05ac8dc0a7fc1bef`.
This establishes a reproducible backend difference under this configuration,
not an OpenGL accuracy verdict: color processing/settings and matched-input
provenance still need investigation, followed by original-hardware comparison.

An opt-in `FLYCAST_PVR_TRACE_RTT_READBACK=1` diagnostic now logs the RGBA
sample at (16,16), immediately after OpenGL readback and before CPU packing.
It does not modify pixels. In `rtt-opengl-trace-run.log` it reports a 640x480
host target, 72-byte stride, pack=1, dither=0, and `rgba=adceefff`.
Applying the existing `roundColor` routine gives R=22, G=52, B=30 and packed
`0xb69e`, explaining the observed CPU writeback exactly. In contrast,
truncating those readback bytes gives `0xae7d`.

The readback RGB bytes are consistent with expanding (21,51,29), the RGB565
components of `0xae7d`, to normalized 8-bit channels. `BindRTT` requests an
RGB texture with `GL_UNSIGNED_SHORT_5_6_5`; this supports a double-quantization
explanation (host target quantization followed by rounded CPU packing).
The actual host attachment bit depths have not yet been queried, so that
first-stage attribution remains an inference. The diagnostic build passed;
no existing-renderer quantization policy was changed and no hardware
correctness claim follows from this observation.

The attachment-precision diagnostic resolves the remaining first-stage
uncertainty on this M1: `rtt-opengl-attachment-run.log` reports
`PVR_GL_RTT_ATTACHMENT rgba_bits=5/6/5/0` together with the same `adceefff`
sample. Thus the host target is already RGB565 before RGBA8 readback and
rounded CPU packing. The opt-in query is restricted to GL/GLES 3+ builds
and changes no pixels. A focused production-packer test checks that all
65,536 RGB565 words survive nearest normalized 8-bit expansion followed
by truncation, and separately records the captured sample's rounded
`0xb69e` result. This tests a conversion invariant, not PVR conformance.

Matched-input follow-up: captures under `/tmp/pvr-rtt-inputs.jQMwSg/`
(`sw2` and `gl`) target the probe's RTT frame 2. Both use full framebuffer
emulation, RTT buffer writeback, single-thread rendering, and no hybrid
verification. `compare-pvr-inputs.mjs` reports `capturedInputsMatch=true`,
configuration captured, and no differing context fields. Shared hashes:

- Processed VRAM: `a93aba42aed4f1e2f7105b5ba0ca6621a8771be3d88525ac1cedec841e0b2172`
- Registers: `b12fb289d4b10e1b51a75933af7a5c324f5d22da7fe2f72e2c1392c33bd9c309`
- TA context 0: `0447980850061fbf4d565fe7e4041177adced7fdb908c5fbdfcd22897c016c4c`

The allocation hashes and interior results reproduce the earlier software
and OpenGL observations. Full post-render VRAM differs in 1,008 of 8,388,608
bytes. The center-sample conversion diagnosis does not by itself explain
every spatial difference; coverage and pixel-by-pixel analysis remain
separate checks. These captures establish equality of the recorded input
subset, not complete hidden-state equivalence or hardware accuracy.

Spatial analysis of those allocations finds identical coverage: both fill
x=4..27, y=4..27 (576 pixels), with 448 black background pixels. Software
stores `0xae7d` throughout the square. OpenGL stores `0xb69e` at 288 pixels,
`0xae9e` at 144, and `0xb67e` at 144. These pairs account for exactly
1,008 changed bytes (576+288+144); there are no coverage differences in
this probe. The reader now reports a 32x32 pixel-word histogram, excluding
guards and row padding, and its three synthetic tests pass.

`rtt-host-dither-run.log` reproduces the OpenGL allocation hash and reports
`dither=0 host_dither=1` with a 5/6/5/0 attachment. The host dither state
is therefore enabled independently of the requested PVR bit in this run.
This is consistent with the spatial color pattern; a controlled host-dither
toggle is still needed to isolate its causal contribution. The diagnostic
only queries state, and the normal renderer remains unchanged.

Controlled follow-up adds an opt-in diagnostic override,
`FLYCAST_PVR_DISABLE_RTT_HOST_DITHER=1`, which disables host dithering only
for RTT and restores its prior enabled state after readback. It is not a
default policy change. In `/tmp/pvr-rtt-inputs.jQMwSg/gl-no-host-dither.log`,
the trace confirms host_dither=0 and the same 5/6/5/0 attachment and
`adceefff` center readback. All 576 quad pixels become `0xb69e`; the 448
background pixels remain black, with unchanged guards/padding. Allocation
SHA-256 is `639d4c0938ea21922828ad16531697df9f04f8fc491b1e6f75968105f26a692c`.
Captured inputs match the baseline OpenGL run byte-for-byte and in all
recorded context fields. This isolates host dithering as the source of
the spatial variation, while the rounded repacking discrepancy remains.
It still does not establish the original PVR's quantization behavior.

The Sega architecture document's §3.4.10 format table explicitly excludes
24/32-bit formats from drawing to a texture map. The RTT probe therefore
now covers selection among the four documented 16-bit formats rather than
adding unsupported wider formats. Build with `make -B -C
tests/manual/pvr-rtt-probe CFLAGS=-DPVR_PROBE_PACK_MODE=N`, where N is 0..3
(default 1). The forced rebuild matters when changing the compile-time mode.
Schema 2 records `packmode`; the reader retains schema-1 RGB565 compatibility.
All variants request dither off, K=0, alpha threshold=0, and opaque vertex
alpha. These do not yet sweep alpha boundaries or K values.

The first mode-2 execution, `rtt-4444-verified.log` in the isolated probe
directory, reports 576 pixels `0xface`, 448 background pixels `0xf000`,
unchanged guards/padding, and all 256 interior samples matching. Allocation
SHA-256 is `1c69b1e178534fcb9e63a7d6969ea567eb191e29c4cb5468df9248b384cab213`.
Same-frame Metal/software verification reports zero differences across
307,200 color pixels and all 8,388,608 VRAM bytes for rtt=1. Four parser tests
pass. Modes 0 and 3 are selectable but have not yet been executed; neither
format selection nor matching emulators establishes hardware conformance.

Modes 0 and 3 have now also executed through same-frame Metal/software
verification. `rtt-0555-verified.log` contains 576 words `0x573d` and 448
black `0x0000` words; allocation SHA-256 is
`6b279b68578fa31e9cbfa9b0bc5ce2f4de8d467e78082afd9b5e4d88c1d068d0`.
`rtt-1555-verified.log` contains 576 words `0xd73d` and 448 background
`0x8000` words; allocation SHA-256 is
`0b8f083bfa8444ae07eb937024557d52758ad974e35c5b5ff74af711a1f0c259`.
Both have unchanged prefix/suffix guards and row padding, expected register
configuration, and zero interior-sample differences. Each verifier reports
zero differences across 307,200 color pixels and 8,388,608 VRAM bytes with
rtt=1. Thus all four documented 16-bit formats have an executed probe case,
but only for this opaque, undithered quad with K=0 and threshold=0. General
alpha/K/dither behavior and original-hardware agreement remain unverified.

After these runs, the full focused renderer suite passes all 135 tests on
native ARM64 and x86-64 under Rosetta. Logs are `renderer-tests-native.log`
and `renderer-tests-x86.log` in the isolated probe directory. This is a
regression check of the current model, not an expanded hardware claim.

The probe now preserves vertex alpha explicitly (`context.gen.alpha=true`)
while retaining opaque ONE/ZERO blending. Compile-time byte parameters
`PVR_PROBE_ALPHA` and `PVR_PROBE_ALPHA_THRESHOLD` select input alpha and
the framebuffer threshold. Schema 3 records both; older schemas remain
readable. Five parser tests pass, including below/equal/above-threshold
expectations and malformed-alpha rejection. These expected values express
the current model, not hardware-derived goldens.

Executed mode-3 runs with threshold 128 and alpha 127, 128, and 129 all
complete with intact guards/padding and zero full-VRAM/color verifier
differences. Logs are `rtt-alpha127-threshold128.log`,
`rtt-alpha128-threshold128.log`, and `rtt-alpha129-threshold128.log` in
the isolated probe directory. Alpha 127 stores 576 quad words `0x573d`
(allocation hash `a0e0da8cf347fb14c99f71089740290ad38f4ca0d68a2736b6d17030cdf65a5f`);
alpha 128 and 129 store `0xd73d` (allocation hash
`0b8f083bfa8444ae07eb937024557d52758ad974e35c5b5ff74af711a1f0c259`).
The equality case therefore exercises the modeled inclusive comparison
through actual RTT writes. The opaque background stays `0x8000` in all
three. This tests one threshold boundary in emulator execution only.

`tools/run-pvr-rtt-probes.mjs` now automates the seven executed cases:
four default formats and three alpha-128 boundary cases. Invoke it with
absolute paths to Flycast, the configured SDK `environ.sh`, and a new
output directory. It refuses an existing output directory, force-rebuilds
each case, archives each ELF with its hash, and retains build/run logs.
It clears inherited `FLYCAST_HYBRID_*` and `FLYCAST_PVR_*` experiment
switches before setting the verification switches. Each run has a 60-second
limit and is interrupted after a complete end marker; failure to shut down
within five seconds triggers termination and is treated as failure.

Acceptance requires matching requested mode/alpha/threshold and registers,
unchanged guards/padding, matching interior samples, complete strict parsing,
and explicit zero-difference color and full-VRAM RTT verifier records. The
runner never promotes emulator output to a hardware golden. Its first full
run passed all seven cases; artifacts and the JSON report are under
`/tmp/pvr-rtt-batch-20261012-a/`. Syntax checking also passes. This replaces
manual per-case build/run handling, not the need for hardware validation.

Added `HybridTileRenderer.BenchmarksDocumentedSmallTriangleMix`, motivated
by Sega System Architecture §1.2's one-million-polygons/second statement
for 100-pixel triangles with a 75% opaque / 25% translucent mix. The test
uses 16,664 untextured triangles per 640x480 frame, 12,498 opaque and 4,166
translucent, each with geometric area 100 pixels. It compares against the
software rasterizer, then samples 20 synchronous hybrid calls and records
wall/GPU timing. The opt-in 60-Hz gate checks both p95 values. This modeled
workload does not claim to reproduce the undocumented details of Sega's
measurement or include full-emulator overhead.

The first launch rejected an empty texture atlas required by the hybrid
API; the fixture now supplies a dummy texel. The corrected launch is still
running as of this record, with no timing verdict yet. A one-second host
sample places the active CPU work in `hybrid::renderInternal`; sample and
run logs are `/tmp/pvr-small-triangle-sample.txt` and
`/tmp/pvr-small-triangle.log`. Do not treat this new benchmark as passing
until the current run completes and its correctness/timing results are read.

The small-triangle baseline completed with matching pixels/depth but failed
the explicit 60-Hz gate: wall median 4018.49 ms, p95 4137.65 ms, and GPU
p95 29.12 ms. Investigation found repeated global candidate scans with
linear searches through the resolved prefix in translucent topological
ordering. Both tile-local and global resolution now use minimum-index
priority queues. Each node is queued when its indegree reaches zero,
preserving the former lowest-input-index-first tie break and cycle failure
behavior without repeatedly searching resolved nodes. Tile bins are built
in increasing original index, so local index ordering is equivalent there.

The same workload after this change retains matching pixels/depth and
records wall median 22.92 ms, p95 24.65 ms, maximum 25.26 ms, and GPU p95
18.32 ms (`/tmp/pvr-toposort.xml`). Wall p95 improves about 168-fold, but
both the wall and GPU 16.67-ms gates still fail. No Dreamcast-spec performance
completion is claimed. All 136 focused tests pass on native ARM64 and
x86-64/Rosetta with timing gates off, and the application build succeeds.

The rebuilt application also passes all seven automated RTT cases, each
with complete readback and zero color/full-VRAM verifier differences;
artifacts are `/tmp/pvr-rtt-toposort-20261012/`. This checks the ordering
change through the production renderer as well as the focused tests.

Internal hybrid work bins now use 16x16 pixels instead of 32x32, with matching
CPU bin construction and Metal lookup. These bins accelerate candidate
selection; they are not PVR region tiles, whose semantics remain supplied
through per-pixel masks. The change reduces unrelated triangle checks for
small geometry while leaving edge/depth/shading arithmetic unchanged.
The small-triangle benchmark retains matching color/depth and measures wall
median 16.12 ms, p95 17.47 ms, and GPU p95 11.68 ms on the M1
(`/tmp/pvr-bin16.xml`). The wall gate still fails. In the same run, the
six-layer workload passes both gates (wall p95 8.86 ms, GPU p95 4.83 ms).
All 136 correctness tests pass on ARM64 and x86-64/Rosetta, and the app
build succeeds. These workload-specific gains are not a full-emulator or
all-games performance claim.

All seven production RTT probe cases also pass with 16x16 work bins,
including full-VRAM hybrid/software verification. Preserved artifacts and
ELF hashes are in `/tmp/pvr-rtt-bin16-20261012/report.json`.

Small-triangle timing now separately records CPU preparation, submission
wait, readback, and path-selection counters. A 16x16-bin repeat records
wall p95 17.57 ms, encode p95 4.63 ms, submit/wait p95 13.26 ms, device p95
11.97 ms, and readback p95 0.15 ms. It uses ordered autosort but zero
hardware-visibility bins. Quantiles describe separate distributions and
must not be summed into a purported measured frame time.

A controlled 8x8-bin experiment preserved benchmark output but regressed
wall p95 to 19.61 ms, with encode p95 7.45 ms and GPU p95 11.61 ms. The
six-layer wall p95 also regressed to 22.15 ms, failing its gate. Results
are `/tmp/pvr-bin8.xml`. The 8x8 change was rejected and both CPU/Metal bin
sizes restored to 16. The timing breakdown remains to guide further work;
the experiment does not justify relaxing correctness or frame-budget gates.

Global translucent-order analysis is now performed only when polygon state
permits its sole consumer, the specialized simple-translucency shader.
Ineligible frames retain the normal tile-local ordering proof and shader.
The global-order timing flag is zero when that analysis is skipped rather
than implying a proof was run. Positive-overlap checks also first reject
disjoint integer pixel bounds, consistent with the shader's bounds test.

Two 20-sample timing runs pass both renderer-boundary 60-Hz gates with
matching software-reference pixels/depth. Small-triangle wall p95 is
15.95 ms and 16.63 ms, respectively; GPU p95 is 11.60 ms and 11.65 ms.
Six-layer wall p95 is 8.81 ms and 9.15 ms. XML reports are
`/tmp/pvr-prune.xml` and `/tmp/pvr-prune-repeat.xml`. The second small-triangle
result has almost no margin, so these passes do not establish robust
full-emulator 60-Hz operation. All 136 focused correctness tests pass on
ARM64 and x86-64/Rosetta. Further workload variety, sustained timing, and
hardware accuracy validation remain required.

The rebuilt application passes all seven automated RTT cases after the
analysis pruning, including complete readback and zero color/full-VRAM
verifier differences (`/tmp/pvr-rtt-prune-20261012/report.json`).

A longer post-optimization Seaman run used real SDL2 output (host muted),
full framebuffer emulation, single-thread rendering, and the opt-in late
swap interval. `/tmp/pvr-seaman-post-prune-audio.log` initially shows roughly
1.0x emulation speed. Audio delivery reports progress to 2,649,600 submitted
input frames, 2,594 callbacks, nine underruns/9,216 silent output frames,
and zero dropped input frames with a 2,822-frame buffer. The underrun count
is unchanged across the last nine delivery reports.

However, delivery reports subsequently stop while PVR/emulation logs continue.
Later four-second-emulated windows run around 1.5–1.8x real time, and the log
contains microphone-control `MDRE_TransmitAgain` warnings. The causal link
between those observations is not established. No hybrid rejection was
logged, but this run fails to establish sustained audio/pacing correctness;
the earlier stable interval must not be generalized to the later phase.
The emulator was stopped after capture. Audio/pacing across this transition
requires investigation, separate from the passing renderer microbenchmarks.

### Follow-up: audio delivery through the previously failing interval

`FLYCAST_AUDIO_TRACE_DELIVERY` now also prints `AUDIO_DELIVERY_FINAL` after
SDL closes its output device. This snapshots the cumulative counters and
remaining queued input under the stream mutex, outside the callback. Unlike
push-triggered periodic reports, it can reveal continued callbacks and silence
even when sample submission has stopped. It changes no buffering, scheduling,
or rendering policy. The ARM64 app builds successfully with this diagnostic.

Two fresh, host-muted Seaman runs retained SDL2 output, renderer 7 with Metal,
single-thread rendering, full framebuffer emulation, requested VSync, and the
experimental late-swap interval. Neither reproduced the earlier cutoff:

- `/tmp/pvr-audio-final-trace.log`, without per-frame hybrid logging: 26 speed
  windows span 0.9784–1.0028x. Final submission is 4,610,560 input frames
  (104.55 seconds at 44.1 kHz), with 4,515 callbacks, 13 underruns, 13,312
  silent output frames, zero dropped input frames, and 512 queued frames.
- `/tmp/pvr-audio-final-hybrid-trace.log`, with `FLYCAST_HYBRID_TRACE=1` as in
  the original run: 29 speed windows span 0.9792–1.0030x. Final submission is
  5,272,576 input frames (119.56 seconds), with 5,160 callbacks, 11 underruns,
  11,264 silent output frames, zero dropped input frames, and no queued frames.
  No hybrid rejection is logged. The final periodic report still had eight
  underruns; three more appear in the shutdown summary.

Both owned emulator processes were stopped with SIGINT and exited normally.
These results contradict a deterministic cutoff at 60 seconds under the tested
conditions, but do not identify the cause of the previous failure. The final
summary includes the shutdown interval, so its underrun total should not be
attributed wholly to gameplay. These are boot/idle sequences, not interactive
playthroughs or proof of glitch-free audio. Late-swap tearing, broader scene
coverage, hardware equivalence, and sustained gameplay remain unverified.

### RTT K-value boundary and format isolation

The Dreamcast/Dev.Box System Architecture (1999-09-03), `FB_W_CTRL` register
description at 0x005F8048, specifies that KRGB0555 bit 15 is `fb_kval[7]`.
The K field is bits 15–8 of the register; it does not supply the alpha channel
of ARGB4444 or ARGB1555. The probe now accepts byte-valued `PVR_PROBE_K`
(default zero) and records it in schema 4. The strict reader retains schemas
1–3, verifies the requested K register bits, and incorporates K only in the
KRGB0555 expected word. Six parser tests pass, including synthetic combinations
of four formats, five K values, and two vertex-alpha values. Synthetic fixtures
are reader tests, not rendered evidence.

The live runner now executes fourteen cases: the previous seven plus KRGB0555
with K=1,127,128,255 and alpha=0, and each other 16-bit format with K=255,
alpha=0, and threshold=128. All fourteen pass in
`/tmp/pvr-rtt-kvalue-20261012/report.json`. Every case reports matching color
and all 8,388,608 VRAM bytes between software and Metal, expected interior
words, unchanged guards/padding, and the requested final register values.

Observed quad/background words (576/448 pixels respectively):

| Format and inputs | Quad | Background |
| --- | --- | --- |
| KRGB0555, K=1 or 127, alpha=0 | 0x573d | 0x0000 |
| KRGB0555, K=128 or 255, alpha=0 | 0xd73d | 0x8000 |
| RGB565, K=255, alpha=0 | 0xae7d | 0x0000 |
| ARGB4444, K=255, alpha=0 | 0x0ace | 0xf000 |
| ARGB1555, K=255, alpha=0, threshold=128 | 0x573d | 0x8000 |

This exercises K independently of vertex alpha through actual emulated RTT
writes. No renderer policy changed. All results remain emulator-only; the
shared framebuffer packers mean zero differential errors are not independent
proof of the packer itself. Dither behavior and original-hardware agreement
remain unverified. The register description establishes the K expectation
precisely but does not itself specify a complete dither matrix, so no new
dither golden is inferred from it.

### Phase-resolved dither observations, without a hardware golden

Schema 5 adds `PVR_PROBE_DITHER` (0 or 1) and records the requested dither bit.
The reader collects sixteen interior word histograms indexed by
`(y % 4) * 4 + x % 4`, each covering sixteen pixels inside the constant-color
quad. Dither-enabled reports deliberately return null for both
`expectedInteriorWord` and `interiorPixelsDifferentFromRequestedColor`.
The register description establishes the enable bit but does not independently
establish the matrix, phase, or alpha increment. Repeating the implementation's
matrix in the reader would therefore not supply a hardware expectation.

The runner now executes twenty cases. Fourteen retain the documented
undithered-packing checks. Six additional dither cases cover all four 16-bit
formats plus ARGB4444 alpha=15 and ARGB1555 alpha=127/threshold=128. Their
acceptance scope is explicitly only capture integrity and software–Metal
agreement: expected registers, unchanged guards and padding, written interior,
and zero differential color/full-VRAM errors. The report's
`interiorExpectedColorVerified` remains null for these six cases.

All twenty completed successfully in
`/tmp/pvr-rtt-dither-20261012/report.json`; the directory retains the ELFs,
hashes, build logs, and raw dumps. Seven reader tests pass, including synthetic
phase indexing and rejection of invalid dither fields. No renderer behavior
changed; a source comment now identifies the alpha increment as the existing
GPU-path model rather than an independently verified PVR rule.

For RGB565, the observed interior packed words by phase are:

| y mod 4 / x mod 4 | 0 | 1 | 2 | 3 |
| --- | --- | --- | --- | --- |
| 0 | ae7e | b69e | ae7e | b69e |
| 1 | ae7e | ae7d | b67e | ae7e |
| 2 | ae7e | b69e | ae7e | b69e |
| 3 | b67e | ae7e | ae7e | ae7d |

Every phase has sixteen identical interior observations in this case. Its
allocation SHA256 is
`3963f633743d35b9fae80b8e3cbd0dbd2ba8de7cee1d02dec19438f120760668`.
The alpha-boundary cases expose the model's constant increment: ARGB4444
alpha=15 stores alpha nibble 1, while ARGB1555 alpha=127 at threshold=128
stores alpha bit 1. These are observations to compare against physical
hardware, not evidence that those alpha rules are correct. In particular,
the shared post-render dither and packing path is outside the independent
CPU/GPU comparison boundary.

### Rejected experiment: hardware visibility for disjoint depth ties

The small-triangle workload previously uses zero hardware-visibility bins:
same-depth triangles in a bin fail the conservative strictly separated depth
test, including the two shared-edge halves of a quad. An experiment reused
the existing geometric non-overlap proof to allow ambiguous depth ranges only
when their triangles had disjoint coverage. This admitted all 1,200 bins.
Focused comparisons covered shared-edge versus overlapping equal-depth
triangles at both supported sample offsets (0 and 8); their software color
and depth matched. The full small-triangle benchmark also retained exact
color/depth agreement for every sample.

The experiment was nevertheless reverted because it increased total cost.
Before: `/tmp/pvr-visibility-before.xml`, wall p95 15.768 ms, GPU p95
11.313 ms, CPU encode p95 3.361 ms. Experiment:
`/tmp/pvr-visibility-after.xml`, wall p95 17.174 ms, GPU p95 10.648 ms,
CPU encode p95 5.184 ms, failing the explicit 16.667-ms wall gate. These are
twenty-sample runs of the same workload, not a general statistical claim.
The new eligibility proof and its temporary test were removed; the prior
conservative visibility policy remains. A future attempt should investigate
the per-bin visibility vertex duplication and draw/encoding overhead rather
than assuming that admitting more hardware work improves end-to-end time.

### Batch identical neighboring visibility draws

The conservative eligibility rules remain unchanged. Adjacent visibility bins
in the same row now share one draw only when their complete ordered primitive
index lists are identical. The draw's scissor is the union of those bins, so
each pixel sees the same primitives in the same order. Empty/fallback bins,
different lists, and row boundaries stop a batch; partial right/bottom bins
retain their framebuffer bounds. `Timing.hardwareVisibilityDraws` records
actual calls. `FLYCAST_HYBRID_DISABLE_VISIBILITY_BATCHING=1` restores per-bin
draws for controlled comparison without rebuilding.

`VisibilityBatchingPreservesGapsDifferentListsAndPartialRows` compares complete
software color/depth at both sample offsets on a 65x33 framebuffer, with a
different-list bin or an ineligible equal-depth bin interrupting a row.
`BenchmarksOpaqueVisibilityBatches` checks eight overlapping opaque triangles
at 640x480, comparing every timed sample's buffers after timing. It performs
100 samples and reduces 1,200 visibility draws to 30 without changing the
1,200 eligible bins. This is a submission-overhead workload, not a claimed
Dreamcast-spec throughput workload.

The first 20-sample measurements were noisy: wall p95 8.767 ms before versus
11.698 ms after, then 7.953 ms on a repeat. Three paired 100-sample runs using
the same binary and the disable switch gave these wall p95 results:

| Pair | Per-bin draws | Batched draws |
| --- | --- | --- |
| 0 | 8.644 ms | 7.600 ms |
| 1 | 8.308 ms | 7.200 ms |
| 2 | 8.544 ms | 7.874 ms |

Artifacts: `/tmp/pvr-batch-{off,on}-{0,1,2}.xml`. This is a modest 8–13%
wall-time improvement in that workload, not a large general speedup. GPU p95
does not improve in every pair; the optimization targets submission overhead.
An initial broader timing run overlapped an app build and failed the
small-triangle wall gate (19.444 ms). After compilation finished,
`/tmp/pvr-batch-gates-idle.xml` passes both established 60-Hz gates:
small-triangle wall/GPU p95 14.246/11.100 ms and six-layer wall/GPU p95
7.302/4.229 ms. These remain renderer-boundary results, not gameplay claims.
All 138 correctness tests pass on native ARM64 and Rosetta, and the ARM64
app builds successfully.
The rebuilt app also completes all twenty RTT probe cases in
`/tmp/pvr-rtt-batched-20261012/report.json`, with zero color/full-VRAM
differences. The six dither cases retain their restricted differential-only
acceptance scope; none of these runs establishes original-hardware accuracy.

### Fix clipped opaque visibility inside a work bin

`HardwareVisibilityRespectsClippedBoundsInsideTiles` exposed a concrete
software/Metal disagreement. A red background covers a 32x32 framebuffer;
a nearer green triangle has the same oversized geometry but pixel bounds
x=3..22, y=5..26. These bounds cut through four 16x16 work bins. Previously,
the hardware visibility fragment shader wrote the green triangle's index
outside its bounds because only coarse bin scissors were applied. The compute
shading pass then rejected that candidate by its bounds, leaving a hole instead
of selecting the valid red background. The regression failed at both sample
offsets, including color and depth at pixel zero. Failure log:
`/tmp/pvr-visibility-clip-before.log`.

The visibility fragment shader now reads the selected triangle's integer
pixel bounds and discards out-of-bounds fragments before writing the winner
index. Rejecting only in the later shading pass cannot recover the overwritten
background candidate. No eligibility restriction or software fallback was
added: all four bins still use hardware visibility. The regression passes
with batching enabled and with `FLYCAST_HYBRID_DISABLE_VISIBILITY_BATCHING=1`.
All 139 focused tests pass on native ARM64 and Rosetta; the app builds.

The first post-fix timing run, `/tmp/pvr-clip-gates.xml`, fails the small-triangle
wall gate narrowly: p95 16.799 ms versus 16.667 ms; GPU p95 is 11.288 ms.
The six-layer workload passes at wall/GPU p95 8.929/4.243 ms. This failure
remains evidence of insufficient performance margin, not a reason to restore
incorrect clipping or weaken the gate. The small-triangle case does not use
hardware visibility, so it does not isolate the cost of the added fragment
bounds check. Hardware accuracy and sustained gameplay are still unproven.
After the live probe suite finished, a separate repeat
(`/tmp/pvr-clip-gates-repeat.xml`) also narrowly fails the small-triangle wall
gate at 16.867 ms (GPU p95 12.511 ms); the six-layer case passes at
11.909/8.099 ms. Both failed timing runs are retained. All twenty live RTT
cases completed in `/tmp/pvr-rtt-visibility-clip-20261012/report.json` with
zero software/Metal color and full-VRAM differences; dither acceptance remains
differential-only. Production `makeHybridTriangle` also clips its command
bounds against scissor/region state, confirming this is a relevant input path
rather than solely an artificial test construction.

### Skip the second opaque-reference scan after visibility

The compute shader previously selected the opaque winner, then traversed all
opaque references again during shading solely to reject every non-winner.
The CPU now records an opaque-prefix end per bin, but only when every opaque
reference precedes every non-opaque reference. With deferred opaque shading
enabled, the shader directly shades the selected winner and advances to that
prefix end. A missing winner advances without shading. Mixed/interleaved
lists keep their original traversal. Visibility calculations, interpolation,
shading, clipping, depth tests, and non-opaque ordering are unchanged.

`OpaqueWinnerShadingPreservesMixedListOrderAndNoWinner` covers three list
orders with initial depths allowing or rejecting all input geometry. It passes
both normally and with `FLYCAST_HYBRID_DISABLE_OPAQUE_PREFIX_SKIP=1`, which
retains the original shading traversal for comparisons. All 140 tests pass on
native ARM64 and Rosetta; the ARM64 app builds.

Timing is improved but still lacks a reliable spec-frame margin. The initial
before run (`/tmp/pvr-prefix-before.xml`) was unusually slow: small-triangle
wall p95 28.401 ms and six-layer wall p95 41.452 ms. An initial optimized run
(`/tmp/pvr-prefix-after.xml`) passes both gates at 14.885 ms and 8.378 ms,
respectively. These unpaired results alone are not a defensible speedup ratio.
Three subsequent same-binary paired runs give:

| Pair | Original traversal wall p95 | Prefix skip wall p95 |
| --- | --- | --- |
| 0 | 30.591 ms | 19.794 ms |
| 1 | 30.632 ms | 23.854 ms |
| 2 | 30.000 ms | 22.133 ms |

Artifacts: `/tmp/pvr-prefix-{off,on}-{0,1,2}.xml`. All timed buffers match,
but every one of these timing samples exceeds the 16.667-ms wall target.
A final separate gated run (`/tmp/pvr-prefix-final-gates.xml`) still fails
small-triangle wall p95 at 19.309 ms (GPU 14.500 ms); six-layer wall/GPU p95
14.635/11.003 ms passes. Host timing varies substantially, so these observations
support reduced work in this workload, not guaranteed 60-Hz emulation or a
universal speedup. Failed results are retained alongside passing runs.
The rebuilt app completes all twenty RTT cases in
`/tmp/pvr-rtt-prefix-20261012/report.json`, with zero color/full-VRAM
software–Metal differences. Dither checks retain their differential-only
scope, and original-hardware validation remains absent.

### Reverse eligible opaque visibility and reject conservatively hidden candidates

Compute visibility can now traverse an opaque prefix backward when every
primitive uses GreaterOrEqual depth with depth writes, nonnegative finite
vertex depths, no alternate modifier-area parameter, and supported sample
offsets. Mixed lists and other depth modes retain forward traversal. The first
accepted reverse candidate uses the normal comparison against initial depth;
subsequent candidates must be strictly greater, preserving the original
last-submitted winner on equal computed depth. The hardware visibility path
is unchanged.

Before edge calculations, a reverse candidate can be skipped only when its
conservative depth ceiling is strictly less than the current winner's depth.
For covered geometry the exact nonnegative barycentric weights sum to one,
so vertex maximum depth bounds interpolation. The shader adds a 32-float-epsilon
margin scaled by `max(maximum, 1e-30)` for integer-to-float conversion,
division, products and sums. The CPU limits fixed coordinates to ±2^26 and
frame dimensions to 2^22, with sample offsets 0 or 8, ensuring edge products
and differences fit signed 64-bit arithmetic for this proof. Inputs outside
these optimization conditions use the original traversal, not rejection.
An overflowing positive float ceiling becomes infinity and cannot prune.

`OpaqueVisibilityPreservesTiesNearTiesAndSlopedDepth` tests 64 deterministic
scenes with twelve layers, both sample offsets, varying initial depths, equal
depths, one-float-step neighbors around 1, and sloped depth planes. Initial
depth buffers force compute visibility. It passes against software with the
optimization enabled and with `FLYCAST_HYBRID_DISABLE_REVERSE_OPAQUE=1`.
All 141 focused tests pass on ARM64 and Rosetta, and the app builds.

Three same-binary paired small-triangle runs show lower p95 times, but not
reliable frame-budget compliance:

| Pair | Forward wall/GPU p95 | Reverse wall/GPU p95 |
| --- | --- | --- |
| 0 | 21.684 / 16.387 ms | 17.418 / 11.894 ms |
| 1 | 19.043 / 14.475 ms | 16.663 / 12.201 ms |
| 2 | 20.711 / 15.568 ms | 18.187 / 12.927 ms |

Artifacts: `/tmp/pvr-reverse-{off,on}-{0,1,2}.xml`. These twenty-sample runs
show approximately 12–20% lower wall p95 on this workload; only pair 1's
optimized result fits 16.667 ms, with negligible margin. A final gated run
after adding the coordinate guard (`/tmp/pvr-reverse-final.xml`) still fails
small-triangle wall p95 at 19.094 ms (GPU 14.114 ms). Six-layer wall/GPU p95
15.536/11.414 ms passes. This is a reduction in proven-unnecessary work,
not a claim of completed Dreamcast-spec performance.
All twenty rebuilt-app RTT probes complete in
`/tmp/pvr-rtt-reverse-20261012/report.json`, with zero color/full-VRAM
software–Metal differences. Dither results remain differential-only, and
no original-hardware validation is implied.

### Rejected experiment: prefix-depth early termination

A follow-up experiment supplied a conservative maximum depth for every
remaining opaque prefix, allowing reverse visibility to break out entirely
when the accepted winner was strictly nearer than that maximum. Ceilings
used the same nonnegative-depth/coordinate proof as reverse visibility,
computed in double and rounded outward to float. A separate GPU buffer held
one ceiling per bin reference. All 141 correctness tests passed.

The experiment reduced GPU time, but not enough end-to-end time to justify
the added per-reference metadata and CPU preparation. Three paired runs of
the same binary gave small-triangle wall p95 values:

| Pair | Early exit disabled | Early exit enabled |
| --- | --- | --- |
| 0 | 18.808 ms | 17.471 ms |
| 1 | 18.146 ms | 17.947 ms |
| 2 | 18.878 ms | 18.018 ms |

Artifacts: `/tmp/pvr-early-{off,on}-{0,1,2}.xml`. Enabled GPU p95 was
12.036–12.079 ms versus 13.411–13.619 ms disabled, but CPU encode p95 was
4.097–4.339 ms enabled versus 3.767–4.608 ms disabled. The wall improvement
was only approximately 1–7%, with all runs above the 16.667-ms gate.
The experiment, its buffer, and its disable switch were removed. The existing
reverse scan with individual conservative rejection remains unchanged. This
does not establish a correctness problem with early exit; it rejects this
particular implementation as an insufficient performance tradeoff.

### Exact visibility coverage and host-independent near-depth evaluation

A new different-geometry comparison exposed holes in hardware-assisted
visibility. Two triangles with different slopes covered a 16x16 bin; native
rasterization included samples on an edge that the fixed-point top-left rule
excluded. That spurious candidate overwrote the valid background winner and
was later rejected in compute shading. The original reproducer fails in
`/tmp/pvr-near-visibility-before.log`, including scene 2, pixel 233.

The visibility pass now draws the integer pixel-bounds rectangle of each
triangle (two native triangles) and evaluates the actual triangle's fixed-point
edges, sample offset, top-left ownership, and clipped bounds in the fragment
shader before writing its index. A rectangle is deliberately used instead of
merely filtering native triangle coverage: filtering cannot restore samples
that host triangle rasterization omitted. Bounding rectangles are independent
of the FPU sample offset; that offset is applied only in the exact edge test.
This path is limited to framebuffer dimensions at most 32768 to bound fixed
coordinate construction and native rectangle-position precision. Larger
dimensions retain compute visibility.

Rosetta then exposed a distinct near-depth issue: the software reference could
choose red where native ARM64 and Metal chose green for depths one float step
apart. Margin-free vertex bounds were not enough to prove the order of
interpolated values, and compute fallback alone did not eliminate the host
disagreement. Software depth interpolation now explicitly uses nested `fma`
operations, with the same evaluation in all three Metal visibility/shading
depth calculations. This removes compiler-dependent contraction from the
depth winner decision while retaining the fused ARM reference evaluation for
the tested cases. It is a deterministic model choice, not independent proof
of original-PVR arithmetic.

Hardware depth eligibility now expands vertex depth intervals by a conservative
32-float-epsilon margin, rejecting ambiguous orders to compute visibility.
`VisibilitySelectionPreservesCoverageAcrossDifferentGeometry` covers 64 shape
pairs, both sample offsets, and both wide and one-float-step depth separation
(256 cases). It asserts that wide separation still uses hardware-assisted
visibility and near separation uses compute, then compares all color/depth
pixels. The full 142-test suite passes on ARM64 and Rosetta. Intermediate
Rosetta failures are retained in `/tmp/pvr-exact-coverage-rosetta.log` and
`/tmp/pvr-exact-depth-rosetta.log`; final passing logs are
`/tmp/pvr-exact-final-{native,rosetta}.log`.

The final gated timing run (`/tmp/pvr-exact-final-gates.xml`) still narrowly
fails small-triangle wall p95: 17.135 ms, with GPU p95 11.576 ms. Six-layer
wall/GPU p95 15.632/10.044 ms passes. The opaque-visibility overhead workload
records wall/GPU p95 9.725/5.860 ms. These correctness fixes are retained
without claiming that the performance or hardware-validation goals are met.
All twenty final rebuilt-app RTT cases also pass their stated checks in
`/tmp/pvr-rtt-exact-final-20261012/report.json`, including zero color/full-VRAM
differences; the six dither cases retain differential-only acceptance.

### Exact-coverage live pacing: presentation remains a large bottleneck

Rebuilt app at `751ddc00b`, Seaman USA boot/idle sequence, renderer 7,
`FLYCAST_HYBRID_SOFT=1`, full framebuffer emulation enabled, real SDL2 audio,
host output muted. Verification and late-swap flags were absent. Emulation,
swap, and audio-delivery tracing were enabled. These were fresh processes,
not paused or interactively driven; they are not complete playthroughs.

| Rendering / VSync | 240-vblank windows | Last five emulation speeds | Final underruns / callbacks |
| --- | ---: | --- | ---: |
| Single-thread / on | 12 | 0.6669, 0.6674, 0.6675, 0.6673, 0.6672 | 746 / 2901 |
| Threaded / on | 53 | 0.6675, 0.6668, 0.6695, 0.6658, 0.6666 | 4252 / 13424 |
| Threaded / off | 21 | 0.9984, 0.9995, 0.9994, 1.0016, 0.9999 | 3 / 3754 |

Logs are `/tmp/pvr-seaman-exact-normal-vsync.log`,
`/tmp/pvr-seaman-exact-threaded-vsync.log`, and
`/tmp/pvr-seaman-exact-threaded-nosync.log`. All processes stopped normally.
All reported zero dropped input frames. The no-VSync run submitted 3,841,024
audio frames; its periodic underrun count stayed at two after the early
sequence, with the final third event recorded during shutdown. Its emulation
speed range was 0.9861–1.0102. Reported 120-call mean swap times ranged
0.820–1.379 ms, versus 9.106–15.428 ms with threaded ordinary VSync.
The single-thread run briefly overlapped an idle installed Flycast app;
the two threaded runs did not.

Threading alone does not resolve the slowdown. Removing display waiting
recovers approximately real-time emulation in this sequence without disabling
framebuffer emulation, identifying presentation synchronization as a major
next target. This diagnostic does not establish tear-free presentation,
general game performance, or threaded framebuffer correctness. The upstream
queue explicitly notes a possible framebuffer synchronization/blinking issue,
and the full-VRAM differential verifier is disabled in threaded mode. Do not
turn VSync off by default or weaken framebuffer semantics on this evidence.
Next investigate display wait/queue scheduling while preserving framebuffer
read ordering, rather than assuming shader throughput alone explains 0.66x.

### Render-completion wait attribution

`FLYCAST_PVR_TRACE_RENDER_WAIT=1` now measures the emulation thread's
`renderEnd.Wait()` wall time in 120-call windows. It does not change the
wait or synchronization. These totals include queue delay and any preceding
window presentation, not just GPU execution. The trace is threaded-only;
partial windows are not reported and restarting the app resets its counters.

The rebuilt ARM64 app repeated the preceding threaded comparison with this
trace enabled. `/tmp/pvr-render-wait-vsync.log` reports early mean waits of
30.496 and 31.923 ms, followed by 46.234, 47.068, 46.719, and 46.989 ms.
The otherwise matching VSync-off run (`/tmp/pvr-render-wait-nosync.log`)
reports early means of 9.702 and 9.861 ms, then 16.499–17.500 ms over nine
windows. Final audio underruns were 398/1872 callbacks with VSync and
3/2298 without; these runs differ in duration, so the raw counts are not a
matched-duration rate comparison. Both stopped normally with zero dropped
audio input frames. The host remained muted.

Source inspection explains the coupling: `mainui_loop` processes the PVR
queue through `emu.render()` and then calls the UI driver's `present()` on
the same thread; the OpenGL driver swaps the window there. Full framebuffer
emulation retains the render-completion wait until framebuffer writes are
finished. This evidence supports separating presentation scheduling from
queue progress, not removing the completion wait. Any such change still
needs immutable presentation ownership, ordered framebuffer reads/writes,
shutdown and resize handling, and verification of tear-free output. No
scheduling or default-VSync behavior was changed by this diagnostic.

### Rejected early-GL-flush experiment

The bundled SDL Cocoa backend (`Cocoa_GL_SwapWindow`) waits on its display
link before calling `flushBuffer`. A temporary opt-in `glFlush()` immediately
before `SDL_GL_SwapWindow`, restricted to the software renderer, tested whether
submitting the presentation draw ahead of that wait would recover pacing.
The app links the bundled SDL build. Positive swap interval 1, threaded
rendering, full framebuffer emulation, and real SDL2 audio stayed enabled;
late swap and differential verification stayed disabled, and the host stayed
muted. No render-completion wait was removed.

`/tmp/pvr-preswap-flush-vsync.log` reproduces the slowdown: later emulation
windows include 0.6751, 0.6672, 0.6637, 0.6671, and 0.6676; corresponding
120-call render-wait means remain 46.471–46.861 ms. Audio underruns grow
from 95 to 440 across those reported windows. This does not support retaining
the change. The process stopped normally, the temporary flag and `glFlush`
were removed, and the normal app was rebuilt. No production code from this
experiment remains. It rules out this particular early-submission approach,
not all possible presentation scheduling improvements.

### Owned framebuffer display images

The software renderer now retains framebuffer-read output in an owned,
immutable `PresentationFrame` instead of using a `SoftwareRasterizer` as a
display buffer. The packet owns converted RGBA pixels, dimensions, aspect
ratio, and the captured framebuffer border color. Display and screenshot
consumers retain the packet locally. A later render or producer replacement
cannot mutate its storage, and a borrowed raw Metal surface cannot override
it. Blank-video packets contain the captured border color. Framebuffer reads,
VRAM writes, and render-completion synchronization are unchanged.

This also fixes a metadata mismatch: the GL letterbox clear previously read
live `VO_BORDER_COL`, even though `RenderFramebuffer` receives latched border
state. Framebuffer presentation now uses that captured color. Non-framebuffer
direct presentation is unchanged. Host rotation/filter preferences still use
the existing UI settings.

`PresentationFrame` tests retain an older consumer image while changing the
source pixels, metadata, dimensions, and producer reference. Invalid sizes
and nonfinite/nonpositive aspect ratios are rejected without multiplication
overflow. All 144 renderer tests pass on native ARM64 and Rosetta in
`/tmp/pvr-owned-display-{native,rosetta}.log`. The rebuilt application compares
229 Seaman color buffers and 229 full-VRAM snapshots with zero mismatches in
`/tmp/pvr-owned-display-seaman.log`; this is boot/idle differential evidence,
not display timing or hardware conformance evidence.
All twenty RTT probe cases pass their stated checks in
`/tmp/pvr-rtt-owned-display-20261012/report.json`; the six dither cases remain
differential-only, not independently validated hardware results.

This is a prerequisite for separating display and rendering, not that
separation itself. The shared pointer is currently accessed by the existing
consumer thread; cross-thread publication still needs synchronization. The
threaded framebuffer-read/VRAM ordering concern is not resolved by owning the
output after the read, nor are borrowed GPU surfaces made immutable by this
change. No performance improvement or tear-free scheduling is claimed.

### Opt-in emulation-thread rendering with independent presentation

`FLYCAST_PVR_DECOUPLED_PRESENT=1`, when initialized on desktop macOS with
renderer 7, threaded rendering, and full framebuffer emulation enabled,
executes PVR Render and RenderFramebuffer messages synchronously on the
emulation thread. Present messages remain on the existing bounded UI queue.
The CPU cannot advance past an incomplete framebuffer read or render write,
while the UI can wait for ordinary VSync independently. SH4 rounding mode is
restored after rendering, including on exceptions. The render-completion
event remains in place; its measured wait becomes negligible because the
work has already completed, not because rendering is free.

Completed display images are published through an atomic shared-pointer
mailbox. The UI retains the image selected by Present through its GL upload;
it does not inspect mutable rasterizer buffers or the Metal presentation
pool. The last-frame-clear flag is atomic. A concurrent publication test
checks pixel/metadata consistency and retention across 2,000 replacements
and mailbox clearing. All 145 renderer tests pass on ARM64 and Rosetta in
`/tmp/pvr-decoupled-{native,rosetta}.log`.

The first real-audio Seaman run, with ordinary swap interval 1 and full
framebuffer emulation, recorded 23 emulation windows at 0.9955–1.0097x.
It submitted 4,165,632 audio input frames over 4,068 callbacks with zero
underruns, silence output frames, or dropped input frames, including normal
shutdown (`/tmp/pvr-decoupled-vsync.log`). This is a substantial improvement
over the preceding ~0.67x ordinary-VSync runs. The host stayed muted and no
gameplay input was supplied. A separate null-audio differential run compared
558 color frames and full-VRAM snapshots with zero mismatches
(`/tmp/pvr-decoupled-verify.log`). Full-VRAM verification is permitted here
because the emulation thread itself performs the render and cannot modify
VRAM concurrently with the comparison.

Those two runs preceded the final UI snapshot-pinning adjustment. The first
RTT run also overlapped that rebuild and is exploratory, not final-build
provenance. The runner now accepts `--decoupled`, records the scheduling mode,
and rejects executable changes during a run. Do not confuse its usual
single-thread default with validation of this opt-in mode.

The final pinned-snapshot build passes all twenty rebuilt RTT probes in
`/tmp/pvr-rtt-decoupled-final-20261012/report.json`, with the six dither cases
retaining differential-only acceptance. Its independent real-audio run
(`/tmp/pvr-decoupled-final-vsync.log`) records 22 emulation windows at
0.9942–1.0104x with swap interval 1. Periodic audio reports remain at zero
underruns; the final counter records three underruns/3,072 silent output
frames over 3,912 callbacks, with 4,002,816 submitted input frames and zero
drops. The difference appears between the last periodic report and normal
shutdown; it must not be reported as an entirely underrun-free run. This
reproduces the pacing improvement on the final build without claiming that
all audio-delivery or presentation requirements are solved.

This remains an experiment, not a new default. Changing threaded rendering
or framebuffer emulation underneath the initialized mode is rejected rather
than silently switching ownership. Latest-image publication may omit display
images when the UI falls behind; no PVR render or VRAM write is skipped by
this handoff. Unique-frame delivery, pause/resume, resize, settings changes,
network rollback, longer play, and other games still need validation. No
visual tear-free claim or full hardware-conformance claim follows from the
audio counters or CPU/Metal agreement. The opt-in path is not enabled for
libretro or non-macOS platforms.

### Completed-swap and image-change delivery accounting

`FLYCAST_PVR_TRACE_DELIVERY=1` now records immutable framebuffer sequence IDs
at the return from the UI's OpenGL window swap. This distinguishes publication
from a completed host swap. It is not proof of physical scanout or tear-free
output. Counters separate unique snapshots, skipped sequence IDs, repeat
swaps, and out-of-order swaps; a unit test covers gaps, duplicates, invalid
zero IDs, and backward IDs. All 146 renderer tests pass on ARM64 and Rosetta
in `/tmp/pvr-delivery-content-{native,rosetta}.log`.

Under this diagnostic flag only, the producer also compares each framebuffer
image with its predecessor using exact RGBA vector equality, dimensions,
aspect ratio, and border color. A separate monotonic content ID increments
on each change, including A-to-B-to-A transitions. Thus missing repeated
copies of one image is distinguished from omitting an intermediate changed
image. These comparisons add CPU work and should not be mistaken for a
zero-overhead benchmark. UI rotation/filter preferences and physical display
timing are outside this content comparison.

The first sequence-only sample (`/tmp/pvr-delivery-seaman.log`) delivered
3,682 of 4,038 published snapshots, skipped 355, and ended with one snapshot
not yet swapped. No out-of-order swaps occurred. It recorded 41 final audio
underruns; native UI inspection occurred during that sample, so it is not a
clean uninterrupted benchmark or evidence that UI inspection caused them.

The subsequent exact-content sample (`/tmp/pvr-delivery-content-seaman.log`)
used the opt-in decoupled path, full framebuffer emulation, ordinary VSync,
real SDL2 audio, and muted host output. It published 5,059 snapshots and
completed swaps for 4,678, skipping 381 with no repeats or backward sequence
IDs. All 2,108 distinct consecutive image states reached completed swaps:
`content_published=2108 content_swapped=2108 content_skipped=0`. No snapshot
was pending at the final counter. This supports the claim that the skipped
snapshots in this run were duplicate images, not missing visual changes.

Across 21 emulation windows, speed ranged 0.9771–1.0100x. Final audio totals
were 3,723,264 submitted frames, 3,641 callbacks, five underruns/5,120 silent
output frames, and zero dropped input frames. Four underruns were already
present in periodic reports, so these are not all shutdown events. The
process stopped normally. This remains boot/idle evidence from one game:
consistent glitch-free audio, 60-Hz changing content, pause/resume, resize,
longer play, and original-hardware validation remain open.

### Decoupled renderer teardown and recreation

The opt-in producer runs outside the UI message queue. Cancelling that queue
therefore does not join an in-flight software/Metal render. Renderer teardown
now calls `Emulator::stopAndWait()` for this mode before touching resources:
it stops execution and waits on an existing emulation future even if the
threading setting or running state has changed. Ordinary renderer recreation
remembers whether execution was running and resumes after initialization;
already-paused games are not automatically resumed. The default non-decoupled
teardown path is unchanged.

`FLYCAST_PVR_TEST_REINIT_FRAME=<positive UI frame>` is an explicit one-shot
integration-test hook, active only for the decoupled renderer while running.
It forces the normal graphics/renderer recreation path and emits begin/end
markers. `tools/run-pvr-lifecycle.mjs` drives it at UI frames 480 and 720 with
null audio, full framebuffer emulation, ordinary VSync, color comparison,
and full-VRAM comparison. It requires verified output before recreation and
at least 40 verified full-VRAM outputs afterward, two final renderer delivery
reports (old and new instances), normal shutdown, and an unchanged executable
hash. Timeouts, forced kills, mismatches, or missing resumption fail the run.
This is not a pacing benchmark, a UI-menu interaction test, or proof that
all lifecycle transitions are safe.

The ARM64 app build passes. Both cases pass in
`/tmp/pvr-lifecycle-rendering-20261012/report.json`: 76/197 full-VRAM
comparisons before recreation and 40 after each, with no color or VRAM
mismatches, successful resumed rendering, and no forced shutdown. The
frame-480 case records one skipped image state in its second renderer
instance; frame 720 ends with one content state pending. These verifier-heavy
runs are not evidence of uninterrupted or lossless display delivery across
recreation.

The initial frame-180 attempt is retained in `/tmp/pvr-lifecycle-20261012`.
It resumed and produced 40 matching post-recreation comparisons, but failed
the required precondition because startup was still framebuffer-only and
there were zero PVR comparisons before recreation. Moving the checkpoints
later tests active rendering rather than weakening that precondition. GUI
pause/resume interaction, window resize, stopped-state recreation, and error
injection remain separate validation work.

### Retaining a paused display across renderer recreation

Recreation now carries an owned software display image across destruction of
the old renderer and GL context. The new renderer copies only RGBA pixels,
dimensions, aspect ratio, and border color into its own presentation packet;
it retains no old GL texture, IOSurface, rasterizer, or sequence identifier.
Delivery accounting starts a fresh epoch. A frame marked cleared after game
termination is not eligible for retention. Other renderer backends and the
non-decoupled software path do not use this handoff.

The lifecycle runner's `--paused` mode adds the explicit
`FLYCAST_PVR_TEST_PAUSED_REINIT` hook. It stops and joins emulation before
recreation, captures the current framebuffer image, requires the recreated renderer
to remain stopped, compares the restored capture byte-for-byte, and draws
and swaps that retained image with the new GL resources before explicitly
resuming. The normal paused recreation path does not resume automatically.

Both final-build paused cases pass in
`/tmp/pvr-paused-final-20261012/report.json`: 79 and 204 full-VRAM comparisons
before recreation and 40 after each, without color/VRAM mismatches. Both
emit `paused_reinit running=0 retained=1 drawn=1` before explicit resume.
These checks validate pixel retention and successful drawing/swap calls, not
physical display scanout. The first case still records one skipped image
state after resume under heavy verification; retention does not establish
lossless delivery of every subsequent image. The earlier exploratory paused
run, before the final cleared-frame guard and draw check, is retained in
`/tmp/pvr-paused-reinit-20261012` and is not the final-build result.
The same final executable also passes both running-recreation cases in
`/tmp/pvr-retained-running-20261012/report.json`, with 79/197 full-VRAM
comparisons before recreation and 41/40 after. Its first case likewise
records one skipped image state, so these are lifecycle/correctness checks,
not smooth-presentation benchmarks.

This does not simulate GUI menu interaction or window resizing. Those paths,
termination/error injection, longer play, and independent hardware validation
remain unverified.

### Controlled renderer-failure recovery

The lifecycle runner's `--error` mode enables the explicit
`FLYCAST_PVR_TEST_RENDER_ERROR` hook. In the decoupled software renderer only,
the 64th Process call throws a `RendererException` before parsing that frame.
The regression requires successful color/full-VRAM comparisons before the
fault, normal UI error recovery, a recreated renderer with execution stopped,
no further verified renders during a 500-ms observation interval, zero
published/swapped frames from the replacement, and normal process shutdown.
Timeouts and forced kills remain failures. This does not alter the default
renderer or inject failures without the explicit environment switch.

This exercises propagation from the emulation-thread render call through its
future into the UI's renderer-error handler, including old-renderer teardown
and new-renderer initialization. It does not inject an error during a GPU
command, a partially completed VRAM write, network rollback, or a concurrent
external settings change. Passing it cannot establish those other error paths.

The final ARM64 build passes the fault test in
`/tmp/pvr-failure-final-20261012/report.json`: 63 matching full-VRAM
comparisons precede the injected failure, zero follow recovery, and the
replacement renderer's final publication/swap counters are all zero. Both
paused-recreation regressions also pass on the same executable in
`/tmp/pvr-failure-paused-regression-20261012/report.json`, with 81/197 verified
VRAM snapshots before recreation and 40 after each. The first paused case
still records one skipped image state during the verifier-heavy transition;
these results do not remove that presentation limitation. The app build,
runner syntax check, and diff whitespace check pass.

### Every-frame changing-content cadence probe

`tests/manual/pvr-cadence-probe` submits 660 simple full-screen opaque quads,
each with a different color surviving RGB565 packing. The first 60 submissions
are warmup; the next 600 report progress against KOS VBlank counts. Unlike
Seaman's repeated image snapshots, this workload can expose omitted visual
updates even when emulation and audio keep pace. It is a minimal scheduling
workload, not peak-throughput or original-hardware conformance evidence.

Built with the pinned KallistiOS SDK, its ELF SHA-256 is
`a41e891d119ab04be49d395fed72448500d18890db377f1f76010bab43ce44ec`.
Both real-SDL2-audio runs used the decoupled Metal/software path and full
framebuffer emulation, with differential verification disabled and host sound
muted. Both reported submitted/VBlank pairs 120/119, 240/239, 360/359,
480/479, and 600/599, then reached the 660-submission end marker. The one-count
offset follows from sampling immediately after the last submission. Emulated
VBlank cadence is separate from wall-clock speed and host swap delivery.

| Run | Image states published | Image states swapped | Image states skipped |
| --- | ---: | ---: | ---: |
| Ordinary VSync | 662 | 650 | 12 |
| VSync disabled | 662 | 662 | 0 |

Logs are `/tmp/pvr-cadence-vsync.log` and `/tmp/pvr-cadence-nosync.log`.
These content counts include startup and all 660 submissions, not just the
600-frame measured interval. Current tracing does not identify precisely
which submission was omitted or whether it was in warmup. The ordinary-VSync
run therefore does not establish lossless changing-frame delivery. VSync-off
is a diagnostic comparison, not a tear-free solution. Both apps remained at
the final static image before normal shutdown, for different durations, so
raw total snapshot and audio counts are not matched-duration rate comparisons.
Final audio underruns were three and zero respectively, with zero dropped
input frames in both runs.

A separate null-audio verifier run (`/tmp/pvr-cadence-verify.log`) completed
all 660 submissions and compared 660 color frames and 660 full-VRAM snapshots
with zero mismatches. That run verifies software/Metal agreement, not pacing.
All three processes stopped normally. This probe makes the remaining
presentation limitation reproducible without relying on a complex game scene;
the opt-in scheduling path remains experimental.

### Software presentation UI composition (2026-10-12)

Inspection for a native Metal presenter exposed an existing ordering bug:
`OpenGLDriver::renderDrawData` drew the retained software image and ImGui,
then `present` drew the software image again. That second draw clears the
window, overwriting the composed UI. The driver now draws software backgrounds
before ImGui and records that fact until swap, avoiding the second draw.
Gameplay frames without ImGui still draw in `present`.

`FLYCAST_PVR_TEST_UI_COMPOSITION` is an opt-in readback regression check. It
adds a one-pixel overlay marker after UI drawing, captures the composed window,
and compares it with another readback immediately before swap. The paused
lifecycle hook submits an empty ImGui draw list over the retained game image;
the marker makes this test sensitive to background clearing even without
interactive menu navigation. The error-recovery case checks actual UI drawing.
The readback overhead is deliberately excluded from normal operation.

Both Seaman paused-recreation checkpoints passed on the final executable in
`/tmp/pvr-ui-paused-20261012/report.json`, SHA-256
`c6bb7f5771427890eaa29d70d4687cd5b21cb437096c1c98f271ffb77cef5f15`.
They verified 77/203 full-VRAM snapshots before recreation and 40 afterward,
with zero CPU/Metal mismatches, and compared 4,915,200 composed-window bytes.
The earlier error-menu check passed in `/tmp/pvr-ui-composition-20261012`;
that predates the marker extension and is not a final-binary result. The 147
renderer unit tests also passed on ARM64. These checks establish composition
preservation in the tested paths, not physical scanout, interactive menu/resize
coverage, or improved cadence. Native Metal presentation remains unimplemented.

The regression test was also checked against a negative control: temporarily
removing the `!softwareBackgroundDrawn` guard restored the old second draw.
At checkpoint 480 the test reported `PVR_UI_COMPOSITION match=0 bytes=4915200`
and terminated with `UI composition overwritten before swap`. Raw evidence is
in `/tmp/pvr-ui-negative-20261012/frame-480.log`. The runner rejected the
unexpected early exit; this was not a timeout or a successful lifecycle run.
The old behavior was then removed again before rebuilding the fixed binary.
Both restored checkpoints passed in `/tmp/pvr-ui-restored-20261012/report.json`
with the same fixed-binary hash as above: 80/196 full-VRAM comparisons before
recreation and 41/40 afterward, with no mismatches. The first run still skipped
one content state during verification; this is not a lossless-delivery result.

Native-presentation integration constraint found during source inspection:
the bundled `SDL_Metal_CreateView` converts a window's flags from OpenGL to
Metal and calls `SDL_GL_UnloadLibrary`. It is not an independent overlay to
bolt onto the active GL window. A native presenter must own the window/UI
backend lifecycle together, including ImGui texture ownership; this checkout
does not currently include an ImGui Metal backend. Preserve the existing
fully converted software image as its input rather than bypassing framebuffer
semantics to reuse an earlier Metal accumulation surface.

### Native Metal UI prerequisite

The matching Dear ImGui v1.92.6 Metal backend is now vendored from
`https://github.com/ocornut/imgui/tree/v1.92.6/backends`, with its upstream
MIT license in `core/deps/imgui/LICENSE.txt`. Backend implementation and header
SHA-256 values are, respectively:
`8aba9bfdb2ecf85f7c7c1dd9af980f0f296f51324f65decdcc96d913dd015788` and
`76dccde7cb59b39e697e0ba850ca27f2df036bf758cd5717274e4264407bbb04`.

An Apple desktop-only, separately requested test target compiles this backend
with Flycast's actual ImGui core, CJK extensions, and FreeType loader:

```sh
cmake --build build-renderer-tests --target flycast-metal-ui-tests -j8
build-renderer-tests/tests/flycast-metal-ui-tests
arch -x86_64 build-renderer-tests/tests/flycast-metal-ui-tests
```

It renders an opaque rectangle and text over a known blue background, checks
the GPU texture readback, changes target dimensions, and destroys/recreates
the backend. The target is excluded from normal builds. This is an offscreen
host-UI integration test, not an emulator-window presenter, a Dreamcast
accuracy check, or a pacing benchmark. No default renderer selection changes.

The offscreen test passes on ARM64 and x86-64 under Rosetta, and on ARM64
with `MTL_DEBUG_LAYER=1` (Metal API validation enabled). Logs:
`/tmp/pvr-metal-ui-native.log` and `/tmp/pvr-metal-ui-rosetta.log`.
The four rendered cases cover two sizes in each of two backend lifetimes.
They check a red rectangle, an untouched blue pixel, and non-background
pixels in the text region (font atlas sampling); they do not assert a
platform-independent font glyph golden image.

The local universal test build initially selected Intel-only libpng for its
FreeType dependency. The build cache was corrected to use
`/tmp/pvr-metal-ui-libpng-universal.dylib`, assembled with `lipo -create` from
the installed Intel and ARM libpng libraries. No system library was modified.
Other hosts need their own architecture-compatible FreeType dependencies;
that temporary path is not part of tracked build configuration. Existing
deployment-target warnings from those host libraries remain and this run does
not establish compatibility with older macOS versions.

The next native-presentation component is
`core/rend/soft/MetalPresentationFrame.mm`: a UI-thread upload of the immutable,
fully converted `PresentationFrame` into an RGBA8-unorm Metal texture. It
does not sample VRAM, reinterpret RGB565, apply gamma conversion, or use the
earlier hybrid accumulation surface. Each upload creates a separate texture;
the future presenter must retain it until its command buffer completes.
Texture pooling and native drawable scheduling are not implemented here.

The offscreen test additionally uploads odd-width and 640-pixel-wide patterns
whose RGBA channels differ, replaces and destroys the CPU packets, and checks
every byte of the first retained GPU texture. The UI composition cases now
draw an uploaded blue framebuffer over a green clear color before drawing
the red overlay and text. This tests the upload/composition interface, not
just ImGui over a render-pass clear. The helper is currently built by the
isolated test target only; the emulator's live presentation path is unchanged.
Both tests pass on ARM64 with Metal API validation enabled and on x86-64
under Rosetta. Raw logs are `/tmp/pvr-metal-frame-native.log` and
`/tmp/pvr-metal-frame-rosetta.log`. These results establish the tested upload
and composition behavior, not sustained frame rates or drawable delivery.

The uploader rejects either dimension above 16,384 before allocating a Metal
texture. This is a conservative supported desktop-path limit based on
[Apple's feature tables](https://developer.apple.com/metal/capabilities/),
not a statement of every newer GPU's maximum. The PVR model itself is unchanged.
Tests exercise oversized width and height with Metal validation enabled, and
a test-only allocation stub verifies that a failed texture allocation returns
no texture without attempting an upload or exhausting real GPU memory.
All four Metal UI tests pass in both architecture slices after a clean
recompilation of the test source. Final evidence:
`/tmp/pvr-metal-limits-rebuilt-native.log` (Metal validation enabled) and
`/tmp/pvr-metal-limits-rebuilt-rosetta.log`. Earlier logs with unequal test
counts came from a source edit during universal compilation and are superseded.

### Native drawable baseline probe

`flycast-metal-window-probe` is a separately built manual SDL/Metal executable.
It opens a resizable native Metal window, uploads 120 independently owned
converted framebuffer packets, and draws them through a nearest-sampled
full-window triangle. It does not instantiate the emulator or its menus.

```sh
cmake --build build-renderer-tests --target flycast-metal-window-probe -j8
MTL_DEBUG_LAYER=1 build-renderer-tests/tests/flycast-metal-window-probe
```

The baseline enables drawable VSync, allows three drawables, submits with
`presentDrawable`, and waits for GPU completion before the next submission.
The presented callback logs `presentedTime`; a zero timestamp is counted as
skipped, not successful delivery. It requires all 120 callbacks and no skipped
frames for success, with a two-second callback drain. Closing the window or
getting an unavailable drawable fails the run. macOS older than 10.15.4 is
rejected because the presentation callbacks are unavailable there.

This isolates host presentation from PVR work and audio. It does not prove
physical scanout, color accuracy of the composed window, sustained gameplay,
or correct integration with Flycast's UI/window lifecycle. A baseline failure
must not be hidden by counting GPU command completion as displayed frames.
Serial baseline runs on the development M1 received all 120 callbacks but
reported two skipped startup frames on ARM64 (IDs 0 and 1), and one under
Rosetta (ID 0). Both therefore fail the lossless-delivery gate. Raw logs:
`/tmp/pvr-metal-window-serial-native.log` (Metal validation enabled) and
`/tmp/pvr-metal-window-serial-rosetta.log`. Executable SHA-256 was unchanged
before and after both runs:
`3e35879d42083973ff8766e1fd6da60f19432351c1250a2ebc7efc71ce651743`.
Earlier exploratory logs are not this serial comparison. The next pacing
experiment must keep the skipped-frame criterion rather than simply exclude
the observed startup losses after the fact.

The probe accepts `--paced` to use
`presentDrawable:afterMinimumDuration:1.0/60.0`. The SDK describes this as
delaying presentation until the previous frame's actual presentation time is
known; it does not guarantee that a drawable is delivered. Run two alternating
pairs on one unchanged binary with:

```sh
node tools/run-metal-window-probe.mjs \
  build-renderer-tests/tests/flycast-metal-window-probe /new/output/directory
```

The runner preserves raw streams, checks all IDs/callback counts, rejects
zero-time frames as skips, validates monotonic nonzero presentation times,
and verifies the binary hash before/after every process. It reports intervals
between delivered callbacks separately from the missing IDs. Timeout or
unexpected termination invalidates a capture; a valid capture with skips
still fails the overall gate after collecting all four cases.

In `/tmp/pvr-metal-pacing-20261012/report.json`, immediate runs skipped IDs
0/1/2 and 1 respectively; both paced runs skipped IDs 0/1. Thus every run
failed lossless delivery. Delivered-interval medians were approximately
16.666 ms in all four runs, but maxima were approximately 50, 33.3, 116.7,
and 50 ms in execution order. Minimum-duration presentation alone is not
a fix. Binary SHA-256:
`2db871a47155d65bfa5b72766906d8a653707358d76fb306696db6190bbd6880`.
All runs enabled Metal API validation and included startup; no warmup frames
were silently removed. This evidence motivates explicit drawable-readiness
and skipped-presentation handling before native emulator integration.

The probe's `--acknowledged` mode waits for each drawable's presentation
callback, retrying the same image ID after a zero-time callback. It bounds
retries to eight across the run and callback waits to two seconds per attempt,
while pumping window events. It still uses minimum-duration presentation.
This is a diagnostic serialization experiment, not the intended emulator
producer policy: blocking the emulation thread on every display callback
would compromise audio and emulated timing.

The comparison runner also accepts `--acknowledged` as its last argument to
alternate immediate and acknowledged runs. It distinguishes skipped attempts
from image IDs that never reached a nonzero presentation time, so a retry
cannot silently erase the underlying failed attempt.

In `/tmp/pvr-metal-ack-comparison-20261012/report.json`, both acknowledged
runs retried ID 0 once and delivered all 120 IDs. Their median delivered
intervals were approximately 33.332 ms, with p95 values of 33.332 and 49.998 ms.
The immediate runs each lost ID 0 and had approximately 16.666 ms medians.
Thus serialization fixes the observed loss but fails the 60 Hz objective;
it must not be promoted as the performance solution. Binary SHA-256:
`8355beaa83dd3ab64cdd2cec7ab25add441467b823bbba890fc2f5a018255e0f`.
The first exploratory acknowledged run was slower still (roughly 50 ms
intervals), reinforcing that this approach lacks adequate cadence.
The next experiment should distinguish startup readiness from steady-state
submission instead of waiting on every displayed image.

The `--startup-ready` mode now retries/waits only for image ID 0, then returns
to minimum-duration submission without waiting for each presented callback.
The manual probe now submits 660 images; the comparison runner recognizes
both historical 120-image and current 660-image captures, retains skipped
attempts separately from missing image IDs, and reports every delivered
interval above 20 ms with its adjacent image IDs. No startup interval is
excluded. Use `--startup-ready` as the runner's final argument to compare
against immediate presentation on the same binary.

The short 120-image comparison in `/tmp/pvr-metal-startup-20261012/report.json`
delivered every ID in both startup-ready runs, with approximately 16.666 ms
medians but maxima of 66.664 and 49.998 ms. A subsequent 660-image comparison
in `/tmp/pvr-metal-startup660-20261012/report.json` also delivered every ID in
both startup-ready runs. They retried ID 0 once and twice, respectively. Their
median and p95 intervals were approximately 16.666 ms. Both had approximately
50 ms between images 0 and 1; the first also had a 33.332 ms gap between
images 284 and 285. The immediate controls each lost ID 1.

The extended comparison's unchanged executable SHA-256 is
`8edb8a5a36eaf42b9c6b9ca8985e9f1c2586926a59e0a2560a5c321cd1bf2d6a`.
All four runs used Metal API validation. This establishes lossless image-ID
delivery in two short native-window runs, not uninterrupted 60 Hz or real-game
performance. It supports testing startup readiness in the live presenter;
frame ownership, UI composition, audio, and emulator pacing still require
integrated verification. The comparison's overall gate remains failed because
its immediate controls lose images.

### Shared Metal framebuffer drawing component

The probe now uses `createMetalPresentationPipeline` and
`encodeMetalPresentation` from `MetalPresentationFrame.mm`, rather than a
private shader copy. The encoder takes a caller-owned render pass and viewport;
it leaves clearing, overlays, command submission, and drawable ownership to
the frontend. It currently supports nearest sampling and unrotated images.
Five offscreen tests pass on ARM64 with Metal validation and under Rosetta,
including exact RGBA/BGRA render-target readbacks, a 2x2 asymmetric image
scaled into an offset viewport, correct row orientation, and untouched border
pixels. Logs: `/tmp/pvr-metal-shared-native.log` and
`/tmp/pvr-metal-shared-rosetta.log`.

The repeated native-window comparison in
`/tmp/pvr-metal-shared-window-20261012/report.json` weakens the earlier startup
readiness result: the first startup-ready run delivered all 660 IDs, but the
second lost ID 1 even after ID 0 was acknowledged. That second run had a
roughly 100 ms interval from ID 0 to ID 2, and a roughly 33.3 ms interval from
ID 261 to ID 262. The immediate controls lost four IDs and zero IDs,
respectively, demonstrating run-to-run variability. A single successful
presentation must not be treated as a reliable readiness guarantee.
Unchanged executable SHA-256:
`ca0e47ad5a1d562301c4c0f77757559f9dea2e504c4a48c6e262265f27d0aa07`.
The reusable component is still built only by the isolated tests/probe;
native emulator window integration and reliable delivery remain unfinished.

The shared Metal presentation shader now forces output alpha to one, matching
the software renderer's OpenGL `GL_TEXTURE_SWIZZLE_A=GL_ONE`. Uploaded
framebuffer alpha remains byte-exact data; it must not become window-compositor
transparency. The asymmetric RGBA/BGRA drawing test now uses source alpha
values 0, 1, 128, and 255, requiring unchanged RGB and opaque display alpha
for every pixel. All five tests pass on ARM64 with Metal validation and under
Rosetta (`/tmp/pvr-metal-alpha-native.log` and
`/tmp/pvr-metal-alpha-rosetta.log`). This checks presentation parity, not an
additional claim about PVR hardware alpha behavior.

The composition regression now draws the uploaded framebuffer through the
shared native presentation pipeline, then ImGui through its own pipeline in
the same render encoder. It no longer substitutes an ImGui background image
for the presentation shader. A zero-alpha blue framebuffer must remain opaque,
hide the green render-pass clear, and retain the red overlay and font pixels
after target resizing and backend recreation. All five tests pass on ARM64
with Metal validation and under Rosetta: `/tmp/pvr-metal-composed-native.log`
and `/tmp/pvr-metal-composed-rosetta.log`. This checks pipeline-state handoff
and composition offscreen; it does not extend the native-window delivery claims.

### Experimental Metal UI driver implementation

`core/rend/soft/MetalUiDriver.mm` implements the ImGui driver interface around
the tested framebuffer upload/drawing and upstream Metal ImGui components.
It accepts a frontend-owned CAMetalLayer, a selected-frame provider, and a
presentation observer. It draws the packet's border and aspect-fitted image
before ImGui, owns UI textures, and marshals drawable callbacks through a
shared queue to the UI thread. Completion blocks retain only that queue and
the immutable packet, not the driver or emulator renderer. Zero presentation
times remain visible to the observer as skips.

The default-off `FLYCAST_BUILD_METAL_UI_DRIVER` CMake option compiles this
prototype; it does not select a runtime renderer or replace the OpenGL window.
The frontend must create/configure the layer, update its drawable size, shut
down the previous ImGui backend, and connect the selected-frame provider and
completion accounting before invoking the factory. Current SDL display metrics
have no Metal drawable-size branch, so that integration remains required.

This driver is not ready for runtime adoption: rotation and display filtering
options are not connected, and startup/skip recovery is not implemented.
Nearest-sampled UI textures were initially rejected; the per-texture sampling
extension described below removes that integration blocker. GPU command completion
is used to bound in-flight texture ownership but is not reported as actual
presentation. Live construction, resize, menus, teardown, audio, and cadence
must still be exercised through the real frontend.

The ARM64 application builds and links with the compile option enabled.
The initial driver compile exposed Cocoa/PVR header name collisions (`RGBColor`
and `Rect`); removing the unnecessary PVR header and using the common
`FlycastException` avoids those collisions while keeping frontend error handling.
The driver itself has not yet been constructed by a live frontend.

The unchanged active OpenGL path passes the injected-error regression in
`/tmp/pvr-metal-driver-gl-regression-20261012/report.json`: 63 full-VRAM
CPU/Metal comparisons before the injected error, zero afterward, a paused
replacement renderer with zero publications, and successful UI-composition
checks. Application SHA-256:
`a33a97831fad46497dd23bb30a78bd855f0f9471a3e0cd4532de193f9d1b5b70`.
The computer remained muted. This verifies compilation/linking and the tested
existing path only; it supplies no native-driver lifecycle or performance claim.

### Standalone native driver lifecycle probe

`flycast-metal-driver-probe` now constructs the actual Metal UI driver in an
SDL Metal window, submits 120 immutable framebuffer packets behind ImGui,
resizes from 640 x 480 to 800 x 600 after submission 60, and creates, looks up,
and deletes a UI texture. It checks that all 120 observer callbacks arrive in
sequence on the UI thread and destroys the driver before the ImGui context
and SDL view. It is separate from the emulator frontend and uses default UI
styling; it does not test game execution, input, audio, or pixel readback.

Both ARM64 and Rosetta runs with Metal validation completed with 120 callbacks
and no lifecycle failure. Both also recorded five skipped presentations:
ARM64 sequences 1, 2, 58, 59, 60; Rosetta sequences 2, 57, 58, 59, 60.
The cluster near the resize checkpoint warrants explicit resize-delivery
testing; these observations alone do not establish its cause. Exit success
means lifecycle success, not lossless delivery or sustained 60 Hz.
The five supporting offscreen Metal tests also passed on both architectures.

Evidence: `/tmp/pvr-metal-driver-probe-native.log`,
`/tmp/pvr-metal-driver-probe-rosetta.log`,
`/tmp/pvr-metal-driver-support-tests.log`, and
`/tmp/pvr-metal-driver-support-rosetta.log`.
Universal probe SHA-256:
`d60233af0133fa2656d0335aaf6f1496e57011721feb833f61d7d39978a56620`.
The initial compile failure was a missing explicit Metal framework header in
the probe, now fixed. Frontend integration and reliable delivery remain open.

The probe was then extended to 660 submissions and given a `--no-resize`
control. Four serial ARM64 runs with Metal validation alternated fixed-size
and resize modes using one unchanged universal binary (SHA-256
`6974983f746189df4e279680a1b721b622213eee546ba50f04ed4d52b2d586bf`).
All had 660 ordered callbacks and no lifecycle failure:

| Run | Skipped sequences | Maximum positive callback gap |
| --- | --- | --- |
| Fixed 1 | 1–5 | 49.997 ms (6 to 7) |
| Resize 1 | 1, 2, 57–60 | 83.329 ms (56 to 61) |
| Fixed 2 | 1, 2 | 16.670 ms |
| Resize 2 | 1, 2, 58–60 | 66.664 ms (57 to 61) |

Logs are `/tmp/pvr-driver-{fixed1,resize1,fixed2,resize2}.log`.
Median and p95 positive callback intervals were approximately 16.666 ms in
all four runs. Resize 1 also had a 33.332 ms gap from 32 to 33; resize 2 had
one from 374 to 375. Fixed-size runs lost no sequences after startup in this
sample. Repeated resize runs lost queued sequences near the size transition;
this supports treating resize delivery separately from steady-size delivery,
not claiming that all pacing failures are solved. Neither mode is a lossless
end-to-end emulator result, and the long gaps remain part of the evidence.

### Frontend Metal display metrics prerequisite

The Apple SDL display-metrics path now queries `SDL_Metal_GetDrawableSize`
for Metal windows, before the existing Vulkan/OpenGL branches. Logical window
dimensions still determine point scaling. This avoids treating logical points
as framebuffer pixels on high-DPI Metal windows; it does not select the native
driver or create a Metal window in the emulator yet.

The ARM64 app builds with this change. The existing OpenGL injected-error
lifecycle regression passes in
`/tmp/pvr-metal-metrics-gl-regression-20261012/report.json`: 63 full-VRAM
comparisons before the fault, zero afterward, and a replacement renderer with
zero publications/swaps. Application SHA-256:
`d543542b5293c1b823b41c6402de306eb570a9cc41a9c6b05481885e3b31b24b`.
This checks the existing path for regression; live emulator Metal metrics and
high-DPI behavior remain to be validated after native-context integration.

### Selected-packet frontend handoff

`selectedPresentation(Renderer*)` now exposes the immutable packet accepted
by the software renderer's `Present()` on the UI thread. Unlike the retention
API, it does not acquire the latest producer publication. The existing GL draw
path uses the same accessor. Retaining the packet does not keep the renderer
alive; future native completion wiring must not capture a raw renderer across
teardown. That completion wiring is not implemented by this change.

The ARM64 app builds. Seaman paused-recreation checks at UI frames 480 and 720
pass, including retained-image equality and UI composition, with respectively
82/197 full-VRAM comparisons before recreation and 40 afterward, all matching.
Evidence: `/tmp/pvr-selected-frame-seaman-20261012/report.json`.
Application SHA-256:
`3ea7f4ef9594641fa5325281cbfbe6a47e85ec3c71157704a8f63ec7cc133735`.
These null-audio checks are not cadence or audio-quality measurements.

An earlier run with the finite cadence ELF passed frame 480 but timed out at
720 (`/tmp/pvr-selected-frame-regression-20261012/frame-720.log`). The latter
recorded successful retained-image drawing and recreation, then no new content
publications; it could not satisfy the required post-recreation comparisons.
That run is not a pass. Use continuously rendering content for this lifecycle
gate rather than assuming the 660-submission probe lasts through both points.

### Per-texture Metal UI sampling

Flycast's VMU overlay uploads its LCD image with nearest sampling. The native
driver now honors that request instead of throwing. A small, explicitly marked
Flycast extension to the imported ImGui Metal backend tracks nearest overrides
by texture ID and supplies a per-draw shader flag. Unregistered textures,
including the font atlas, retain the backend's linear sampling. The driver
clears an old override when replacing or deleting a named texture; backend
context destruction releases the registry. The imported backend is therefore
no longer byte-identical to upstream; the original import hashes above are
provenance, not hashes of the current modified source.

The new offscreen test scales a two-color texture and checks every output
pixel for exact nearest texel colors, verifies blending with linear sampling,
then clears the override and verifies linear blending returns. All six Metal
tests pass with validation on ARM64 and Rosetta:
`/tmp/pvr-metal-sampling-native.log` and
`/tmp/pvr-metal-sampling-rosetta.log`.
The live driver's 660-packet fixed-size probe now uploads a nearest-sampled UI
texture and passes lifecycle checks with two skipped presentations
(`/tmp/pvr-metal-sampling-driver.log`). This does not yet validate the actual
VMU overlay in the emulator frontend or establish lossless frame delivery.

The full ARM64 application subsequently built with the sampling extension
compiled in. Its existing OpenGL injected-error regression passes:
`/tmp/pvr-metal-sampling-app-regression-20261012/report.json`, with 63 matching
full-VRAM comparisons before the fault, zero afterward, and zero publications
or swaps from the paused replacement renderer. Application SHA-256:
`3695ae4bc10aef3c6be1cb68ae9333f87e2850d631fa443a8f0bc3686236a301`.
This confirms application linkage and existing-path recovery, not native Metal
frontend activation. The build still warns that local libpng/libomp binaries
target newer macOS versions than the application's deployment target; it is
not evidence of compatibility with older macOS releases.

### First opt-in native emulator context

The default-off build option now includes an experimental SDL Metal graphics
context. With renderer 7 and `FLYCAST_PVR_NATIVE_METAL=1`, it creates the Metal
window/view, manages drawable-size updates and ImGui teardown, and provides the
software renderer's selected immutable packet to the native driver. Software
renderer initialization skips GL resource creation in this context.

This first integration requires threaded rendering, framebuffer emulation,
`FLYCAST_PVR_DECOUPLED_PRESENT=1`, nearest display filtering, and no rotation.
It uses the driver's fixed 60 Hz presentation request; display filtering,
rotation, configurable pacing, minimize/recovery, and late-callback accounting
across renderer epochs are not complete. Callbacks log packet IDs and times
without retaining or dereferencing a renderer. Existing GL swap counters are
not native presentation counters. The default OpenGL path remains unchanged.

Run the bounded integration smoke test with:

```sh
node tools/run-pvr-native-metal.mjs APP CADENCE_ELF NEW_OUTPUT_DIRECTORY
```

The corrected first run passes in
`/tmp/pvr-native-context-serial-20261012/report.json`: native context activation,
660 matching full-VRAM CPU/Metal comparisons, 791 presentation callbacks,
zero zero-time callbacks, all 660 probe IDs seen with positive presentation
times, normal exit, and no forced termination. The 600 measured IDs have
first-presentation gap median/p95 approximately 16.666 ms, but maximum
99.995 ms and 20 gaps over 20 ms. Thus even this lossless sample is not a
sustained frame-rate pass. It uses null audio and verification overhead; it
does not demonstrate playable Seaman, audio quality, or hardware conformance.

Application SHA-256:
`9ac77bf6226578dbb65199b84c2342bf052f6d9f8d7e75580bafeaa59f37995e`.
The same binary passes the existing OpenGL injected-error regression in
`/tmp/pvr-native-context-gl-regression-20261012/report.json`.

The initial native test at `/tmp/pvr-native-context-first-20261012` timed out
because the runner omitted serial-console settings needed to receive the ELF
end marker. It recorded 660 matching VRAM comparisons but is not a passing
test. The corrected runner explicitly enables serial output; no application
binary change was needed between these two native runs.

### Native presentation with verifier disabled and SDL audio

The native runner now accepts `--timing`. This disables Metal validation and
CPU/Metal verification, selects SDL2 audio, and records final audio counters,
missing frame IDs, and first-presentation gaps for measured IDs 60–659.
`integrationPassed` and exit success mean the test ran to completion, not that
frame delivery or audio passed a performance gate. `performanceValidated`
remains false. The computer remained muted while the SDL audio backend ran.

Two serial runs of the same application hash above produced:

| Run | Missing measured IDs | Zero-time callbacks | Audio underruns | Long measured gaps |
| --- | --- | --- | --- | --- |
| Timing 1 | None | 0 | 1 (1024 silence frames) | One, 33.331 ms |
| Timing 2 | 199, 426 | 0 | 0 | Four, maximum 33.332 ms |

Both median and p95 gaps were approximately 16.666 ms. Logs/reports are in
`/tmp/pvr-native-timing1-20261012` and `/tmp/pvr-native-timing2-20261012`.
Their initial report schema called the integration field `passed`; it has
since been renamed `integrationPassed` to avoid confusing it with performance.
Neither run simultaneously establishes lossless cadence and clean audio.
Missing IDs with no skipped drawable callbacks point to a loss before native
presentation, but producer/selection tracing is still needed to locate it.
This simple probe does not establish Seaman performance or audio quality.

### Native producer/selection trace

`--stages` adds the existing publication/selection trace to the native runner.
It validates those records with `analyzeStages`, reports publication-to-selection
latency, and joins each missing frame ID to its publications, selections, and
native callbacks. It deliberately does not subtract native `presentedTime`
from the producer's steady-clock timestamps: they are different clock domains.
The existing three strict stage-parser tests pass.

Two serial `--timing --stages` runs of the unchanged application are recorded
in `/tmp/pvr-native-stages1-20261012` and `/tmp/pvr-native-stages2-20261012`.
Both completed normally with zero SDL audio underruns and one zero-time native
callback. The first missed IDs 309 and 593; each had one publication, zero
selections, and zero native callbacks. The second delivered all IDs. Both
had one approximately 33.332 ms measured first-presentation gap.

For ID 309, sequence 324 was published at steady time 417013859220 us;
sequence 325 (ID 310) replaced it 7.585 ms later. The next selection accepted
325, after the previous selection had accepted 323 (ID 308). ID 593 shows the
same replacement-before-selection pattern. This directly locates these losses
in the latest-only producer/UI handoff, before Metal submission. It does not
prove all future losses have this cause or that an arbitrary queue will solve
pacing; the previously rejected shallow FIFO remains contrary evidence.
Publication-to-selection p95 was 7.004 ms and 4.455 ms in these two traced runs.

### Opt-in distinct-image ordered handoff experiment

`FLYCAST_PVR_ORDERED_PRESENT=1` (runner `--ordered`) enables a three-image
FIFO only in the native Metal context. Consecutive images with identical
dimensions, aspect, border, and pixels are coalesced before entering the queue.
Overflow discards the oldest queued image and increments the reported
`PVR_ORDERED_FINAL dropped` counter. The producer does not wait for display;
queue access uses a mutex. Empty-queue presentations retain the last selected
image so native callbacks and UI rendering continue to be serviced. The
latest-only mailbox remains the default and still supplies lifecycle retention.

All 149 renderer tests pass on ARM64 and Rosetta, including ordering,
coalescing, overflow, clearing the epoch, and changed-metadata tests:
`/tmp/pvr-ordered-tests-native.log`, `/tmp/pvr-ordered-tests-rosetta.log`.
The ARM64 application builds. Four serial runs used the same application hash
`113acb340f95030889307210ac2d4a5c55bff56aee3b9d539b7daf0144c46e4c`:

| Run | Missing IDs | Queue overflow | Audio underruns | Gaps over 20 ms |
| --- | --- | --- | --- | --- |
| Control 1 | None | Not applicable | 2 | 1 |
| Ordered 1 | None | 0 | 1 | 1 |
| Control 2 | None | Not applicable | 1 | 2 |
| Ordered 2 | None | 0 | 0 | 2 |

Reports are `/tmp/pvr-ordered-{control1,test1,control2,test2}-20261012/report.json`.
Long gaps were approximately 33.332 ms; all median/p95 gaps were approximately
16.666 ms. The first pair had zero skipped native callbacks; the second pair
had one each. Ordered 1's underrun was already present at the first periodic
audio report and did not increase afterward. These short runs do not establish
a practical improvement over the controls, which also delivered every ID.
The experiment remains disabled by default, not a performance solution.

### Seaman native/OpenGL boot and idle samples

`tools/run-pvr-game-timing.mjs APP GAME NEW_OUTPUT_DIRECTORY` performs serial
45-second native and OpenGL samples with the same software/hybrid renderer,
threaded EFB conversion, SDL2 audio, nearest filtering, and VSync. It disables
verification/Metal validation and the ordered-queue experiment, preserves the
application hash, saves separate stdout/stderr, and reports emulation-speed
samples and final audio counters. It stops each application gracefully after
the wall-clock duration and rejects early exits or forced termination.

Seaman samples in `/tmp/pvr-native-seaman-timing-20261012/report.json` used
application SHA-256
`113acb340f95030889307210ac2d4a5c55bff56aee3b9d539b7daf0144c46e4c`.
Both exited normally. Native sampled speed ranged from 0.9854 to 1.0105 times
emulated time, with three audio underruns (3072 output silence frames), 2549
presentation callbacks, and one zero-time callback at sequence 1. OpenGL
sampled speed ranged from 0.9827 to 1.0049, with nine underruns (9216 output
silence frames). Neither dropped input audio frames.

Native's first periodic audio report had zero underruns, its second had two,
and later periodic reports remained at two; the final counter was three.
Thus the problem is not exclusively shutdown accounting. OpenGL underruns
increased during the sample. One pair is insufficient for a comparative
performance conclusion: startup durations differed, these were equal wall-time
windows rather than synchronized game-state captures, and instrumentation is
not free. No input was driven and no visual screenshot was captured. These
are unattended boot/idle samples, not a Seaman playthrough, an audible-quality
assessment (the computer remained muted), or proof of hardware accuracy.

The runner now pins `config:aica.BufferSize=2822` (the buffer size already
observed in the first pair) and accepts `--reverse` to run OpenGL first.
It also retains the last periodic audio record separately from the final one.
The reversed pair at `/tmp/pvr-native-seaman-reverse-20261012/report.json`
used the same application hash and completed normally: OpenGL had zero
underruns, native had two (2048 silence frames), and neither dropped input
audio. Native's final periodic report still showed zero underruns; the final
record showed two with zero queued input frames. This does not distinguish
late-run starvation from shutdown-related silence and must not be relabeled
as zero underruns. Native had 2589 callbacks and one skipped callback.
Both paths again sampled approximately 1x emulated speed. The reversal of the
apparent audio advantage means these samples do not establish a native Metal
performance win. No buffer-size increase or audio-backend implementation
change was made to improve the reported numbers.
