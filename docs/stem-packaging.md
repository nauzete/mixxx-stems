# Mixxx Stems packaging

The `Mixxx Stems packages` workflow creates and validates the two supported
distributions:

- `Mixxx-Stems-x64.msi`
- `Mixxx-Stems-x64-portable.zip`
- `mixxx-stems_VERSION_arm64.deb`

Every package is uploaded with a SHA-256 sidecar. A `stems-v*` tag publishes
the already validated files as a GitHub release. A manual dispatch builds the
same files without publishing a release.

## Independent Windows installation

`MIXXX_STEMS_BUILD=ON` gives the MSI a distinct product name, install
directory, upgrade GUID, and application name. The latter makes Qt select a
separate user configuration directory. The stem cache is stored below that
configuration directory, so official Mixxx and Mixxx Stems do not share it.

The portable archive has no installer and may be extracted anywhere. Both
Windows packages include `onnxruntime.dll`; neither requires Visual Studio,
Python, or a separate ONNX Runtime installation. The ONNX Runtime MIT license
is installed with the application licenses.

## Ubuntu ARM64

The DEB is built on the native `ubuntu-24.04-arm` runner. ONNX Runtime is linked
statically, so the target system does not need a separately installed
`libonnxruntime.so`. CI verifies `aarch64`, the Debian `arm64` field, the ELF
machine type, dynamic-library resolution, package installation, and startup.

Install the downloaded package with:

```bash
sudo apt install ./mixxx-stems_VERSION_arm64.deb
```

The target system does not need Python, PyTorch, Conda, a compiler, or a build
toolchain. The model is verified and exercised during packaging, but remains a
separately downloadable release asset instead of being duplicated in every
installer.

## Local branded build

Pass `-DMIXXX_STEMS_BUILD=ON -DONNXRUNTIME=ON -DSTEM=ON` when configuring.
The ONNX Runtime library and headers must be discoverable by CMake. Windows
packaging requires a dynamic ONNX Runtime build so the runtime DLL can be
installed alongside `mixxx.exe`.
