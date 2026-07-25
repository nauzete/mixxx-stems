# HTDemucs model export

The runtime model is exported only from the official
[`mixxxdj/demucs`](https://github.com/mixxxdj/demucs) fork. The source commit,
Python version, CPU PyTorch toolchain, ONNX opset, and ONNX Runtime validator
are pinned in `.github/workflows/model-export.yml`.

The workflow calls the upstream `scripts/convert-pth-to-onnx.py` exporter and
then applies project-specific validation. It does not substitute a different
model or exporter.

## Outputs

The workflow uploads an artifact containing:

- `htdemucs.onnx`;
- `model-manifest.json`;
- `parity-report.json`;
- `pip-freeze.txt`;
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
