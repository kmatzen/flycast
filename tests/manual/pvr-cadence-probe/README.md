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

### Rejected two-image FIFO experiment

A subsequent experiment replaced latest-image consumption with a FIFO holding
at most two pending immutable images. Publication never waited for the UI;
overflow dropped the oldest pending image. This preserved PVR/VRAM execution
ordering but could add a pending frame of presentation latency.

The same on/off/on/off protocol passed capture validation in all four runs.
The first VSync run still omitted measured ID 368; the second omitted none.
Both non-VSync runs omitted none. All four reported zero audio underruns.
The experimental suite passed 148 cases on ARM64 and Rosetta, including queue
ordering, overflow, and retained-image ownership. This did not establish
reliable delivery or an improvement distinguishable from run-to-run variation.
The FIFO and its test/flag were removed; the default latest-image path remains.

Raw local evidence: `/tmp/pvr-cadence-fifo-20261012/report.json` and separate
stream logs. Experimental app SHA-256:
`73c144bb20f63be526be285ed894c20d54770b8413b36dd8f0e594f4b62aea7b`.
The ELF hash is unchanged. These are exploratory results, not a performance
claim. Next investigation should locate time spent between frame publication,
UI acquisition, and completed swap before changing buffering again.

### Publication/selection/swap trace

Pass `--stages` to the cadence runner to enable `FLYCAST_PVR_TRACE_STAGES`.
Then analyze each separate stderr log:

```sh
node tools/read-pvr-stages.mjs /path/to/vsync-on-1.stderr.log
node --test tools/read-pvr-stages.test.mjs
```

The records use a process-wide monotonic clock and immutable image sequence
numbers. Publication is timestamped immediately before the mailbox store;
selection follows the UI mailbox load; swap follows the host swap return.
The selection-to-swap interval includes upload, drawing, UI scheduling, and
swap waiting, so it does not isolate a GPU or display-driver stall. Logging
itself adds overhead. These are diagnostic runs, not performance benchmarks.

In `/tmp/pvr-cadence-stages-20261012`, the two VSync runs missed one and six
measured IDs respectively. Every missing ID had a publication record but no
selection record. Both non-VSync runs missed none. VSync run one selected ID
539 at 410878106277 microseconds and completed its swap at 410878133379
(27,102 microseconds later). In between, IDs 540 and 541 were published at
410878120110 and 410878129527. The next selection was 541: 540 was replaced
before UI selection, not omitted by the PVR rendering work.

Selection-to-swap medians were 15,701 and 14,955 microseconds for the VSync
runs. Their p95 values were 16,933 and 16,942. Both had zero audio underruns;
the non-VSync runs had nine and zero. App SHA-256:
`8951acc796c24276f9345aad4d3a750abcc5ce55be4b397b3f50dde93cde89cc`.
ELF hash is unchanged. The captures fail the complete measured-delivery gate.

A native Metal presentation experiment must preserve the fully converted
framebuffer packet, rather than bypass framebuffer packing/readback semantics.
It also needs the UI overlay, resizing, paused-image retention, and teardown
to work together: the existing OpenGL driver owns both ImGui rendering and
the final swap. Replacing only the rasterizer does not replace that path.
