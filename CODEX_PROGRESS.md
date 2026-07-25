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

| Component | Revision | Synchronization date |
| --- | --- | --- |
| Mixxx | `66f7912343c9938339b9d3b12ed7a9fee38d97f5` | 2026-07-25 |
| Demucs | `d788c1a06876ced89b11d6531f771e5e40204d48` | 2026-07-25 |
| vcpkg | `41f250c938a7af7ee57bb49f30f94c5355d03c2f` | 2026-07-25 |

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
- Component PRs will target `stems-integration` after their first coherent,
  buildable change. GitHub does not allow a PR between identical branch tips.

### Builds and tests

- No Mixxx build has been run yet.
- No package artifact has been generated yet.
- No ONNX model has been exported or committed.
- No physical hardware test has been run.

### Environment notes and blockers

- GitHub CLI 2.96.0 was installed during bootstrap because it was absent.
- GitHub CLI authentication could not reach `api.github.com` from its process,
  while authenticated PowerShell REST calls and the connected GitHub app work.
  Git operations use Git Credential Manager.
- `BLOCKED_EXTERNAL_HARDWARE`: Raspberry Pi 5, DDJ-FLX4, 10.1-inch touch
  display, thermal/throttling, master/headphone output, underrun, and two-hour
  soak tests require the physical devices.

### Reasoning level

- Current model: GPT-5.6 Sol.
- Current reasoning level: Medium.
- Level changes this session: none.

### Next task

1. Establish clean Windows x64 and Ubuntu 24.04 ARM64 baseline build workflows.
2. Add ONNX Runtime to the Mixxx/vcpkg manifests only after baseline builds are
   green.
