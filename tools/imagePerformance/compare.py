"""Reproducible component comparisons; results never imply viewer first-paint latency."""
import argparse
import importlib.metadata
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import time
import ctypes
import threading
import tempfile
import sys


class MagickCore:
    def __init__(self):
        self.lib = ctypes.CDLL(str(Path(os.environ["HC_IMAGE_DLL_DIRS"].split(os.pathsep)[0]) / "libMagickCore-7.Q16HDRI-10.dll"))
        p, z, i, s = ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_char_p
        signatures = {
            "MagickCoreGenesis": ([s, i], None), "AcquireExceptionInfo": ([], p),
            "DestroyExceptionInfo": ([p], p), "DestroyImage": ([p], p),
            "ConstituteImage": ([z, z, s, i, p, p], p), "ResizeImage": ([p, z, z, i, p], p),
            "ExportImagePixels": ([p, ctypes.c_ssize_t, ctypes.c_ssize_t, z, z, s, i, p, p], i),
        }
        for name, (args, result) in signatures.items():
            f = getattr(self.lib, name)
            f.argtypes, f.restype = args, result
        self.lib.MagickCoreGenesis(None, 0)

    def resize(self, array, size):
        import numpy as np
        exception = self.lib.AcquireExceptionInfo()
        image = output = None
        try:
            image = self.lib.ConstituteImage(array.shape[1], array.shape[0], b"RGBA", 1, array.ctypes.data, exception)
            if not image:
                raise ValueError("MagickCore ConstituteImage")
            output = self.lib.ResizeImage(image, size[0], size[1], 22, exception)
            if not output:
                raise ValueError("MagickCore ResizeImage")
            result = np.empty((size[1], size[0], 4), dtype=np.uint8)
            if not self.lib.ExportImagePixels(output, 0, 0, size[0], size[1], b"RGBA", 1, result.ctypes.data, exception):
                raise ValueError("MagickCore ExportImagePixels")
            return result
        finally:
            if output:
                self.lib.DestroyImage(output)
            if image:
                self.lib.DestroyImage(image)
            self.lib.DestroyExceptionInfo(exception)


def run_ffmpeg(root, engine):
    import numpy as np
    from PIL import Image
    with (root / "results" / f"{engine}-resize.jsonl").open("w", encoding="utf-8") as report:
        for case in json.loads((root / "matrix.json").read_text(encoding="utf-8")):
            if case["operation"] != "resize":
                continue
            f = case["filter"]
            if engine == "swscale" and f != "lanczos3":
                continue
            if engine == "zscale" and f not in ("bilinear", "lanczos3", "spline36"):
                continue
            image = Image.open(root / "corpus" / case["file"]).convert("RGBA")
            payload = image.tobytes()
            w, h, runs = case["width"], case["height"], case.get("runs", 5)
            args = ["ffmpeg", "-hide_banner", "-loglevel", "verbose", "-threads", "4", "-filter_threads", "4"]
            if engine == "libplacebo":
                args += ["-init_hw_device", "vulkan=vk:0", "-filter_hw_device", "vk"]
            args += ["-f", "rawvideo", "-pixel_format", "rgba", "-video_size", f"{image.width}x{image.height}", "-framerate", "30", "-i", "pipe:0"]
            kernel = dict(lanczos3="lanczos", spline16="spline16", spline36="spline36", spline64="spline64", bilinear="bilinear", ewaRobidouxSharp="ewa_robidouxsharp", ewaRobidoux="ewa_robidoux", bicubicCatmullRom="catmull_rom", mitchellNetravali="mitchell", auto="lanczos")[f]
            if engine == "libplacebo":
                graph = f"format=rgba,hwupload,libplacebo=w={w}:h={h}:upscaler={kernel}:downscaler={kernel}:deband=false:format=rgba,hwdownload,format=rgba"
            elif engine == "zscale":
                graph = f"format=gbrap,zscale=w={w}:h={h}:filter={kernel},format=rgba"
            else:
                graph = f"scale=w={w}:h={h}:flags=lanczos"
            args += ["-vf", graph, "-frames:v", str(runs + 1), "-pix_fmt", "rgba", "-f", "rawvideo", "pipe:1"]
            with tempfile.TemporaryFile() as log:
                start = time.perf_counter()
                proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
                def feed():
                    try:
                        for _ in range(runs + 1):
                            proc.stdin.write(payload)
                        proc.stdin.close()
                    except (BrokenPipeError, OSError):
                        pass
                feeder = threading.Thread(target=feed)
                feeder.start()
                times, frame, first, previous = [], b"", 0, start
                for n in range(runs + 1):
                    data = bytearray()
                    while len(data) < w * h * 4:
                        part = proc.stdout.read(w * h * 4 - len(data))
                        if not part:
                            break
                        data.extend(part)
                    now = time.perf_counter()
                    if len(data) != w * h * 4:
                        break
                    if n == 0:
                        first = (now - start) * 1000
                    else:
                        times.append((now - previous) * 1000)
                    previous, frame = now, data
                proc.stdout.close()
                feeder.join(timeout=10)
                try:
                    code = proc.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    code = proc.wait()
                log.seek(0)
                details = log.read().decode("utf-8", errors="replace")
            (root / "results" / f"{engine}-{case['id']}.log").write_text(details, encoding="utf-8")
            error = "" if code == 0 and len(times) == runs else details[-2000:]
            row = dict(id=case["id"], engine=engine, operation="resize", boundary="persistent-process-pipe-to-pipe", first_frame_ms=first, samples_ms=times,
                       p50_ms=float(np.median(times)) if times else 0, p95_ms=float(np.percentile(times, 95)) if times else 0, error=error)
            report.write(json.dumps(row) + "\n")
            report.flush()
            if not error:
                Image.frombytes("RGBA", (w, h), bytes(frame)).save(root / "results" / f"{engine}-{case['id']}.png")


