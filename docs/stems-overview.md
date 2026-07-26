# Mixxx Stems overview

> This fork is not an official Mixxx release.

Mixxx Stems adds local, background HTDemucs separation to current Mixxx
`main`. The runtime is C++ with ONNX Runtime CPU execution. Python and PyTorch
are used only by the reproducible model-export workflow.

The implementation is split into independent layers:

1. a validated ONNX model contract and CPU inference runner;
2. bounded decoding, normalization, segmentation, overlap-add, and progress;
3. incremental five-stream STEM MP4 encoding and atomic publication;
4. a persistent priority queue and quota-bound LRU cache;
5. original-to-generated source linking at the next deck load;
6. stable ControlObjects consumed by the skin and controller mapping.

Inference, decoding, encoding, hashing, file access, and large allocation take
place on low-priority background workers. The audio thread does not perform
model inference, locks, file I/O, or blocking work. A generated file is never
hot-swapped into a playing deck.

## User interface

Select the `PioneerXDJ-RR Stems` skin for a compact 800 x 480 touch interface.
It exposes both decks, four stem mute/volume/color rows, generation,
cancellation, progress, queue, cache, model, worker, and error state.

Select `Pioneer DDJ-FLX4 Stems` as the controller mapping. The official
mapping behavior is retained. Keyboard mode pads 1-4 control drums, bass,
other, and vocals; pads 5-8 retain stem FX. Hold the Keyboard mode selector to
start separation, or hold it again while processing to cancel. The Keyboard
LED blinks during processing, double-flashes on failure, and stays lit when
the generated stems are ready.

## Model and storage

The pinned model is downloaded from the
`htdemucs-955717e8-onnx-v1` release and verified before use. Its SHA-256 is
`db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d`.
The model is not stored in Git.

Generated audio uses AAC-LC at 44.1 kHz in a five-stream MP4: premix, drums,
bass, other, and vocals. At the default 128 kbit/s per stream, a four-minute
file is estimated at 19.2 MB before container overhead, below the 32 MiB
target. Only an exact `.partial` file is used before atomic publication.

See the component documents in this directory for cache, queue, container,
library-linking, controls, model, packaging, and benchmark details.

## Licensing and source

Mixxx and this derivative remain GPL. The supplied PioneerXDJ-RR skin retains
its GPLv3 notice and attribution. The FLX4 variant retains the official mapping
authors. Demucs and ONNX Runtime license notices are distributed with their
respective model and application artifacts. FFmpeg and codec notices remain
part of the Mixxx package.

Corresponding source for release binaries is the Git tag used by the
`Mixxx Stems packages` workflow. No model binary or generated audio belongs in
the source repository.

## Current limitation

Physical Raspberry Pi 5, DDJ-FLX4, touch, thermal, storage, output, underrun,
and two-hour soak validation remains `BLOCKED_EXTERNAL_HARDWARE`. Runner
benchmarks must not be presented as Raspberry Pi 5 results.
