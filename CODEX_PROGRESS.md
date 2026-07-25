# Codex progress

## Session 2026-07-25

### Bootstrap

- Authenticated GitHub owner: `nauzete` (verified through Git Credential
  Manager and the GitHub API).
- Created forks:
  - `https://github.com/nauzete/mixxx-stems`
  - `https://github.com/nauzete/vcpkg-mixxx-stems`
- Configured `origin` to the user forks and `upstream` to the official
  repositories. Push URLs for official upstream remotes are disabled.
- Cloned `mixxxdj/demucs` as a read-only reference with its push URL disabled.
- Synchronized local `main` branches to the pinned upstream revisions below.
- Created integration branch `stems-integration`.

### Base revisions

| Component                 | Revision                                   | Synchronization date |
| ------------------------- | ------------------------------------------ | -------------------- |
| Mixxx                     | `66f7912343c9938339b9d3b12ed7a9fee38d97f5` | 2026-07-25           |
| Demucs                    | `d788c1a06876ced89b11d6531f771e5e40204d48` | 2026-07-25           |
| vcpkg                     | `41f250c938a7af7ee57bb49f30f94c5355d03c2f` | 2026-07-25           |
| vcpkg 2.7 dependency line | `1c20f84aa1ffca2ef18a7d9c6bd7cdd1f5f2e265` | 2026-07-25           |

### Completed work

- Read the supplied project specification, repository `AGENTS.md`, upstream
  `AGENTS.md`, and upstream `CONTRIBUTING.md`.
- Rechecked every issue and pull request listed in the supplied
  `SOURCE_MATRIX.md`.
- Audited current stem playback, ControlObjects, STEM MP4 reader, FLX4 mapping,
  Demucs exporter, and ONNX dependency state.
- Recorded the results and architectural consequences in
  `docs/research-state.md`.

### Branches

- `main`: synchronized with `mixxxdj/mixxx:main`.
- `stems-integration`: active integration branch.
- `feature/onnx-runtime`
- `feature/demucs-runner`
- `feature/stem-container`
- `feature/stem-cache`
- `feature/background-queue`
- `feature/library-integration`
- `feature/stem-controls`
- `feature/stem-ui`
- `feature/flx4-stems`
- `feature/windows-packaging`
- `feature/arm64-packaging`

All feature branches start from the documented integration baseline.

### Pull requests

- Draft integration PR: `https://github.com/nauzete/mixxx-stems/pull/1`
- Merged native ARM64 packaging PR:
  `https://github.com/nauzete/mixxx-stems/pull/2`
- Merged HTDemucs model export PR:
  `https://github.com/nauzete/mixxx-stems/pull/3`
- Draft ONNX Runtime C++ integration PR:
  `https://github.com/nauzete/mixxx-stems/pull/4`
- Draft ONNX Runtime dependency validation PR:
  `https://github.com/nauzete/vcpkg-mixxx-stems/pull/1`

### Builds and tests

- Enabled GitHub Actions on the new fork.
- Phase 1 (clean baseline builds) is complete at commit `bd7fe8613b`.
- GitHub Actions run
  `https://github.com/nauzete/mixxx-stems/actions/runs/30159873805`
  completed successfully on its second attempt, including `Ready to merge`.
- Windows x64 compiled, passed the configured tests, and generated an MSI.
- Ubuntu 24.04 ARM64 compiled natively on `ubuntu-24.04-arm`; all 1,265
  enabled tests passed.
- The ARM64 job confirmed:
  - runner architecture: `aarch64`;
  - Debian package architecture: `arm64`;
  - packaged executable: `ELF 64-bit LSB ... ARM aarch64`.
- Windows ARM64 also compiled, passed its configured tests, and generated an
  ARM64 MSI. This is additional coverage, not a product target.
- The first run exposed two known upstream architecture-specific failures:
  - libmad `FPM_DEFAULT` produces the documented first-sound sample 3,326 on
    Ubuntu ARM64;
  - Microsoft Media Foundation crashes in
    `SoundSourceProxyTest.regressionTestCachingReaderChunkJumpForward` on
    Windows ARM64 (`mixxxdj/mixxx#15638`).
    Commit `bd7fe8613b` handles both cases without relaxing Windows x64 tests.
- A macOS x64 Audio Unit initialization flake (`mixxxdj/mixxx#16448`) failed
  the first attempt and passed when only failed jobs were rerun.
- Supplied package integrity passed for all 51 files.
- Supplied PioneerXDJ-RR Stems scaffold XML is well formed.
- Phase 2 exported HTDemucs with the official
  `mixxxdj/demucs@d788c1a06876ced89b11d6531f771e5e40204d48` exporter.
