# Stem benchmarks and physical validation

The packaging workflow runs one deterministic HTDemucs segment with 1, 2, 4,
and 8 threads on Windows x64, and with 1, 2, and 4 threads on a native Ubuntu
ARM64 runner. Each run records elapsed inference time, audio duration,
real-time factor (RTF), peak process RSS, thread count, architecture, and ONNX
Runtime version as JSON.

`RTF = processing seconds / audio seconds`. The engineering target is below
`1.0`, with `0.75` or lower preferred. A GitHub ARM64 runner demonstrates build
compatibility and gives an orientative CPU result only. It is not a Raspberry
Pi 5 performance result.

For a Raspberry Pi run:

```bash
tools/stems/run-stem-benchmark.sh \
  ./demucs-onnx-benchmark \
  ./htdemucs.onnx \
  evidence/benchmark \
  2
```

The benchmark uses synthetic stereo tones, not copyrighted audio. It performs
model loading outside the timed interval and times one exact
`[1, 2, segment]` CPU inference using the fixed segment declared by the model.
Peak RSS covers the complete benchmark process.

## Two-hour local telemetry

Start Mixxx Stems, find its PID, then run:

```bash
MIXXX_STEMS_LOG=/path/to/mixxx.log \
tools/stems/monitor-rpi5.sh PID 7200 5 evidence/soak
```

The monitor writes only local files. It samples RSS, available RAM, swap, CPU,
temperature, Raspberry Pi throttling flags, stem-cache size, persistent queue
length, and underrun/xrun mentions. It also captures a final PipeWire snapshot
when `pw-top` is available.

Review the CSV for out-of-memory termination, sustained swap use, rising RSS,
thermal throttling, and audio underruns. The bounded DSP buffers are designed
to avoid complete-track float32 audio and four complete float32 stems in RAM,
but only this physical test can validate the full application workload.

## Hardware acceptance

Record the manual results with:

```bash
tools/stems/record-hardware-checks.sh evidence/hardware.tsv
```

Until signed evidence is produced on the named devices, all of these remain
`BLOCKED_EXTERNAL_HARDWARE`:

- real DDJ-FLX4 mapping, pads, and LEDs;
- master and headphone outputs;
- Raspberry Pi 5 Cortex-A76 performance;
- temperature and throttling;
- microSD and SSD behavior;
- audio underruns;
- 10.1-inch touch display at 800 x 480;
- a two-hour soak test.

CI does not and cannot approve those physical checks.
