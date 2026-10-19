"""Summarize measured boundaries without ranking unequal-quality kernels as equivalent."""
import csv
import importlib.metadata
import json
from pathlib import Path
import sys

ROOT = Path(sys.argv[1])
KERNELS = {
    "qt": "Qt SmoothTransformation", "wic": "WIC Fant", "stb": "stbir sRGB: Mitchell down / Catmull-Rom up",
    "pillow": "Lanczos3 encoded light", "opencv": "Lanczos4 encoded light straight alpha",
    "opencv-linear": "Lanczos4 linear light premultiplied alpha",
    "vips": "Lanczos3 encoded light premultiplied alpha", "vips-linear": "Lanczos3 linear light premultiplied alpha",
    "scipy": "cubic B-spline", "skimage": "cubic B-spline plus antialiasing",
    "magickcore": "ImageMagick Lanczos3", "swscale": "libswscale Lanczos via pipes",
    "libplacebo": "requested kernel; libplacebo renderer defaults via Vulkan and pipes",
}


def collect():
    rows = []
    for folder in [ROOT] + ([ROOT / "extended"] if (ROOT / "extended").exists() else []):
        for report in (folder / "results").glob("*.jsonl"):
            for line in report.read_text(encoding="utf-8").splitlines():
                if not line.strip():
                    continue
                row = json.loads(line)
                row["cohort"] = "primary" if folder == ROOT else "extended"
                row["actual_kernel"] = row.get("actual_kernel", KERNELS.get(row["engine"], row.get("filter", "decoder")))
                row["peak_memory_bytes"] = row.get("peak_working_set", row.get("process_peak_bytes"))
                row["memory_scope"] = row.get("memory_scope", "process-cumulative" if row["peak_memory_bytes"] else "unmeasured")
                rows.append(row)
    columns = ["cohort", "engine", "id", "operation", "actual_kernel", "p50_ms", "p95_ms", "first_frame_ms", "shared_renderer_init_ms", "first_operation_ms", "peak_memory_bytes", "memory_scope", "output_bits_per_pixel", "output_mode", "output_dtype", "output_shape", "error"]
    with (ROOT / "comparison.csv").open("w", encoding="utf-8", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    packages = {}
    for name in ("numpy", "pillow", "opencv-python-headless", "scipy", "scikit-image", "pyvips", "imagecodecs", "pillow-heif", "OpenImageIO", "skia-python"):
        try:
            packages[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            pass
    summary = dict(rows=len(rows), engines=sorted({r["engine"] for r in rows}), packages=packages,
                   errors=[dict(engine=r["engine"], case=r["id"], error=r["error"]) for r in rows if r.get("error")],
                   notes=["Resize inputs are canonical RGBA8; conversion and alpha/gamma semantics vary by backend.",
                          "FFmpeg rows include pipe transfer, filter-graph conversion, synchronization and result download.",
                          "Process high-water memory includes prior cases and artifact encoding; use isolated cases for allocation comparisons.",
                          "First-frame results include process startup where explicitly named; warm p50 values do not establish viewer startup latency.",
                          "The primary dataset is synthetic, and the extended natural image comes from libheif's public example."])
    summary["notes"] += ["Decoder pixel comparisons use Qt as a reference and normalize only an opaque alpha channel; ICC policy may still differ.",
                         "Automatic kernel selection changed for pixel-rounded fit sizes, so auto rows do not assert identical kernels.",
                         "Unsupported formats and native decoder process failures remain explicit errors; they are not successful measurements."]
    summary["notes"] += ["Decoder PNG artifacts use RGB/gray normalization outside timed decode; raw output format/depth may differ.",
                         "Native tags include thread and implementation-phase variants, not distinct libraries."]
    comparisons = []
    for cohort, case, before, after in [
        ("primary", "large-0.25-lanczos3", "reference", "native-final"),
        ("extended", "decode-24mp-avif", "avif-exact-t2", "avif-yuv-t4"),
        ("extended", "48mp-0.25-ewaRobidouxSharp", "native-final", "gpu"),
    ]:
        candidates = {r["engine"]: r for r in rows if r["cohort"] == cohort and r["id"] == case}
        current = next((name for name in ("native-color", "native-lifetime", "native-final", "native") if name in candidates), None)
        if before == "native-final":
            before = current
        if after == "native-final":
            after = current
        if before in candidates and after in candidates:
            a, b = candidates[before], candidates[after]
            comparisons.append(dict(cohort=cohort, case=case, before=before, after=after,
                                    before_p50_ms=a["p50_ms"], after_p50_ms=b["p50_ms"],
                                    ratio=a["p50_ms"] / b["p50_ms"] if b["p50_ms"] else None,
                                    scope="component; see raw row boundary and quality/precision reports"))
    summary["selected_comparisons"] = comparisons
    (ROOT / "comparison-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return rows


def avif_precision():
    import imagecodecs
    import numpy as np
    result = []
    for folder in [ROOT, ROOT / "extended"]:
        if not folder.exists():
            continue
        for path in (folder / "results").glob("avif-exact-t2-*.png"):
            other = path.with_name(path.name.replace("avif-exact-t2-", "avif-yuv-t2-", 1))
            if not other.exists():
                continue
            a, b = imagecodecs.png_decode(path.read_bytes()), imagecodecs.png_decode(other.read_bytes())
            delta = np.abs(a.astype(np.int64) - b.astype(np.int64))
            result.append(dict(case=path.name.removeprefix("avif-exact-t2-"), dtype=str(a.dtype), max_channel=int(delta.max()), mae=float(delta.mean())))
    (ROOT / "avif-precision.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return result


def decode_validation():
    import imagecodecs
    import numpy as np
    rows = []
    for folder in [ROOT, ROOT / "extended"]:
        if not folder.exists():
            continue
        matrix = json.loads((folder / "matrix.json").read_text(encoding="utf-8"))
        for case in matrix:
            if case["operation"] != "decode":
                continue
            reference = folder / "results" / ("qt-" + case["id"] + ".png")
            if not reference.exists():
                continue
            a = imagecodecs.png_decode(reference.read_bytes())
            for path in (folder / "results").glob("*-" + case["id"] + ".png"):
                engine = path.stem[:-(len(case["id"]) + 1)]
                b = imagecodecs.png_decode(path.read_bytes())
                row = dict(cohort=folder.name, case=case["id"], engine=engine,
                           reference_shape=list(a.shape), output_shape=list(b.shape), reference_dtype=str(a.dtype), output_dtype=str(b.dtype))
                # Normalize the harmless opaque-alpha channel difference, while
                # retaining orientation and high-bit precision mismatches.
                x, y = a, b
                if x.ndim == y.ndim == 3 and {x.shape[2], y.shape[2]} == {3, 4}:
                    if x.shape[2] == 4 and np.all(x[..., 3] == np.iinfo(x.dtype).max):
                        x = x[..., :3]
                    if y.shape[2] == 4 and np.all(y[..., 3] == np.iinfo(y.dtype).max):
                        y = y[..., :3]
                row["same_shape"] = x.shape == y.shape
                if x.shape == y.shape:
                    delta = np.abs(x.astype(np.float64) / np.iinfo(x.dtype).max - y.astype(np.float64) / np.iinfo(y.dtype).max)
                    row.update(max_normalized_channel=float(delta.max()), normalized_mae=float(delta.mean()))
                rows.append(row)
    (ROOT / "decode-validation.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")


rows = collect()
precision = avif_precision()
decode_validation()
print(json.dumps(dict(rows=len(rows), engines=len({r["engine"] for r in rows}), avif_precision=precision), indent=2))
