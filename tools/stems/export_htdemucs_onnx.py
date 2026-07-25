#!/usr/bin/env python3

"""Export and validate the official mixxxdj/demucs HTDemucs ONNX model."""

import argparse
import contextlib
import hashlib
import importlib.metadata
import json
import math
from pathlib import Path
import platform
import runpy
import subprocess
import sys
from typing import Any

import numpy as np
import onnx
import onnxruntime as ort
import torch
from torch.nn import functional as F

MODEL_NAME = "htdemucs"
MODEL_SIGNATURE = "955717e8"
MODEL_CHECKPOINT = "955717e8-8726e21a.th"
MODEL_CHECKPOINT_URL = (
    "https://dl.fbaipublicfiles.com/demucs/"
    f"hybrid_transformer/{MODEL_CHECKPOINT}"
)
OPSET_VERSION = 17
DEFAULT_OVERLAP = 0.25
EXPECTED_SOURCES = ["drums", "bass", "other", "vocals"]
MANIFEST_SCHEMA_VERSION = 1
FORMAT_VERSION = 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run the pinned mixxxdj/demucs exporter, validate ONNX Runtime "
            "parity, and emit a deterministic manifest."
        )
    )
    parser.add_argument(
        "--demucs-dir",
        type=Path,
        required=True,
        help="Checked-out mixxxdj/demucs source directory",
    )
    parser.add_argument(
        "--demucs-commit",
        required=True,
        help="Expected full mixxxdj/demucs commit SHA",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="Empty output directory for model and reports",
    )
    parser.add_argument(
        "--mean-absolute-tolerance",
        type=float,
        default=1e-4,
        help="Maximum permitted mean absolute parity error",
    )
    parser.add_argument(
        "--max-absolute-tolerance",
        type=float,
        default=2e-2,
        help="Maximum permitted absolute parity error",
    )
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_git(demucs_dir: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(demucs_dir), *args],
        text=True,
        encoding="utf-8",
    ).strip()


def validate_source_checkout(
    demucs_dir: Path, expected_commit: str
) -> dict[str, str]:
    source_commit = run_git(demucs_dir, "rev-parse", "HEAD")
    if source_commit != expected_commit:
        raise RuntimeError(
            "Demucs checkout mismatch: "
            f"expected {expected_commit}, got {source_commit}"
        )
    if run_git(
        demucs_dir,
        "status",
        "--porcelain",
        "--untracked-files=no",
    ):
        raise RuntimeError(
            "Tracked files in the Demucs checkout must be clean before export"
        )
    return {
        "commit": source_commit,
        "commit_date": run_git(
            demucs_dir, "show", "-s", "--format=%cI", "HEAD"
        ),
        "repository": "https://github.com/mixxxdj/demucs",
    }


@contextlib.contextmanager
def scoped_argv(argv: list[str]):
    original = sys.argv
    sys.argv = argv
    try:
        yield
    finally:
        sys.argv = original


def run_official_exporter(demucs_dir: Path, output_dir: Path) -> Path:
    exporter = demucs_dir / "scripts" / "convert-pth-to-onnx.py"
    if not exporter.is_file():
        raise FileNotFoundError(
            f"Official Demucs exporter not found: {exporter}"
        )

    torch.manual_seed(0)
    np.random.seed(0)
    torch.set_num_threads(2)
    with scoped_argv([str(exporter), str(output_dir)]):
        runpy.run_path(str(exporter), run_name="__main__")

    model_path = output_dir / f"{MODEL_NAME}.onnx"
    if not model_path.is_file() or model_path.stat().st_size == 0:
        raise RuntimeError(
            "The official Demucs exporter did not produce "
            "a non-empty ONNX model"
        )
    return model_path


def load_core_model():
    from demucs.htdemucs import HTDemucs
    from demucs.pretrained import get_model

    model = get_model(MODEL_NAME)
    if isinstance(model, HTDemucs):
        core_model = model
    elif (
        hasattr(model, "models")
        and len(model.models) == 1
        and isinstance(model.models[0], HTDemucs)
    ):
        core_model = model.models[0]
    else:
        raise TypeError(
            f"Unsupported {MODEL_NAME} model type: {type(model)!r}"
        )
    core_model.eval()
    if list(core_model.sources) != EXPECTED_SOURCES:
        raise RuntimeError(
            f"Unexpected source order: {core_model.sources!r}; "
            f"expected {EXPECTED_SOURCES!r}"
        )
    return core_model


