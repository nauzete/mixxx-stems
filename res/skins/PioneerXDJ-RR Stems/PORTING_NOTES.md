# PioneerXDJ-RR Stems port

This is a derivative of the supplied PioneerXDJ-RR skin. The original license and attribution are retained.

## Implemented

- Four stem pads per deck in a 2x2 layout within the main mixing view:
  - `[ChannelX_Stem1],mute|volume|color`
  - `[ChannelX_Stem2],mute|volume|color`
  - `[ChannelX_Stem3],mute|volume|color`
  - `[ChannelX_Stem4],mute|volume|color`
- Existing `[ChannelX],stem_count`.
- Independent `[ChannelX_StemN],solo` controls preserve mute state.
- A global-looking Mute/Solo selector updates both deck mode controls.
- Pads remain disabled and gray until
  `[ChannelX],stem_live_ready` reports the first published chunk.
- Per-deck eject controls use the existing `[ChannelX],eject` API.

## Control contract

The fork implements and tests:

- `[ChannelX],separation_trigger`
- `[ChannelX],separation_cancel`
- `[ChannelX],separation_percentage`
- `[ChannelX],separation_state`
- `[ChannelX],separation_queue_position`
- `[ChannelX],stem_cache_status`
- `[ChannelX],separation_error`
- global `[StemSeparation]` status controls

The existing stem bindings were verified against current Mixxx `main`.

## Validation boundary

Static XML, resource, control-binding, and 800×480 layout checks are automated.
Screenshots and touch interaction still require launching a packaged build.
