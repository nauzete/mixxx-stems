# PioneerXDJ-RR Stems port

This is a derivative of the supplied PioneerXDJ-RR skin. The original license and attribution are retained.

## Implemented

- Separate `STEMS` tab for 800x480 and larger displays.
- Four stem rows per deck using current Mixxx stem groups:
  - `[ChannelX_Stem1],mute|volume|color`
  - `[ChannelX_Stem2],mute|volume|color`
  - `[ChannelX_Stem3],mute|volume|color`
  - `[ChannelX_Stem4],mute|volume|color`
- Existing `[ChannelX],stem_count`.
- Interactive `KnobComposed` volume controls using local SVG assets.
- Background separation state, percentage, queue, cache, and error controls.
- Global model download and worker status.
- Conditional completed-stem and empty-state panels.

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
