# PioneerXDJ-RR Stems

Compact GPLv3 classic Mixxx skin for 800×480 and larger touch displays. This
derivative keeps the supplied PioneerXDJ-RR/Pioneered layout as a separate skin
and adds a dedicated two-deck STEMS page.

This skin is part of an experimental fork and is not an official Mixxx release.

## Stems page

- Four real Mixxx stem groups per deck: drums, bass, other, and vocals.
- Touch-sized mute buttons and interactive volume knobs.
- Existing stem color and `stem_count` readouts.
- Generate/cancel controls, textual state, percentage, queue position, cache
  status, and error category.
- Global model, download, queue, worker, and active-job status.
- Stem rows are hidden until a completed stem representation is loaded.

Generated audio becomes active only on a subsequent deck load. The skin does
not perform separation logic.

## Attribution

The original theme was created for small-screen, controller-assisted use by:

- [timewasternl](https://github.com/timewasternl)
- [GorgiAstro](https://github.com/GorgiAstro)
- [BvOBart](https://github.com/bvobart)
- [bencejuhaasz](https://github.com/bencejuhaasz)

The original GPLv3 license is preserved in `LICENSE`.
