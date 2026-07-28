# Stem library linking

Generated audio is an alternate representation of an existing logical Mixxx
track. It is not imported as a second library track.

`StemAlternateSourceLinker` persists this association in the stem cache. Each
link contains the normalized source path, source size and modification time,
and the cache entry identifier. A link is returned only when both the original
source fingerprint and completed `.stem.mp4` still exist. Stale links are
removed with an atomic index update.

The separation worker publishes the link only after the container and cache
entry have both been validated. Cached results also repair a missing link when
they are reused.

## Deck load behavior

`BaseTrackPlayer` accepts an alternate-source resolver. The resolver is called
once during a normal GUI-thread deck load, after access to the original source
has been granted. The resolved URL travels with that load request through
`EngineBuffer` and `CachingReader`.

`SoundSourceProxy` opens audio from the generated URL while retaining the
original `TrackPointer`. Consequently, cues, beatgrid, BPM, key, color, rating,
play count, history, and library metadata continue to belong to the original
track.

The URL is captured for one load request. Completing separation does not
replace the source of an already loaded or playing deck. The generated
representation is therefore first used on a subsequent load.

All link-index and filesystem operations run outside the real-time audio
thread.
