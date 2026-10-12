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
near 1.00x. The current trace does not identify which probe submission was
omitted; per-frame identifiers would be needed to isolate the warmup boundary.
