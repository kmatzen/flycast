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