- ONNX full-check and ONNX Runtime CPU parity passed for deterministic
  synthetic audio and the official Demucs `test.mp3` fixture in run
  `https://github.com/nauzete/mixxx-stems/actions/runs/30167727897`.
- The final combined Phase 1 and Phase 2 commit passed the complete Mixxx
  Actions matrix, including `Ready to merge`, in run
  `https://github.com/nauzete/mixxx-stems/actions/runs/30167727961`.
- Phases 1 and 2 are integrated at `66e612aaf7fbd9f8477a80928657f34eb4522295`.
- The model binary remains outside Git history.
- Phase 3 integrates ONNX Runtime 1.23.2 CPU through the Mixxx vcpkg fork and
  CMake target `onnxruntime::onnxruntime`.
- `DemucsOnnxRunner` loads the published HTDemucs model, validates the exact
  input/output tensor contract and stem order, and executes synchronous CPU
  inference for use only from a background worker.
- The dedicated C++ smoke test downloaded the release model, verified its
  SHA-256, compiled the same runner source, and completed real-model inference
  on Windows x64 and native Ubuntu 24.04 ARM64:
  `https://github.com/nauzete/mixxx-stems/actions/runs/30173562764`.
- The complete Mixxx Actions matrix passed, including all builds, tests,
  static analysis, formatting, and `Ready to merge`:
  `https://github.com/nauzete/mixxx-stems/actions/runs/30173562848`.
- The focused vcpkg port validation passed on Windows x64 and native Ubuntu
  24.04 ARM64:
  `https://github.com/nauzete/vcpkg-mixxx-stems/actions/runs/30171701454`.
- The first native ARM64 smoke build exposed an ONNX Runtime 1.23.2 static
  CMake export that omitted its public-header directory. Commit `96fd5d8710`
  normalizes the imported target without changing the dependency or runner.
- Phase 3 is complete at Mixxx commit `96fd5d8710` and vcpkg commit
  `5f091759f`.
- No physical hardware test has been run.

### Phase 1 artifacts

| Artifact         |  Artifact ID |              Size |
| ---------------- | -----------: | ----------------: |
| Windows x64 MSI  | `8620176208` | 107,307,752 bytes |
| Ubuntu ARM64 DEB | `8620596995` |  24,817,424 bytes |

Downloaded Ubuntu ARM64 DEB SHA-256:
`921a8eacc4a79adff49787ce659ccdabc2b0b2391d4ed2ecb0ddd2d14bd74f86`.

### Phase 2 model artifact

- Release:
  `https://github.com/nauzete/mixxx-stems/releases/tag/htdemucs-955717e8-onnx-v1`
- Model: `htdemucs`, version/signature `955717e8`
- ONNX opset: 17
- Input: `[1, 2, 343980]` at 44,100 Hz
- Output: `[1, 4, 2, 343980]`
- Stem order: drums, bass, other, vocals
- ONNX size: 304,413,278 bytes
- ONNX SHA-256:
  `db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d`
- Real-audio parity maximum absolute error: `0.0012071133` (limit `0.02`)
- Release assets include the deterministic manifest, parity report, complete
  checksum file, pinned Python environment, and Demucs MIT license notice.

### Environment notes and blockers

- GitHub CLI 2.96.0 was installed during bootstrap because it was absent.
- GitHub CLI authentication could not reach `api.github.com` from its process,
  reliably during initial setup. GitHub CLI/API calls work when supplied the
  token from Git Credential Manager, but DNS resolution remains intermittent.
  The connected GitHub app is installed only for `PenaltyHUB`, so it cannot
  mutate the new personal forks; authenticated API calls are used instead.
- `mixxxdj/vcpkg:main` does not contain the CPU `onnxruntime` port merged by
  PR #194. Created `nauzete/vcpkg-mixxx-stems:feature/onnx-runtime` from
  `mixxxdj/vcpkg:2.7` at
  `1c20f84aa1ffca2ef18a7d9c6bd7cdd1f5f2e265`.
- `BLOCKED_EXTERNAL_HARDWARE`: Raspberry Pi 5, DDJ-FLX4, 10.1-inch touch
  display, thermal/throttling, master/headphone output, underrun, and two-hour
  soak tests require the physical devices.

### Reasoning level

- Current model: GPT-5.6 Sol.
- Current reasoning level: Medium.
- Level changes this session: none.

### Next task

1. Merge the validated Phase 3 component branches into their integration
   branches.
2. Begin Phase 4 on `feature/demucs-runner`.
3. Implement bounded decoding, normalization, segmentation, overlap-add,
   reusable buffers, progress, and cancellation without touching the audio
   thread.
