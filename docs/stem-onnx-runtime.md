# ONNX Runtime integration

The stem-separation runtime uses the ONNX Runtime C++ API only. Python,
PyTorch, and FFmpeg are not runtime dependencies of `DemucsOnnxRunner`.

## Dependency

The project pins the `nauzete/vcpkg-mixxx-stems` fork at commit
`859d11a94a9cf32fbf7fecc7de3b54980102470b`. Its `2.7`-based
`feature/onnx-runtime` branch contains the upstream vcpkg
`onnxruntime` 1.23.2 CPU port.

Top-level CMake discovers either:

- the vcpkg config target `onnxruntime::onnxruntime`; or
- an official ONNX Runtime binary distribution supplied through
  `ONNXRUNTIME_ROOT`.

`ONNXRUNTIME` defaults to enabled only when the dependency is available. A
caller that sets `-DONNXRUNTIME=ON` without the dependency receives a
configuration error.

## Runner contract

`mixxx::stems::DemucsOnnxRunner` validates the published HTDemucs tensor
contract while constructing its session:

- input name and shape: `input`, `[1, 2, segment]`;
- output name and runtime shape: `output`, `[1, 4, 2, segment]`;
- accepted fixed segment range: 44,100 through 343,980 frames;
- element type: 32-bit float;
- logical source order: drums, bass, other, vocals;
- execution provider: CPU;
- execution mode: sequential;
- one inter-op thread and a configurable positive intra-op thread count.

The CPU memory arena and memory pattern are disabled on all supported
platforms. Measurements of the fixed 2.6-second model on Windows showed the
arena change reducing the isolated inference-process peak from about 1.75 GB
to 1.07 GB, with a latency tradeoff. These figures are diagnostic comparisons,
not Raspberry Pi measurements.

ONNX worker threads run below normal priority on Windows and at nice level 10
on Linux. Intra-op and inter-op spinning are disabled, so a live session
waiting for more playback demand does not keep cores busy. Thread-count
changes never block an active live job; if stems are already being generated,
the requested value is applied to the first job started after all active live
jobs finish. For Raspberry Pi 5, start with one or two threads. Four is
available for measurement but can reduce UI and audio scheduling headroom.
Windows exposes up to eight threads, but more inference throughput does not
necessarily improve interface responsiveness.

A MatMul/Gemm-only dynamic INT8 experiment reduced the 2.6-second model file
from about 304 MB to 220 MB and preserved the synthetic parity output, but on
Windows it was slower and saved only about 0.10 GB with the arena disabled.
It is therefore not selected as the default model. Full dynamic INT8 is also
rejected because the CPU execution provider cannot run the resulting
`ConvInteger` graph.

Construction and inference allocate memory and block. The class is restricted
to a background worker and must never be invoked by the real-time audio
thread.

`StemSeparationManager` owns a single serialized processor and that processor
owns one `DemucsOnnxRunner` session. Both decks therefore share one loaded
model; loading the same track in a second deck reuses the same live session
and job instead of loading another model or starting parallel inference.

## Smoke validation

The `ONNX Runtime C++ smoke test` workflow builds the same runner source on
native Windows x64 and Ubuntu 24.04 ARM64 runners. It verifies the SHA-256 of
the published model before loading it, runs a deterministic stereo segment,
and rejects an invalid tensor contract, output shape, element count, or
non-finite output.
