# Changing-frame cadence probe

This KallistiOS program submits 660 full-screen opaque quads. Each submission
has a different color that remains distinct after RGB565 framebuffer packing.
The first 60 submissions are warmup; progress records cover the following 600
and report KOS VBlank counts. The workload is intentionally simple: it tests
presentation cadence, not peak polygon or fill-rate throughput.

Build with a configured KallistiOS environment:

```sh
source /path/to/KallistiOS/environ.sh
make -C tests/manual/pvr-cadence-probe
```

The development run used KallistiOS revision
`568c3404c292a941bcf550465bd968f4fa249aa2`. No console execution has been
recorded, and this program is not a hardware conformance reference.

Run the ELF with renderer 7, full framebuffer emulation, threaded rendering,
SDL2 audio, and these environment flags:

```text
FLYCAST_HYBRID_SOFT=1
FLYCAST_PVR_DECOUPLED_PRESENT=1
FLYCAST_PVR_TRACE_DELIVERY=1
FLYCAST_PVR_TRACE_CADENCE_IDS=1
FLYCAST_PVR_TRACE_EMULATION=1
FLYCAST_AUDIO_TRACE_DELIVERY=1
```

Enable `Debug.SerialConsoleEnabled=yes` and `Debug.SerialPTY=false` to collect
the probe records. Compare `rend.vsync=yes` and `rend.vsync=no` in separate
fresh processes. Keep CPU/Metal differential verification disabled for timing;
use a separate verifier run for correctness. Do not interpret disabling VSync
as a tear-free solution.

`PVR_CADENCE_PROGRESS submitted=N vblanks=N-1` indicates one submission per
VBlank over the measured interval; the counter is sampled just after the last
submission. It does not prove a 60 Hz wall-clock rate: use the emulator pacing
trace as separate evidence. After `PVR_CADENCE_END`, the program retains its
last image. Stop the emulator normally to obtain final delivery/audio counters.

Delivery counters include startup and warmup, and exact-content comparison
adds CPU overhead. They identify changed framebuffer images reaching completed
host swap calls, not physical scanout. Missing changed images are a failure
to preserve the entire visual sequence even when average emulation speed is
near 1.00x. The cadence-ID trace decodes the center pixel of the immutable
source image associated with each completed swap. It records changed IDs,
not repeated swaps, and separates omissions in IDs 0–59 (warmup) from IDs
60–659 (measurement). It is specific to this probe's color encoding, not a
general game-frame detector or a readback of the actual display.

For a repeatable VSync-on/off/on/off comparison, run:

```sh
node tools/run-pvr-cadence.mjs /absolute/path/to/Flycast \
  /absolute/path/to/pvr-cadence-probe.elf /new/output/directory
```

The runner records binary hashes, preserves stdout and stderr separately
(serial output can arrive in partial lines), validates all five progress
records, and checks that decoded IDs are strictly increasing. Each process
stops normally after the end marker and final ID. A timeout, forced shutdown,
or malformed capture fails validation. Valid captures with missing measured
IDs still complete all four cases, then return a failing exit status. Missing
IDs remain failures even when emulation speed and audio counters look healthy.

## Recorded development comparison (2026-10-12)

On the development M1, with full framebuffer emulation and decoupled
presentation, the four serial runs produced:

| Run | Missing warmup IDs | Missing measured IDs | Audio underruns |
| --- | ---: | ---: | ---: |
| VSync on, first | 0 | 10 | 0 |
| VSync off, first | 0 | 0 | 6 |
| VSync on, second | 1 | 0 | 0 |
| VSync off, second | 0 | 0 | 3 |

All captures passed protocol validation; the overall lossless-measurement
gate failed. The first run omitted IDs 68, 188, 362, 401, 433, 472, 591, 593,
639, and 646, so loss is not confined to startup. The second VSync run omitted
only warmup ID 57. These short runs demonstrate variability, not a causal
explanation or a stable failure rate. Audio counters cover the whole process.
The host stayed muted; the real SDL2 audio backend remained active.

Local raw evidence: `/tmp/pvr-cadence-separated-20261012/report.json` and its
separate per-run stdout/stderr logs. App SHA-256:
`355a45d202e30fb3150e1c25d3d581c4989a1e76f2c05cce21b54c85ebb9e914`.
ELF SHA-256:
`a41e891d119ab04be49d395fed72448500d18890db377f1f76010bab43ce44ec`.
The decoder and existing renderer suite passed all 147 tests on native ARM64
and x86-64 under Rosetta. Neither those tests nor these swaps prove physical
scanout timing or original-hardware accuracy.