def onnx_tensor_shape(value_info: Any) -> list[int | str | None]:
    shape: list[int | str | None] = []
    for dimension in value_info.type.tensor_type.shape.dim:
        if dimension.HasField("dim_value"):
            shape.append(dimension.dim_value)
        elif dimension.HasField("dim_param"):
            shape.append(dimension.dim_param)
        else:
            shape.append(None)
    return shape


def load_and_check_onnx(model_path: Path) -> tuple[Any, dict[str, Any]]:
    model = onnx.load(str(model_path), load_external_data=True)
    onnx.checker.check_model(model, full_check=True)
    if len(model.graph.input) != 1 or len(model.graph.output) != 1:
        raise RuntimeError(
            "Expected exactly one ONNX input and one ONNX output"
        )

    input_info = model.graph.input[0]
    output_info = model.graph.output[0]
    input_shape = onnx_tensor_shape(input_info)
    output_shape = onnx_tensor_shape(output_info)
    if input_shape[:2] != [1, 2] or not isinstance(input_shape[-1], int):
        raise RuntimeError(f"Unexpected ONNX input shape: {input_shape!r}")
    if len(output_shape) != 4:
        raise RuntimeError(f"Unexpected ONNX output shape: {output_shape!r}")
    for actual, expected in zip(output_shape[:3], [1, 4, 2]):
        if isinstance(actual, int) and actual != expected:
            raise RuntimeError(
                f"Unexpected ONNX output shape: {output_shape!r}"
            )
    if (
        isinstance(output_shape[-1], int)
        and output_shape[-1] != input_shape[-1]
    ):
        raise RuntimeError(
            "Input/output sample count mismatch: "
            f"{input_shape!r} -> {output_shape!r}"
        )

    metadata = {
        "input": {
            "name": input_info.name,
            "shape": input_shape,
            "dtype": "float32",
        },
        "output": {
            "name": output_info.name,
            "shape": output_shape,
            "dtype": "float32",
        },
        "opsets": [
            {"domain": item.domain or "ai.onnx", "version": item.version}
            for item in model.opset_import
        ],
        "producer_name": model.producer_name,
        "producer_version": model.producer_version,
    }
    return model, metadata


def normalize_like_demucs_api(waveform: torch.Tensor) -> torch.Tensor:
    reference = waveform.mean(0)
    return (waveform - reference.mean()) / (reference.std() + 1e-8)


def synthetic_fixture(samples: int, sample_rate: int) -> torch.Tensor:
    time = torch.arange(samples, dtype=torch.float32) / sample_rate
    left = 0.35 * torch.sin(2 * math.pi * 220 * time) + 0.15 * torch.sin(
        2 * math.pi * 880 * time
    )
    right = 0.30 * torch.sin(2 * math.pi * 330 * time) + 0.10 * torch.sin(
        2 * math.pi * 1760 * time
    )
    return normalize_like_demucs_api(torch.stack([left, right])).unsqueeze(0)


def real_audio_fixture(
    demucs_dir: Path,
    samples: int,
    sample_rate: int,
) -> tuple[torch.Tensor, dict[str, Any]]:
    from demucs.audio import AudioFile

    fixture_path = demucs_dir / "test.mp3"
    if not fixture_path.is_file():
        raise FileNotFoundError(
            f"Official Demucs audio fixture missing: {fixture_path}"
        )
    waveform = AudioFile(fixture_path).read(
        streams=0,
        samplerate=sample_rate,
        channels=2,
    )
    if waveform.shape[-1] < samples:
        waveform = F.pad(waveform, (0, samples - waveform.shape[-1]))
    else:
        waveform = waveform[..., :samples]
    fixture_metadata = {
        "repository_path": "test.mp3",
        "sha256": sha256_file(fixture_path),
        "use": "validation only; not redistributed",
    }
    return normalize_like_demucs_api(waveform).unsqueeze(0), fixture_metadata


def torch_inference(
    core_model, fixture: torch.Tensor, onnx_exportable: bool
) -> np.ndarray:
    core_model.onnx_exportable = onnx_exportable
    with torch.inference_mode():
        result = core_model(fixture)
    return result.detach().cpu().numpy()


def create_ort_session(model_path: Path) -> ort.InferenceSession:
    options = ort.SessionOptions()
    options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    options.intra_op_num_threads = 2
    options.inter_op_num_threads = 1
    options.graph_optimization_level = (
        ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    )
    return ort.InferenceSession(
        str(model_path),
        sess_options=options,
        providers=["CPUExecutionProvider"],
    )


