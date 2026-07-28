# Stem separation ControlObjects

The separation service owns these controls. Widgets, controller scripts, and
skins only observe or request work through this contract; they do not perform
model, cache, or queue work.

## Per-deck controls

`X` is the one-based Mixxx deck number.

| Control | Direction | Range | Initial value | Meaning |
| --- | --- | ---: | ---: | --- |
| `[ChannelX],separation_trigger` | write | button | 0 | Manually retry/queue the loaded track; normal deck loads trigger automatically |
| `[ChannelX],separation_cancel` | write | button | 0 | Cooperatively cancel fingerprinting or the associated job |
| `[ChannelX],separation_percentage` | read | 0–100 | 0 | Monotonic progress for the current attempt |
| `[ChannelX],separation_state` | read | 0–10 | 0 or 9 | State listed below |
| `[ChannelX],separation_queue_position` | read | -1 or greater | -1 | `0` is active, positive values are queued |
| `[ChannelX],stem_cache_status` | read | 0–2 | 0 | None, ready, or processing |
| `[ChannelX],stem_live_ready` | read | 0–1 | 0 | At least the first progressive stem chunk is readable |
| `[ChannelX],separation_error` | read | 0–4 | 0 | Stable numeric error category |
| `[ChannelX],stem_active_mode` | read/write | 0–1 | 0 | `0` makes pads Mute controls; `1` makes them exclusive Solo controls |

Separation states are:

| Value | State |
| ---: | --- |
| 0 | idle |
| 1 | queued |
| 2 | preparing |
| 3 | separating |
| 4 | encoding |
| 5 | validating |
| 6 | ready |
| 7 | cancelled |
| 8 | failed |
| 9 | unavailable |
| 10 | paused |

Cache status values are `0` none, `1` ready, and `2` processing. Error values
are `0` none, `1` model unavailable, `2` invalid source, `3` queue failure,
and `4` processing failure. Classic skins and mappings should render localized
text for these numeric values.

Changing or ejecting the loaded track resets all per-deck values. The live
alternate source is installed during the initial load. It serves a unity-sum
original-track fallback until each generated range is flushed, then
`CachingReader` replaces only the affected cached blocks. Generation starts
with a 10-second target and stays 10 seconds ahead of `playposition`; it does
not process the complete track merely because an analyzer reads ahead. The
last deck eject also cancels the job and removes its temporary stems.

## Global controls

| Control | Direction | Range | Initial value | Meaning |
| --- | --- | ---: | ---: | --- |
| `[StemSeparation],enabled` | read/write, persistent | 0–1 | 1 | Pause or resume the worker |
| `[StemSeparation],queue_size` | read | 0 or greater | 0 | Queued and paused jobs |
| `[StemSeparation],active_jobs` | read | 0–1 | 0 | Active low-priority jobs |
| `[StemSeparation],cache_size_bytes` | read | 0 or greater | 0 | Completed cache bytes |
| `[StemSeparation],cache_limit_bytes` | read | 0 or greater | platform default | Configured quota |
| `[StemSeparation],worker_state` | read | 0–4 | 0 or 3 | Idle, active, paused, unavailable, or downloading |
| `[StemSeparation],model_available` | read | 0–1 | 0 | Pinned manifest and model passed SHA-256 verification |
| `[StemSeparation],model_download_progress` | read | 0–100 | 0 | Streaming model download progress |
| `[StemSeparation],active_mode` | read/write | 0–1 | 0 | Global Mute/Solo mode propagated to all registered decks |
| `[StemSeparation],inference_threads` | read/write, persistent | platform range | configured default | ONNX intra-op threads; Windows 1–8, Linux/ARM64 1–4; changes apply after active live jobs finish |

Worker state values are `0` idle, `1` active, `2` paused, `3` unavailable, and
`4` downloading.

All source hashing, model verification, downloading, inference, encoding, and
filesystem work remains outside the real-time audio thread.
