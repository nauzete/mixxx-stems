# HTDemucs chunk pipeline

`StemChunkPipeline` implements the bounded C++ inference stage used by the
background stem worker. It is not safe for Mixxx's real-time audio thread:
decoding, ONNX inference, callbacks, and allocations may all block.

## Audio and tensor contract

- decoded input: stereo float32 at 44,100 Hz;
- model segment: fixed by the validated model manifest; the balanced export
  uses 171,990 frames (3.9 seconds), while the runner remains compatible with
  the official 343,980-frame model;
- overlap: 25%;
- stride: 75% of the model segment;
- model input: planar `[1, 2, segment]`;
- model output: planar `[1, 4, 2, segment]`;
- source order: drums, bass, other, vocals.

The source reader asks Mixxx's current `SoundSourceProxy` providers for stereo
44.1 kHz output and rejects a decoder that cannot satisfy that contract. Reads
are bounded and addressed by absolute frame offset.

## Normalization

The implementation applies the `mixxxdj/demucs` normalization contract to
each bounded inference window:

1. calculate the mono reference `(left + right) / 2` for the current window;
2. calculate its local mean and sample standard deviation;
3. normalize both input channels with those values;
4. run each padded model segment;
5. restore the original scale and mean on every output source.

There is no whole-track preliminary scan. The first model window can begin as
soon as that window has been decoded, which bounds time-to-first-result and
avoids reading a long track twice. Overlap-add smooths the boundary between
windows normalized with slightly different statistics.

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

Pipeline-owned storage is proportional only to the fixed model segment and is
independent of track duration. The 3.9-second variant uses one half of the
pipeline buffer capacity of the official 7.8-second export. ONNX Runtime owns
its separate session and output allocations.

Each finalized stride is written immediately to a disk-backed, interleaved
PCM16 temporary source. `CachingReader` initially serves the original track
through an eight-channel unity-sum fallback, then invalidates only cached
blocks covered by newly published stems. The next worker read replaces those
blocks without inference, allocation, locking, or file I/O on the audio
thread. The temporary source is removed after the final deck using the track
is ejected.

Cancellation is checked before every inference, after every inference, and
before output is committed. A cancelled run stops without emitting further
chunks.
