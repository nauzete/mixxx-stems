# Mixxx Stems overview

> This fork is not an official Mixxx release.

Mixxx Stems adds local, background HTDemucs separation to current Mixxx
`main`. The runtime is C++ with ONNX Runtime CPU execution. Python and PyTorch
are used only by the reproducible model-export workflow.

The implementation is split into independent layers:

1. a validated ONNX model contract and CPU inference runner;
2. bounded decoding, normalization, segmentation, overlap-add, and progress;
3. progressive disk-backed PCM16 publication for the loaded decks;
4. selective `CachingReader` replacement of original-audio fallback chunks;
5. optional five-stream STEM MP4 encoding and a quota-bound LRU cache;
6. stable ControlObjects consumed by the skin and controller mapping.

Inference, decoding, encoding, hashing, file access, and large allocation take
place on low-priority background workers. The audio thread does not perform
model inference, locks, file I/O, or blocking work. A loaded track begins with
an eight-channel unity-sum fallback, then already cached fallback blocks are
selectively discarded as separated chunks become readable.

## User interface

Select the `PioneerXDJ-RR Stems` skin for a compact 800 x 480 touch interface.
It exposes both decks, four color-coded 2x2 stem pads, a global Mute/Solo mode,
an inference-thread selector, and deck eject controls. Separation starts
automatically when a track loads.

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

The automatic live path writes interleaved eight-channel PCM16 to disk as each
stride completes. It initially requests 10 seconds and then follows each
deck's playback position with a 10-second look-ahead instead of separating the
whole track immediately. Both decks share the same model session, while their
chunk requests are served fairly by the serialized runner. It never retains
complete float32 stems in RAM and removes the temporary file when the last deck
using the track is ejected. A 512 MiB guard prevents unbounded temporary
output. The existing offline cache path can still publish an AAC-LC
five-stream MP4 atomically.

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