def error_metrics(
    reference: np.ndarray, candidate: np.ndarray
) -> dict[str, Any]:
    if reference.shape != candidate.shape:
        raise RuntimeError(
            "Parity shape mismatch: "
            f"{reference.shape!r} != {candidate.shape!r}"
        )
    difference = np.abs(
        reference.astype(np.float64) - candidate.astype(np.float64)
    )
    return {
        "max_absolute_error": float(difference.max()),
        "mean_absolute_error": float(difference.mean()),
        "root_mean_square_error": float(np.sqrt(np.mean(difference**2))),
        "reference_shape": list(reference.shape),
    }


def validate_metrics(
    name: str,
    metrics: dict[str, Any],
    mean_tolerance: float,
    max_tolerance: float,
) -> None:
    if (
        metrics["mean_absolute_error"] > mean_tolerance
        or metrics["max_absolute_error"] > max_tolerance
    ):
        raise RuntimeError(
            f"{name} parity exceeded tolerance: {metrics!r}; "
            f"mean <= {mean_tolerance}, max <= {max_tolerance}"
        )


def run_parity(
    core_model,
    session: ort.InferenceSession,
    input_name: str,
    fixtures: dict[str, torch.Tensor],
    mean_tolerance: float,
    max_tolerance: float,
) -> dict[str, Any]:
    results: dict[str, Any] = {}
    for fixture_name, fixture in fixtures.items():
        pytorch_onnx_path = torch_inference(
            core_model,
            fixture,
            onnx_exportable=True,
        )
        onnxruntime_output = session.run(
            None,
            {input_name: fixture.cpu().numpy()},
        )[0]
        ort_metrics = error_metrics(pytorch_onnx_path, onnxruntime_output)
        validate_metrics(
            f"{fixture_name}: PyTorch ONNX path vs ONNX Runtime",
            ort_metrics,
            mean_tolerance,
            max_tolerance,
        )
        fixture_results: dict[str, Any] = {
            "pytorch_onnx_path_vs_onnxruntime": ort_metrics
        }
        if fixture_name == "synthetic":
            pytorch_native = torch_inference(
                core_model,
                fixture,
                onnx_exportable=False,
            )
            native_metrics = error_metrics(pytorch_native, pytorch_onnx_path)
            validate_metrics(
                "synthetic: native PyTorch vs PyTorch ONNX path",
                native_metrics,
                mean_tolerance,
                max_tolerance,
            )
            fixture_results["pytorch_native_vs_pytorch_onnx_path"] = (
                native_metrics
            )
        results[fixture_name] = fixture_results
    return results


def distribution_versions() -> dict[str, str]:
    packages = [
        "demucs",
        "numpy",
        "onnx",
        "onnxruntime",
        "torch",
        "torchaudio",
    ]
    return {name: importlib.metadata.version(name) for name in packages}


