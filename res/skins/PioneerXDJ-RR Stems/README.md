# PioneerXDJ-RR Stems

Compact GPLv3 classic Mixxx skin for 800×480 and larger touch displays. This
derivative keeps the supplied PioneerXDJ-RR/Pioneered layout as a separate skin
and integrates stem controls into the main mixing view.

This skin is part of an experimental fork and is not an official Mixxx release.

## Stem controls

- Four 2x2 stem pads in the left panel of each waveform: drums, vocals, bass,
  and other.
- Unavailable stems remain disabled and gray.
- Ready pads use green, blue, red, and purple respectively.
- A global ACTIVE STEM selector in the upper-right switches both decks between
  Mute and exclusive Solo behavior.
- ONNX threads are selectable from 1-8 on Windows and 1-4 on Linux/ARM64.
- One or two threads are recommended on Raspberry Pi 5; higher values trade UI
  and audio scheduling headroom for inference throughput.
- Each lower deck includes an Eject button.

The skin does not perform model inference or file operations.

## Attribution

The original theme was created for small-screen, controller-assisted use by:

- [timewasternl](https://github.com/timewasternl)
- [GorgiAstro](https://github.com/GorgiAstro)
- [BvOBart](https://github.com/bvobart)
- [bencejuhaasz](https://github.com/bencejuhaasz)

The original GPLv3 license is preserved in `LICENSE`.
