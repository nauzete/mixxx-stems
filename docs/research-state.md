# Stems integration research state

Last verified: 2026-07-25

This document records the upstream state used by the experimental Mixxx Stems
fork. Repository and pull request state is expected to change and must be
rechecked before reusing unmerged code.

## Pinned upstream revisions

| Repository | Branch | Revision |
| --- | --- | --- |
| `mixxxdj/mixxx` | `main` | `66f7912343c9938339b9d3b12ed7a9fee38d97f5` |
| `mixxxdj/demucs` | `main` | `d788c1a06876ced89b11d6531f771e5e40204d48` |
| `mixxxdj/vcpkg` | `main` | `41f250c938a7af7ee57bb49f30f94c5355d03c2f` |

Mixxx identifies this revision as 2.7.0-alpha. New functionality therefore
targets `main`; branch 2.6 is retained only for comparison.

## Current upstream capabilities

- STEM playback is already implemented behind the `STEM` CMake option.
- `SoundSourceSTEM` accepts `.stem.mp4` and `.stem.m4a` and requires five
  stereo audio streams: one premixed stream followed by exactly four stem
  streams. The premix is not currently loaded for deck playback.
- The existing controls are `[ChannelX],stem_count` and
  `[ChannelX_StemN],volume`, `mute`, `color`, plus per-stem VU meters and
  QuickEffect groups.
- The official DDJ-FLX4 XML and JavaScript mapping are both present. Keyboard
  mode already maps pads 1-4 to stem mute/solo behavior and pads 5-8 to stem
  QuickEffects through `stemsPadsModesStatus`,
  `stemMutePadsFirstControl`, and `stemFxPadsFirstControl`.
- Stem manifest reading exists in `StemInfoImporter`. Mixxx can read the
  `moov/udta/stem` JSON atom but `main` does not provide a stem container
  writer.
- No ONNX or ONNX Runtime dependency or C++ inference implementation exists in
  the Mixxx CMake project or vcpkg manifest.
- The library schema in `main` still assumes one physical location per logical
  track. Alternate-source linking must not be implemented against unmerged PR
  APIs without adaptation.

## Issue and pull request matrix

| Item | State on 2026-07-25 | Integration consequence |
| --- | --- | --- |
| `mixxxdj/mixxx#15495` | Open, updated 2026-06-28 | Current upstream epic. ORT manifest/buildenv, C++ runner, pipeline, and separation UI remain unchecked. |
| `mixxxdj/mixxx#11391` | Open, updated 2026-04-03 | General AI separation request remains open. |
| `mixxxdj/mixxx#14758` | Open, updated 2026-01-30 | Stem-file linking is not solved on `main`. |
| `mixxxdj/mixxx#13070` | Merged | Stem engine support is available on `main`. |
| `mixxxdj/mixxx#13086` | Merged | Stem controls and their real group names are available on `main`. |
| `mixxxdj/mixxx#13106` | Merged | Stem analyser support is available on `main`. |
| `mixxxdj/mixxx#13268` | Merged | Advanced stem loading controls are available on `main`. |
| `mixxxdj/mixxx#14635` | Open against 2.6 | Tests only; inspect rather than copy. |
| `mixxxdj/mixxx#15881` | Closed, not merged | The external `stemgen` approach was not accepted. |
| `mixxxdj/mixxx#15888` | Open, dirty merge state | Stem metadata editing/writing work is incomplete and depends on other changes. |
| `mixxxdj/mixxx#15891` | Open against 2.6, dirty merge state | Python HTDemucs plus MP4Box is incompatible with this fork's C++ runtime requirement. |
| `mixxxdj/mixxx#16305` | Open, blocked | Proposed 1-to-N `track_locations` foundation is not in `main`. |
| `mixxxdj/mixxx#16756` | Open, blocked | Optional stacked stem waveforms are not in `main`. |
| `mixxxdj/vcpkg#194` | Merged | Mixxx vcpkg has absorbed upstream ONNX Runtime port history. |
| `microsoft/vcpkg#36850` | Merged | The upstream ONNX Runtime port exists. Mixxx still needs manifest/build integration. |

## Demucs export contract

The pinned `mixxxdj/demucs` revision contains
`scripts/convert-pth-to-onnx.py`. It selects `htdemucs`, enables the
ONNX-exportable model path, exports with opset 17, and uses an input tensor of
shape `[1, 2, 343980]` (7.8 seconds at 44.1 kHz). The configured and pretrained
source order is:

1. drums
2. bass
3. other
4. vocals

The output order must still be captured in the generated model manifest and
validated by parity tests. The ONNX model is a release asset and must never be
committed to Git.

## Decisions for this fork

- Runtime separation will be C++ and ONNX Runtime CPUExecutionProvider only.
- Python and PyTorch are restricted to reproducible export and parity jobs.
- Output must be streamed to a five-audio-stream STEM MP4 (`premix + 4 stems`)
  because that is the format enforced by the pinned Mixxx reader.
- Inference, hashing, decoding, encoding, file I/O, database access, and large
  allocations remain off the real-time audio thread.
- The library-linking layer will be isolated behind a narrow interface until
  upstream resolves the schema/API represented by PR #16305.
- Physical Raspberry Pi 5, DDJ-FLX4, touch display, thermal, underrun, and soak
  tests remain external hardware acceptance work.
