# PVR software and hybrid renderer status

This work contains two distinct renderers with different platform support and
different standards of evidence.

| Renderer | Implementation | Platform support | Current evidence |
| --- | --- | --- | --- |
| Software reference | Portable C++ | Platforms that build Flycast's software renderer | Unit tests and game-frame behavior tests |
| Accelerated hybrid backend | Metal rasterization/compute, with software fallback | Apple Metal platforms | Differential tests against the software reference and live macOS testing |
| Experimental native presentation | SDL Metal window plus Metal ImGui backend | macOS; default-off build option and runtime opt-in | Synthetic delivery, Seaman boot/idle, and renderer-recreation checks |

The hybrid renderer is **not** currently a portable GPU renderer. It does not
provide Vulkan, Direct3D, OpenGL compute, or non-Apple implementations. On
other platforms, `HybridTileRenderer.cpp` deliberately returns `false` so the
software renderer remains authoritative.

## Accuracy claims

The software renderer models the PVR pipeline explicitly, including tile-region
list selection, depth modes and writes, punch-through alpha testing, translucent
sorting, accumulation-buffer selection, modifier volumes, texture addressing
and filtering, mip and trilinear selection, fog, color clamping, blending, and
framebuffer conversion. Its unit tests cover those modeled behaviors.

The Metal backend is tested for bit-exact color and depth agreement with that
software implementation. This proves CPU/GPU equivalence for the exercised
states; it does not by itself prove that either implementation matches every
undocumented PowerVR2 edge case. A claim of complete hardware conformance
requires a broader test corpus captured from real Dreamcast hardware. No such
independent golden framebuffer corpus has yet been established for this work.
The software renderer is therefore a reference implementation, not a certified
hardware oracle. Passing its differential tests cannot validate assumptions
shared by the CPU and GPU implementations.

The current tested core suite contains 149 tests, including presentation
handoff tests, and passes on ARM64 and Rosetta. A separate six-test Metal UI
suite covers uploads, composition, and sampling. These counts are not counts
of hardware-verified PVR behaviors. RTT probes include expected-packing tests,
but dither comparisons remain integrity/differential evidence rather than
hardware golden results. See the [validation protocol](pvr-renderer-comparison-protocol.md)
for exact workload, binary, and log provenance.

## Performance claims

Performance measurements in this branch are Apple-specific and do not yet
establish reliable end-to-end Dreamcast frame rates. Earlier isolated ~5 ms
fill-rate results are historical, not a summary of the current correctness
path. Later measurements retained after coverage/depth correctness fixes show:

| Workload | Wall-time p95 | GPU-time p95 | 16.67 ms wall budget |
| --- | --- | --- | --- |
| Small triangles | 17.135 ms | 11.576 ms | Misses |
| Six translucent layers | 15.632 ms | 10.044 ms | Meets in this sample |
| Opaque visibility overhead | 9.725 ms | 5.860 ms | Meets in this sample |

These are the recorded benchmark samples, not newly rerun measurements for
every subsequent commit. A later 12-run measurement of the current renderer
test binary passed the p95 gates in every run: small-triangle wall p95 ranged
12.077–14.838 ms and six-layer wall p95 8.059–9.912 ms. A small-triangle maximum
still reached 17.562 ms. No rasterization algorithm change was made for this
remeasurement; it must not be presented as a measured optimization gain over
the historical failure. Native presentation has delivered all synthetic frame
IDs in some runs but lost frames in others; producer/selection traces locate
observed losses in the latest-only handoff. An opt-in distinct-image queue has
not yet shown a clear practical improvement. Approximately 16.67 ms median
intervals coexist with longer gaps and cannot establish sustained 60 Hz.

Seaman's 45-second unattended boot/idle samples run near 1x emulated speed on
both native Metal and OpenGL presentation. Audio-underrun counts vary and the
apparent advantage reverses across run order. These are not synchronized game
captures, a full playthrough, or proof of clean audio. The computer was muted.
None of these results should be generalized to other GPUs, operating systems,
graphics APIs, or game workloads without testing.

The 120 Mpixel/s comparison comes from the published Dreamcast specification
in *Game Developer*, September 1999:
https://ubm-twvideo01.s3.amazonaws.com/o1/vault/GD_Mag_Archives/GDM_September_1999.pdf
The focused benchmark can enforce its 60 Hz threshold with
`FLYCAST_HYBRID_ENFORCE_60HZ=1`.
Peak fill rate alone is not a conformance or full-emulator performance gate.

The asynchronous path preserves correctness by keeping ordered GPU work on one
Metal command queue, chaining prior IOSurface contents GPU-to-GPU, and draining
the latest completed result back into the CPU oracle before an unsupported
state falls back. Full framebuffer emulation and render-to-texture paths remain
synchronous because their VRAM visibility requirements differ from direct
screen presentation.

Native Metal presentation consumes immutable images after VRAM write/read
conversion; it does not bypass framebuffer semantics with a raw GPU surface.
It remains experimental: display rotation, configurable pacing,
native completion accounting across renderer epochs, and broader window/UI
lifecycle validation are unfinished. The default presentation path is unchanged.
