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

- input name and shape: `input`, `[1, 2, 343980]`;
- output name and runtime shape: `output`, `[1, 4, 2, 343980]`;
- element type: 32-bit float;
- logical source order: drums, bass, other, vocals;
- execution provider: CPU;
- execution mode: sequential;
- one inter-op thread and a configurable positive intra-op thread count.

Construction and inference allocate memory and block. The class is restricted
to a background worker and must never be invoked by the real-time audio
thread.

## Smoke validation

The `ONNX Runtime C++ smoke test` workflow builds the same runner source on
native Windows x64 and Ubuntu 24.04 ARM64 runners. It verifies the SHA-256 of
the published model before loading it, runs a deterministic stereo segment,
and rejects an invalid tensor contract, output shape, element count, or
non-finite output.
