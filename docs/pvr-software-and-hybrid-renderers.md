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
the current 72-layer synthetic stress case completes in roughly one 60 Hz frame
and tested Seaman scenes meet their game cadence. These measurements must not be
generalized to other Apple GPUs, operating systems, or graphics APIs without
testing them there.

The asynchronous path preserves correctness by keeping ordered GPU work on one
Metal command queue, chaining prior IOSurface contents GPU-to-GPU, and draining
the latest completed result back into the CPU oracle before an unsupported
state falls back. Full framebuffer emulation and render-to-texture paths remain
synchronous because their VRAM visibility requirements differ from direct
screen presentation.
