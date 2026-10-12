# PVR software and hybrid renderer status

This work contains two distinct renderers with different platform support and
different standards of evidence.

| Renderer | Implementation | Platform support | Current evidence |
| --- | --- | --- | --- |
| Exact software oracle | Portable C++ | Platforms that build Flycast's software renderer | Unit tests and game-frame behavior tests |
| Accelerated hybrid backend | Metal compute with IOSurface presentation | Apple Metal platforms; direct IOSurface/OpenGL presentation is macOS-specific | Differential tests against the software oracle and live macOS testing |

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
requires a broader test corpus captured from real Dreamcast hardware.

## Performance claims

Performance measurements in this branch are Apple-specific. On an Apple M1,
the reproducible 640x480 fill-rate workload completes in about 5.0 ms against a
16.67 ms 60 Hz frame budget. It draws six full-screen translucent layers, or
1.84 million shaded pixels per frame, which stays below the contemporary quoted
Dreamcast peak of 120 million pixels per second (2.0 million per 60 Hz frame).
The separate 72-layer full-screen stress case is intentionally far outside that
hardware envelope and takes about 62.4 ms; it is a throughput diagnostic, not a
frame-rate claim. Tested Seaman scenes meet their game cadence. These
measurements must not be generalized to other Apple GPUs, operating systems, or
graphics APIs without testing them there.

The 120 Mpixel/s comparison comes from the published Dreamcast specification
in *Game Developer*, September 1999:
https://ubm-twvideo01.s3.amazonaws.com/o1/vault/GD_Mag_Archives/GDM_September_1999.pdf
The focused benchmark can enforce its 60 Hz threshold with
`FLYCAST_HYBRID_ENFORCE_60HZ=1`.

The asynchronous path preserves correctness by keeping ordered GPU work on one
Metal command queue, chaining prior IOSurface contents GPU-to-GPU, and draining
the latest completed result back into the CPU oracle before an unsupported
state falls back. Full framebuffer emulation and render-to-texture paths remain
synchronous because their VRAM visibility requirements differ from direct
screen presentation.