def generate(root):
    import numpy as np
    from PIL import Image

    corpus = root / "corpus"
    corpus.mkdir(parents=True, exist_ok=True)
    matrix = []
    for name, w, h in [("small", 640, 480), ("photo", 1920, 1280),
                       ("large", 6000, 4000), ("webtoon", 900, 12000)]:
        y, x = np.ogrid[:h, :w]
        a = np.empty((h, w, 4), dtype=np.uint8)
        a[..., 0] = (x * 3 + y * 2 + ((x // 19 + y // 23) % 2) * 127) % 256
        a[..., 1] = (x + y * 5) % 256
        a[..., 2] = ((x // 7 ^ y // 11) * 39) % 256
        a[..., 3] = 255
        if name == "small":
            a[..., 3] = (x + y) % 256
        image = Image.fromarray(a)
        image.save(corpus / (name + ".png"))
        filters = ["bilinear", "lanczos3", "spline36", "ewaRobidouxSharp"]
        for ratio in ([.25, .75, 1.5] if name == "small" else [.25, .5]):
            for f in filters:
                matrix.append(dict(id=f"{name}-{ratio}-{f}", file=name + ".png",
                                   width=round(w * ratio), height=round(h * ratio), filter=f,
                                   operation="resize", runs=3 if name in ("large", "webtoon") else 7))
        if name == "photo":
            rgb = image.convert("RGB")
            for ext in ("jpg", "png", "webp", "tiff", "bmp"):
                rgb.save(corpus / ("decode." + ext))
                matrix.append(dict(id="decode-" + ext, file="decode." + ext,
                                   width=640, height=427, operation="decode", runs=7))
            rotated = Image.Exif()
            rotated[274] = 6
            rgb.save(corpus / "orientation.jpg", exif=rotated)
            matrix.append(dict(id="decode-orientation", file="orientation.jpg", width=427,
                               height=640, operation="decode", runs=7))
            image.save(corpus / "depth16.tiff")
            ramp = np.arange(w * h, dtype=np.uint16).reshape(h, w)
            Image.fromarray(ramp).save(corpus / "gray16.png")
            matrix.append(dict(id="decode-gray16", file="gray16.png", width=640, height=427,
                               operation="decode", runs=7))
    sources = root.parent / "image-codecs/sources/libheif-1.23.6/examples"
    for ext in ("heic", "avif"):
        source = sources / ("example." + ext)
        if source.exists():
            shutil.copyfile(source, corpus / source.name)
            matrix.append(dict(id="decode-" + ext, file=source.name, width=640, height=427,
                               operation="decode", runs=7))
    avif_source = root.parent / "image-codecs/sources/libavif-1.4.2/tests/data"
    for source in sorted(avif_source.glob("*.avif")):
        if source.name in {"paris_icc_exif_xmp.avif", "colors-animated-12bpc-keyframes-0-2-3.avif", "colors-animated-8bpc-alpha-exif-xmp.avif", "seine_sdr_gainmap_srgb_icc.avif", "alpha_noispe.avif", "abc_color_irot_alpha_irot.avif"}:
            shutil.copyfile(source, corpus / source.name)
            matrix.append(dict(id="decode-" + source.stem, file=source.name, width=640, height=427, operation="decode", runs=7))
    (root / "matrix.json").write_text(json.dumps(matrix, indent=2), encoding="utf-8")


def run_python(root, engine, operation):
    import numpy as np
    import psutil
    from PIL import Image, ImageOps

    if engine in ("swscale", "zscale", "libplacebo"):
        return run_ffmpeg(root, engine)
    if engine == "magickcore":
        magick = MagickCore()

    if engine.startswith("vips"):
        import pyvips
        pyvips.concurrency_set(int(os.environ.get("HC_BENCH_THREADS", "4"))) if hasattr(pyvips, "concurrency_set") else None
        pyvips.cache_set_max(0)
    if engine.startswith("opencv"):
        import cv2
        cv2.setNumThreads(int(os.environ.get("HC_BENCH_THREADS", "4")))
    if engine == "scipy":
        from scipy.ndimage import zoom
    if engine == "skimage":
        from skimage.transform import resize
    if engine in ("imagecodecs", "spng"):
        import imagecodecs
    if engine == "pillow-heif":
        import pillow_heif
        pillow_heif.register_heif_opener()
    if engine == "wand":
        from wand.image import Image as WandImage

    report = Path(os.environ["HC_IMAGE_REPORT"]) if os.environ.get("HC_IMAGE_REPORT") else root / "results" / f"{engine}-{operation}.jsonl"
    with report.open("w", encoding="utf-8") as output:
        for case in json.loads((root / "matrix.json").read_text(encoding="utf-8")):
            if os.environ.get("HC_IMAGE_CASE") and case["id"] != os.environ["HC_IMAGE_CASE"]:
                continue
            if case["operation"] != operation:
                continue
            if operation == "resize" and case["filter"] != "lanczos3":
                continue
            path = root / "corpus" / case["file"]
            size = (case["width"], case["height"])
            if operation == "resize":
                image = Image.open(path).convert("RGBA")
                array = np.array(image)
            times, error, result = [], "", None
            for n in range(-1, case.get("runs", 5)):
                started = time.perf_counter()
                try:
                    if operation == "decode":
                        if engine in ("pillow", "pillow-heif"):
                            result = ImageOps.exif_transpose(Image.open(path))
                            result.load()
                        elif engine == "opencv":
                            result = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
                            if result is None:
                                raise ValueError("unsupported format")
                        elif engine == "vips":
                            v = pyvips.Image.new_from_file(str(path), access="sequential").autorot()
                            dtype = {"uchar": np.uint8, "ushort": np.uint16, "float": np.float32, "uint": np.uint32, "double": np.float64}[v.format]
                            result = np.frombuffer(v.write_to_memory(), dtype=dtype).reshape(v.height, v.width, v.bands)
                        elif engine == "imagecodecs":
                            options = {"index": 0, "numthreads": 4} if path.suffix.lower() == ".avif" else {}
                            result = imagecodecs.imread(path, **options)
                        elif engine == "spng":
                            if path.suffix.lower() != ".png":
                                continue
                            result = imagecodecs.spng_decode(path.read_bytes())
                        elif engine == "wand":
                            with WandImage(filename=str(path)) as v:
                                result = v.make_blob("RGBA")
                        else:
                            continue
                    elif engine == "magickcore":
                        result = magick.resize(array, size)
                    elif engine == "pillow":
                        result = image.resize(size, Image.Resampling.LANCZOS)
                    elif engine.startswith("opencv"):
                        if engine.endswith("linear"):
                            a = array.astype(np.float32) / 255
                            a[..., :3] = np.where(a[..., :3] <= .04045, a[..., :3] / 12.92,
                                                  ((a[..., :3] + .055) / 1.055) ** 2.4)
                            a[..., :3] *= a[..., 3:4]
                            r = cv2.resize(a, size, interpolation=cv2.INTER_LANCZOS4)
                            alpha = np.clip(r[..., 3:4], 0, 1)
                            rgb = np.clip(np.divide(r[..., :3], alpha, out=np.zeros_like(r[..., :3]), where=alpha > 1e-8), 0, 1)
                            rgb = np.where(rgb <= .0031308, rgb * 12.92, 1.055 * rgb ** (1 / 2.4) - .055)
                            result = np.clip(np.concatenate((rgb, alpha), axis=2) * 255 + .5, 0, 255).astype(np.uint8)
                        else:
                            result = cv2.resize(array, size, interpolation=cv2.INTER_LANCZOS4)
                    elif engine == "scipy":
                        result = zoom(array, (size[1] / array.shape[0], size[0] / array.shape[1], 1), order=3)
                    elif engine == "skimage":
                        result = np.clip(resize(array, (size[1], size[0], 4), order=3,
                                                anti_aliasing=True, preserve_range=True), 0, 255).astype(np.uint8)
                    elif engine.startswith("vips"):
                        v = pyvips.Image.new_from_memory(array.data, array.shape[1], array.shape[0], 4, "uchar").copy(interpretation="srgb")
                        if engine.endswith("linear"):
                            v = v.colourspace("scrgb")
                        v = v.premultiply().resize(size[0] / array.shape[1], vscale=size[1] / array.shape[0], kernel="lanczos3").unpremultiply()
                        if engine.endswith("linear"):
                            v = v.colourspace("srgb")
                        v = v.cast("uchar")
                        result = np.frombuffer(v.write_to_memory(), dtype=np.uint8).reshape(size[1], size[0], 4)
                    elif engine == "wand":
                        with WandImage(blob=image.tobytes(), format="RGBA", width=image.width, height=image.height, depth=8) as v:
                            v.resize(*size, filter="lanczos")
                            result = np.frombuffer(v.make_blob("RGBA"), dtype=np.uint8).reshape(size[1], size[0], 4)
                    else:
                        raise ValueError("unknown engine")
                    if n >= 0:
                        times.append((time.perf_counter() - started) * 1000)
                except Exception as exc:
                    error = repr(exc)
                    break
            if not times and not error:
                continue
            row = dict(id=case["id"], engine=engine, operation=operation, samples_ms=times,
                       p50_ms=float(np.median(times)) if times else 0,
                       p95_ms=float(np.percentile(times, 95)) if times else 0, error=error,
                       process_peak_bytes=psutil.Process().memory_info().peak_wset)
            actual = {"pillow": "Lanczos3 encoded light", "opencv": "Lanczos4 encoded light", "opencv-linear": "Lanczos4 linear premultiplied", "vips": "Lanczos3 encoded premultiplied", "vips-linear": "Lanczos3 linear premultiplied", "scipy": "cubic B-spline", "skimage": "cubic B-spline plus antialiasing", "magickcore": "Lanczos3 encoded light"}
            if operation == "resize":
                row["actual_kernel"] = actual.get(engine, engine)
            if not error and result is not None:
                view = np.asarray(result)
                row["output_mode"] = result.mode if isinstance(result, Image.Image) else v.interpretation if engine == "vips" else "native array"
                if engine == "imagecodecs" and path.suffix.lower() in (".jpg", ".jpeg") and view.ndim == 3 and view.shape[2] == 4:
                    row["output_mode"] = "CMYK"
                row["output_shape"] = list(view.shape)
                row["output_dtype"] = str(view.dtype)
                row["output_bits_per_pixel"] = int(view.dtype.itemsize * 8 * (view.shape[2] if view.ndim == 3 else 1))
            output.write(json.dumps(row) + "\n")
            output.flush()
            if operation == "resize" and not error:
                Image.fromarray(np.asarray(result)).save(root / "results" / f"{engine}-{case['id']}.png")
            elif operation == "decode" and not error and result is not None:
                decoded = np.asarray(result)
                # PNG artifacts are RGB/gray. A CMYK array must not be exported
                # as RGBA: the K component is not an alpha channel.
                if isinstance(result, Image.Image) and result.mode == "CMYK":
                    decoded = np.asarray(result.convert("RGB"))
                elif engine == "vips" and v.interpretation == "cmyk":
                    # The sequential file pipeline was consumed by the timed
                    # read. Normalize the materialized CMYK data, without rereading.
                    decoded = np.asarray(Image.fromarray(decoded, mode="CMYK").convert("RGB"))
                elif engine == "imagecodecs" and path.suffix.lower() in (".jpg", ".jpeg") and decoded.ndim == 3 and decoded.shape[2] == 4:
                    decoded = np.asarray(Image.fromarray(decoded, mode="CMYK").convert("RGB"))
                if engine == "opencv" and decoded.ndim == 3:
                    decoded = decoded[..., [2, 1, 0, 3]] if decoded.shape[2] == 4 else decoded[..., ::-1]
                if decoded.ndim == 3 and decoded.shape[2] == 1:
                    decoded = decoded[..., 0]
                decoded = np.ascontiguousarray(decoded)
                target = root / "results" / f"{engine}-{case['id']}.png"
                if decoded.dtype == np.uint16 and decoded.ndim == 3:
                    import imagecodecs
                    target.write_bytes(imagecodecs.png_encode(decoded))
                elif decoded.dtype in (np.uint8, np.uint16):
                    Image.fromarray(decoded).save(target)


def isolated_decodes(root, engine):
    folder = root / "results" / "isolated"
    folder.mkdir(exist_ok=True)
    with (root / "results" / f"{engine}-decode.jsonl").open("w", encoding="utf-8") as output:
        for case in json.loads((root / "matrix.json").read_text(encoding="utf-8")):
            if case["operation"] != "decode":
                continue
            report = folder / f"{engine}-{case['id']}.jsonl"
            report.unlink(missing_ok=True)
            artifact = root / "results" / f"{engine}-{case['id']}.png"
            artifact.unlink(missing_ok=True)
            env = dict(os.environ, HC_IMAGE_CASE=case["id"], HC_IMAGE_REPORT=str(report))
            error = ""
            try:
                run = subprocess.run([sys.executable, __file__, str(root), "run", "--engine", engine, "--operation", "decode"],
                                     env=env, capture_output=True, timeout=90)
                (folder / f"{engine}-{case['id']}.log").write_bytes(run.stdout + run.stderr)
                if run.returncode:
                    error = f"decoder process exit 0x{run.returncode & 0xffffffff:08x}"
            except subprocess.TimeoutExpired:
                error = "decoder process timeout (90s)"
            if not error and report.exists():
                rows = [json.loads(line) for line in report.read_text(encoding="utf-8").splitlines() if line.strip()]
            else:
                artifact.unlink(missing_ok=True)
                rows = [dict(id=case["id"], engine=engine, operation="decode", error=error or "missing decoder report", samples_ms=[])]
            for row in rows:
                row["isolation"] = "per-case process; startup excluded from component timing"
                output.write(json.dumps(row) + "\n")
            output.flush()


def quality(root):
    import numpy as np
    from PIL import Image
    rows = []
    for ref in (root / "results").glob("reference-*.png"):
        case = ref.stem[len("reference-"):]
        a = np.asarray(Image.open(ref).convert("RGBA")).astype(np.float64)
        for p in (root / "results").glob("*-" + case + ".png"):
            engine = p.stem[:-(len(case) + 1)]
            b = np.asarray(Image.open(p).convert("RGBA")).astype(np.float64)
            if a.shape != b.shape:
                continue
            d = np.abs(a - b)
            # Compare on both dark and light backgrounds: hidden RGB in transparent
            # pixels must not dominate the display-quality decision.
            ca, cb = a[..., :3] * a[..., 3:4] / 255, b[..., :3] * b[..., 3:4] / 255
            dark = np.abs(ca - cb)
            light = np.abs(ca + 255 - a[..., 3:4] - cb - 255 + b[..., 3:4])
            rows.append(dict(case=case, engine=engine, max_channel=float(d.max()),
                             mae=float(d.mean()), composite_mae=float((dark.mean() + light.mean()) / 2)))
    (root / "quality.json").write_text(json.dumps(rows, indent=2), encoding="utf-8")


def main():
    dll_dirs = []
    if os.name == "nt":
        for directory in os.environ.get("HC_IMAGE_DLL_DIRS", "" ).split(os.pathsep):
            if directory:
                dll_dirs.append(os.add_dll_directory(directory))
    p = argparse.ArgumentParser()
    p.add_argument("root", type=Path)
    p.add_argument("action", choices=["generate", "run", "quality", "isolated-decodes"])
    p.add_argument("--engine", default="pillow")
    p.add_argument("--operation", default="resize")
    args = p.parse_args()
    if args.action == "generate":
        generate(args.root)
    elif args.action == "run":
        run_python(args.root, args.engine, args.operation)
    elif args.action == "isolated-decodes":
        isolated_decodes(args.root, args.engine)
    else:
        quality(args.root)


if __name__ == "__main__":
    main()
