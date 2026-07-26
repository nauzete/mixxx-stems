# HTDemucs chunk pipeline

`StemChunkPipeline` implements the bounded C++ inference stage used by the
background stem worker. It is not safe for Mixxx's real-time audio thread:
decoding, ONNX inference, callbacks, and allocations may all block.

## Audio and tensor contract

- decoded input: stereo float32 at 44,100 Hz;
- model segment: 343,980 frames (7.8 seconds);
- overlap: 25%;
- stride: 257,985 frames;
- model input: planar `[1, 2, 343980]`;
- model output: planar `[1, 4, 2, 343980]`;
- source order: drums, bass, other, vocals.

The source reader asks Mixxx's current `SoundSourceProxy` providers for stereo
44.1 kHz output and rejects a decoder that cannot satisfy that contract. Reads
are bounded and addressed by absolute frame offset.

## Normalization

The implementation follows `mixxxdj/demucs`:

1. scan the mono reference `(left + right) / 2` in bounded blocks;
2. calculate its global mean and sample standard deviation;
3. normalize both input channels with those values;
4. run each padded model segment;
5. restore the original scale and mean on every output source.

The preliminary statistics pass represents the first 10% of reported progress.
Inference and overlap-add represent the remaining 90%.

## Overlap-add and final padding

Each segment uses the triangular weight from the official Demucs
`apply_model()` implementation. The pipeline emits only samples that cannot be
affected by a future segment, shifts the retained overlap into reusable
accumulators, and divides by the accumulated weight.

Short final segments are padded around their center using available track
context, with zero padding only outside the track. Model output is center
trimmed before overlap-add, matching `TensorChunk.padded()` and
`center_trim()`.

## Memory and cancellation

Pipeline-owned storage has a constant capacity of 6,879,600 float samples
(about 26.2 MiB), independent of track duration. It consists of reusable input,
overlap, weight, and emission buffers. ONNX Runtime owns its separate session
and output allocations.

Cancellation is checked during the statistics scan, before every inference,
after every inference, and before output is committed. A cancelled run stops
without emitting further chunks.
