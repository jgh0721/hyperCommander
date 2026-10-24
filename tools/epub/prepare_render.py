"""Prepare a bounded, passive publication snapshot for developer renderer comparison."""
from __future__ import annotations
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import zipfile
from package_probe import LIMITS, Rejected, inspect, xml

ALLOWED = {"application/xhtml+xml", "text/css", "image/svg+xml", "image/png", "image/jpeg",
           "image/gif", "font/woff", "font/woff2", "font/ttf", "font/otf",
           "application/x-dtbncx+xml"}
FORBIDDEN = {"script", "iframe", "frame", "frameset", "object", "embed", "applet", "form",
             "base", "foreignobject", "animate", "animatetransform", "animatemotion", "set"}
RENDER_LIMIT = 32 * 1024 * 1024


def prepare(path: Path):
    with path.open("rb") as source:
        data = source.read(LIMITS["inputBytes"] + 1)
    report = inspect(path, snapshot=data)
    if report["status"] != "structure-checked":
        raise Rejected("renderer-requires-passive-reflowable", report["issues"])
    entries = []
    total = 0
    with zipfile.ZipFile(io.BytesIO(data)) as book:
        resources = report["resources"] + [dict(path=report["rootfile"], mediaType="application/oebps-package+xml")]
        for resource in resources:
            name, media = resource["path"], resource["mediaType"]
            if media not in ALLOWED and name != report["rootfile"]:
                raise Rejected("renderer-media-unsupported", name)
            raw = book.read(name)
            total += len(raw)
            if total > RENDER_LIMIT:
                raise Rejected("renderer-snapshot-limit")
            if media in {"application/xhtml+xml", "image/svg+xml"}:
                root = xml(raw, name)
                for node in root.iter():
                    tag = node.tag.rsplit("}", 1)[-1].lower()
                    if tag in FORBIDDEN or (tag == "meta" and node.get("http-equiv")):
                        raise Rejected("renderer-active-markup", name)
                    for key, value in node.attrib.items():
                        local = key.rsplit("}", 1)[-1].lower()
                        if local.startswith("on") or key == "{http://www.w3.org/XML/1998/namespace}base":
                            raise Rejected("renderer-active-attribute", name)
                        if local in {"href", "src", "action"} and value.strip().lower().startswith(("javascript:", "vbscript:", "data:text/")):
                            raise Rejected("renderer-active-url", name)
            entries.append(dict(path=name, mediaType=media, bytes=len(raw),
                                sha256=hashlib.sha256(raw).hexdigest(), base64=base64.b64encode(raw).decode("ascii")))
    return dict(schemaVersion=1, phase="P0-B", policy="passive-reflowable-v1", sourceSha256=report["sha256"],
                title=report["title"], languages=report["languages"], readingProgression=report["readingProgression"],
                packageVersion=report["packageVersion"], rootfile=report["rootfile"],
                spine=report["spine"], navigation=report["navigation"], images=report["images"], resources=entries,
                limitations=["developer snapshot, not product parser", "UTF-8 XML only", "no editing or saving",
                             "fixed layout, encrypted, signed and active publications rejected"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("epub", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        result = prepare(args.epub)
        # Exclusive creation also protects the input, symlinks and existing output.
        with args.output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(result, stream, ensure_ascii=False, separators=(",", ":"))
    except (Rejected, OSError) as error:
        parser.exit(2, str(error) + "\n")


if __name__ == "__main__":
    main()
