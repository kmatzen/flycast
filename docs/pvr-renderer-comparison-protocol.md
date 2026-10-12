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
