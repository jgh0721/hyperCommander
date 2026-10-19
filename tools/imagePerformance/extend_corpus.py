"""Natural image, 48 MP, large AVIF, 16-bit and JPEG metadata conditions."""
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image
import pillow_heif
import tifffile

root = Path(sys.argv[1])
extra = root / "extended"
corpus = extra / "corpus"
corpus.mkdir(parents=True, exist_ok=True)
(extra / "results").mkdir(exist_ok=True)
pillow_heif.register_heif_opener()
natural = Image.open(root / "corpus/example.heic").convert("RGBA")
natural.save(corpus / "natural.png")
matrix = []
for kernel in ("bilinear", "bicubicCatmullRom", "mitchellNetravali", "lanczos3", "spline16", "spline36", "spline64", "ewaRobidoux", "ewaRobidouxSharp"):
    matrix.append(dict(id="natural-0.5-" + kernel, file="natural.png", width=round(natural.width / 2), height=round(natural.height / 2), filter=kernel, operation="resize", runs=5))
w, h = 8000, 6000
x, y = np.ogrid[:w, :h]
large = np.empty((h, w, 4), dtype=np.uint8)
large[..., 0] = (x.T * 3 + y.T * 2) % 256
large[..., 1] = ((x.T // 19 ^ y.T // 23) * 71) % 256
large[..., 2] = (x.T + y.T * 7) % 256
large[..., 3] = 255
Image.fromarray(large).save(corpus / "48mp.png")
for kernel in ("lanczos3", "spline64", "ewaRobidouxSharp"):
    matrix.append(dict(id="48mp-0.25-" + kernel, file="48mp.png", width=2000, height=1500, filter=kernel, operation="resize", runs=3, reference=False))
del large
photo = Image.open(root / "corpus/photo.png").convert("RGB")
photo.save(corpus / "progressive.jpg", progressive=True, quality=90)
photo.convert("CMYK").save(corpus / "cmyk.jpg", quality=90)
for name in ("progressive.jpg", "cmyk.jpg"):
    matrix.append(dict(id="decode-" + name, file=name, width=640, height=427, operation="decode", runs=7))
rgb = np.asarray(photo).astype(np.uint16) * 257
rgb[..., 0] = (rgb[..., 0].astype(np.uint32) + np.arange(rgb.shape[1])[None, :] % 193).clip(0, 65535)
tifffile.imwrite(corpus / "rgb16.tiff", rgb, photometric="rgb")
matrix.append(dict(id="decode-rgb16", file="rgb16.tiff", width=640, height=427, operation="decode", runs=7))
for name, image in (("large-avif", photo), ("24mp-avif", Image.open(root / "corpus/large.png").convert("RGB"))):
    image.save(corpus / (name + ".avif"), quality=75, speed=9, max_threads=4, tile_rows=1, tile_cols=1)
    matrix.append(dict(id="decode-" + name, file=name + ".avif", width=640, height=427, operation="decode", runs=7))
photo.save(corpus / "한글 파일 01.png")
matrix.append(dict(id="decode-unicode", file="한글 파일 01.png", width=640, height=427, operation="decode", runs=7))
matrix.append(dict(id="fit-rounding-auto", file="natural.png", width=1000, height=round(natural.height * 1000 / natural.width), filter="auto", operation="resize", runs=5))
(extra / "matrix.json").write_text(json.dumps(matrix, indent=2, ensure_ascii=False), encoding="utf-8")
