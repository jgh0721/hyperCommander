"""Additional in-process engines. Kernel names are recorded explicitly."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("engine", choices=["skia", "oiio", "simd"])
    parser.add_argument("--vendor-root", type=Path)
    args = parser.parse_args()
    import numpy as np
    import psutil
    from PIL import Image
    if args.engine == "skia":
        import skia
    elif args.engine == "oiio":
        import OpenImageIO as oiio
        oiio.attribute("threads", 4)
    else:
        simd = ctypes.CDLL(str((args.vendor_root or args.root) / "simd-build/Release/Simd.dll"))
        simd.SimdResizerInit.argtypes = [ctypes.c_size_t] * 5 + [ctypes.c_int] * 2
        simd.SimdResizerInit.restype = ctypes.c_void_p
        simd.SimdResizerRun.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        simd.SimdRelease.argtypes = [ctypes.c_void_p]
    with (args.root / "results" / f"{args.engine}-resize.jsonl").open("w", encoding="utf-8") as report:
        for case in json.loads((args.root / "matrix.json").read_text(encoding="utf-8")):
            if case["operation"] != "resize" or case["filter"] != "lanczos3":
                continue
            source = np.asarray(Image.open(args.root / "corpus" / case["file"]).convert("RGBA")).copy()
            w, h = case["width"], case["height"]
            times, error, result = [], "", None
            for n in range(-1, case.get("runs", 5)):
                started = time.perf_counter()
                try:
                    if args.engine == "skia":
                        image = skia.Image.fromarray(source, colorType=skia.kRGBA_8888_ColorType,
                                                     alphaType=skia.kUnpremul_AlphaType, colorSpace=skia.ColorSpace.MakeSRGB())
                        out = image.resize(w, h, skia.SamplingOptions(skia.CubicResampler.Mitchell()))
                        result = out.toarray(colorType=skia.kRGBA_8888_ColorType, alphaType=skia.kUnpremul_AlphaType)
                    elif args.engine == "oiio":
                        image = oiio.ImageBuf(source.astype(np.float32) / 255)
                        image = oiio.ImageBufAlgo.premult(image)
                        out = oiio.ImageBufAlgo.resize(image, filtername="lanczos3", roi=oiio.ROI(0, w, 0, h, 0, 1, 0, 4), nthreads=4)
                        out = oiio.ImageBufAlgo.unpremult(out)
                        result = out.get_pixels(oiio.UINT8)
                        if out.has_error:
                            raise ValueError(out.geterror())
                    else:
                        # SimdResizeChannelByte=0, SimdResizeMethodBicubic=6.
                        # Verify the enum values against the pinned SimdLib.h.
                        context = simd.SimdResizerInit(source.shape[1], source.shape[0], w, h, 4, 0, 6)
                        if not context:
                            raise ValueError("SimdResizerInit unsupported combination")
                        try:
                            result = np.empty((h, w, 4), dtype=np.uint8)
                            simd.SimdResizerRun(context, source.ctypes.data, source.strides[0], result.ctypes.data, result.strides[0])
                        finally:
                            simd.SimdRelease(context)
                    if n >= 0:
                        times.append((time.perf_counter() - started) * 1000)
                except Exception as exc:
                    error = repr(exc)
                    break
            row = dict(id=case["id"], engine=args.engine, operation="resize", actual_kernel={"skia": "Mitchell cubic", "oiio": "Lanczos3", "simd": "bicubic"}[args.engine],
                       boundary="RGBA8 memory to RGBA8 memory; conversions included", samples_ms=times,
                       p50_ms=float(np.median(times)) if times else 0, p95_ms=float(np.percentile(times, 95)) if times else 0,
                       process_peak_bytes=psutil.Process().memory_info().peak_wset, error=error)
            report.write(json.dumps(row) + "\n")
            report.flush()
            if not error:
                Image.fromarray(result).save(args.root / "results" / f"{args.engine}-{case['id']}.png")


if __name__ == "__main__":
    main()
