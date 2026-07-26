# Incremental STEM container

The background separation pipeline writes directly to a five-stream
`.stem.mp4.partial` file. It never creates full-track PCM WAV intermediates.

## Stream contract

The MP4 contains five stereo AAC-LC streams at 44.1 kHz:

1. premix, calculated as the sum of all separated sources;
2. drums;
3. bass;
4. other;
5. vocals.

All stem streams use the same codec, sample rate, and bitrate. The default
128 kbit/s per stream has a raw four-minute estimate of 19.2 MB, below the
32 MiB product target.

The writer accepts packed `[stem, channel, frame]` chunks in strict sequential
order. It buffers at most one AAC frame per stream and passes encoded packets
straight to FFmpeg's MP4 muxer.

## Commit protocol

1. Remove only an exact stale `.partial` path for the requested output.
2. Encode and mux into that `.partial` file.
3. Insert a compact version-1 JSON manifest at `moov/udta/stem`. Existing MP4
   data is shifted in bounded 1 MiB blocks if necessary.
4. Reopen the file with FFmpeg and require five stereo audio streams, four
   matching 44.1 kHz AAC stem streams, a readable manifest, and the configured
   per-track size limit.
5. Rename the validated file to `.stem.mp4` in the same directory.

Cancellation and every failed validation remove the partial output. The
writer performs codec work, allocation, and file I/O and is restricted to a
background worker.
