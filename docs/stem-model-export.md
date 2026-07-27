# HTDemucs model export

The runtime model is exported only from the official
[`mixxxdj/demucs`](https://github.com/mixxxdj/demucs) fork. The source commit,
Python version, CPU PyTorch toolchain, ONNX opset, and ONNX Runtime validator
are pinned in `.github/workflows/model-export.yml`.
FFmpeg is installed explicitly to decode the upstream MP3 fixture, and its
resolved version is recorded in the model manifest.

The workflow loads the same official checkpoint/export path and fixes the
runtime segment to 3.9 seconds without padding back to the 7.8-second training
segment. It then applies project-specific validation, including a direct
quality comparison against the official padded path. It does not substitute a
different model or checkpoint.

## Outputs

The workflow uploads an artifact containing:

- `htdemucs.onnx`;
- `model-manifest.json`;
- `parity-report.json`;
- `pip-freeze.txt`;
- `demucs-LICENSE`;
- `SHA256SUMS`.

The ONNX binary is a release artifact and is ignored by Git.

## Validation

The export job:

1. verifies the exact Demucs checkout and its cleanliness;
2. runs the official exporter with deterministic random seeds;
3. runs `onnx.checker.check_model(..., full_check=True)`;
4. verifies fixed input and output tensor shapes;
5. compares the native PyTorch and ONNX-exportable PyTorch paths;
6. compares ONNX Runtime CPU output with PyTorch for deterministic synthetic
   audio and the official Demucs `test.mp3` fixture;
7. records mean, maximum, and root-mean-square errors;
8. verifies all output checksums before upload.

The fixture audio is used only during validation and is not redistributed.
The model artifact must not be committed to the Mixxx repository.