def write_json(path: Path, data: Any) -> None:
    path.write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def write_pip_freeze(path: Path) -> None:
    result = subprocess.check_output(
        [sys.executable, "-m", "pip", "freeze", "--all"],
        text=True,
        encoding="utf-8",
    )
    lines = sorted(
        (line.strip() for line in result.splitlines() if line.strip()),
        key=str.casefold,
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    args = parse_args()
    demucs_dir = args.demucs_dir.resolve()
    output_dir = args.output_dir.resolve()
    source = validate_source_checkout(demucs_dir, args.demucs_commit)

    output_dir.mkdir(parents=True, exist_ok=True)
    if any(output_dir.iterdir()):
        raise RuntimeError(f"Output directory must be empty: {output_dir}")

    sys.path.insert(0, str(demucs_dir))
    model_path = run_official_exporter(demucs_dir, output_dir)
    _, onnx_metadata = load_and_check_onnx(model_path)

    core_model = load_core_model()
    sample_rate = int(core_model.samplerate)
    segment_seconds = float(core_model.segment)
    input_samples = onnx_metadata["input"]["shape"][-1]
    expected_samples = int(segment_seconds * sample_rate)
    if input_samples != expected_samples:
        raise RuntimeError(
            f"Export segment mismatch: ONNX uses {input_samples} samples, "
            f"model metadata requires {expected_samples}"
        )

    real_fixture, real_fixture_metadata = real_audio_fixture(
        demucs_dir,
        input_samples,
        sample_rate,
    )
    fixtures = {
        "synthetic": synthetic_fixture(input_samples, sample_rate),
        "official_demucs_test_mp3": real_fixture,
    }
    session = create_ort_session(model_path)
    parity = run_parity(
        core_model,
        session,
        onnx_metadata["input"]["name"],
        fixtures,
        args.mean_absolute_tolerance,
        args.max_absolute_tolerance,
    )
    runtime_output_shape = [1, len(EXPECTED_SOURCES), 2, input_samples]
    for fixture_name, fixture_results in parity.items():
        metrics = fixture_results["pytorch_onnx_path_vs_onnxruntime"]
        if metrics["reference_shape"] != runtime_output_shape:
            raise RuntimeError(
                f"{fixture_name} produced unexpected runtime output shape: "
                f"{metrics['reference_shape']!r}"
            )
    onnx_metadata["output"]["runtime_shape"] = runtime_output_shape

    parity_report_path = output_dir / "parity-report.json"
    parity_report = {
        "fixtures": parity,
        "tolerances": {
            "max_absolute_error": args.max_absolute_tolerance,
            "mean_absolute_error": args.mean_absolute_tolerance,
        },
    }
    write_json(parity_report_path, parity_report)

    pip_freeze_path = output_dir / "pip-freeze.txt"
    write_pip_freeze(pip_freeze_path)

    checkpoint_path = (
        Path(torch.hub.get_dir()) / "checkpoints" / MODEL_CHECKPOINT
    )
    if not checkpoint_path.is_file():
        raise FileNotFoundError(
            f"Downloaded model checkpoint missing: {checkpoint_path}"
        )

    exporter_path = demucs_dir / "scripts" / "convert-pth-to-onnx.py"
    model_sha256 = sha256_file(model_path)
    manifest = {
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "format_version": FORMAT_VERSION,
        "model": {
            "name": MODEL_NAME,
            "signature": MODEL_SIGNATURE,
            "sample_rate": sample_rate,
            "audio_channels": int(core_model.audio_channels),
            "segment": {
                "samples": input_samples,
                "seconds": segment_seconds,
            },
            "overlap": DEFAULT_OVERLAP,
            "logical_stem_order": EXPECTED_SOURCES,
            "tensor_index_to_stem": {
                str(index): source_name
                for index, source_name in enumerate(EXPECTED_SOURCES)
            },
            "normalization": {
                "pre_model": (
                    "mono_reference = stereo.mean(channel); "
                    "(stereo - mono_reference.mean()) / "
                    "(mono_reference.std() + 1e-8)"
                ),
                "post_model": (
                    "output * (mono_reference.std() + 1e-8) "
                    "+ mono_reference.mean()"
                ),
                "model_internal": (
                    "HTDemucs forward normalization remains enabled"
                ),
            },
            "checkpoint": {
                "filename": MODEL_CHECKPOINT,
                "sha256": sha256_file(checkpoint_path),
                "url": MODEL_CHECKPOINT_URL,
            },
            "license": {
                "identifier": "MIT",
                "source": (
                    "https://github.com/mixxxdj/demucs/blob/"
                    f"{source['commit']}/LICENSE"
                ),
            },
        },
        "onnx": {
            **onnx_metadata,
            "filename": model_path.name,
            "opset": OPSET_VERSION,
            "sha256": model_sha256,
            "size_bytes": model_path.stat().st_size,
        },
        "export": {
            "command": (
                "python tools/stems/export_htdemucs_onnx.py "
                "--demucs-dir _deps/demucs "
                f"--demucs-commit {source['commit']} "
                "--output-dir model-export-output"
            ),
            "export_date": source["commit_date"].split("T", maxsplit=1)[0],
            "reproducible_timestamp_source": "Demucs commit date",
            "source": source,
            "official_exporter": {
                "path": "scripts/convert-pth-to-onnx.py",
                "sha256": sha256_file(exporter_path),
            },
            "python": platform.python_version(),
            "operating_system": "ubuntu-24.04-x86_64",
            "packages": distribution_versions(),
            "random_seed": 0,
        },
        "validation": {
            "onnx_checker_full_check": True,
            "onnxruntime_execution_provider": "CPUExecutionProvider",
            "onnxruntime_intra_op_threads": 2,
            "onnxruntime_inter_op_threads": 1,
            "onnxruntime_execution_mode": "sequential",
            "parity_report": parity_report_path.name,
            "real_audio_fixture": real_fixture_metadata,
        },
    }

    manifest_path = output_dir / "model-manifest.json"
    write_json(manifest_path, manifest)

    checksummed_paths = [
        model_path,
        manifest_path,
        parity_report_path,
        pip_freeze_path,
    ]
    checksums_path = output_dir / "SHA256SUMS"
    checksums_path.write_text(
        "".join(
            f"{sha256_file(path)}  {path.name}\n" for path in checksummed_paths
        ),
        encoding="utf-8",
        newline="\n",
    )

    print(f"Validated model: {model_path}")
    print(f"Model SHA-256: {model_sha256}")
    print(json.dumps(parity_report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
