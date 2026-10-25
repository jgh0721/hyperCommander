"""Read-only, bounded P0 EPUB structure probe; not a renderer or EPUBCheck."""
from __future__ import annotations
import argparse
import hashlib
import io
import json
import posixpath
import re
import stat
import struct
import sys
import urllib.parse
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path

LIMITS = dict(inputBytes=64*1024*1024, entries=4096, entryBytes=16*1024*1024,
              totalBytes=128*1024*1024, compressionRatio=200, xmlBytes=2*1024*1024)
NS = dict(ocf="urn:oasis:names:tc:opendocument:xmlns:container", opf="http://www.idpf.org/2007/opf",
          dc="http://purl.org/dc/elements/1.1/", x="http://www.w3.org/1999/xhtml",
          ncx="http://www.daisy.org/z3986/2005/ncx/")


class Rejected(Exception):
    def __init__(self, code, location=""):
        self.code, self.location = code, location
        super().__init__(code)


def canonical_member(name):
    if not name or name.startswith("/") or any(c in name for c in ("\\", "\x00", ":")):
        raise Rejected("unsafe-entry-path", name)
    parts = name.rstrip("/").split("/")
    if any(p in ("", ".", "..") for p in parts):
        raise Rejected("unsafe-entry-path", name)
    return "/".join(parts)


def resolve(base, href):
    if not href or re.search(r"%(?![0-9a-fA-F]{2})", href):
        raise Rejected("invalid-resource-url", href)
    parsed = urllib.parse.urlsplit(href)
    if parsed.scheme or parsed.netloc or parsed.query:
        raise Rejected("external-resource-url", href)
    try:
        path = urllib.parse.unquote(parsed.path, encoding="utf-8", errors="strict")
    except UnicodeError as error:
        raise Rejected("invalid-resource-url", href) from error
    if path.startswith("/") or any(c in path for c in ("\\", "\x00", ":")):
        raise Rejected("unsafe-resource-path", href)
    result = posixpath.normpath(posixpath.join(posixpath.dirname(base), path)) if path else base
    if result == ".." or result.startswith("../"):
        raise Rejected("unsafe-resource-path", href)
    return canonical_member(result)


def xml(data, location):
    if len(data) > LIMITS["xmlBytes"]:
        raise Rejected("xml-limit", location)
    try:
        text = data.decode("utf-8-sig", errors="strict")
    except UnicodeError as error:
        raise Rejected("p0-xml-encoding-unsupported", location) from error
    if "\x00" in text or re.search(r"<!\s*(DOCTYPE|ENTITY)\b", text, re.I):
        raise Rejected("xml-dtd-or-entity", location)
    declaration = re.match(r"\s*<\?xml\s+[^?]*encoding\s*=\s*['\"]([^'\"]+)", text)
    if declaration and declaration[1].lower() not in ("utf-8", "utf8"):
        raise Rejected("p0-xml-encoding-unsupported", location)
    try:
        return ET.fromstring(text)
    except ET.ParseError as error:
        raise Rejected("invalid-xml", location) from error


def inspect(path: Path, *, snapshot: bytes | None = None):
    report = dict(schemaVersion=1, phase="P0-A", source=str(path.resolve()), status="rejected",
                  issues=[], resources=[], spine=[], images=[], navigation=[], limits=LIMITS,
                  rendererVerified=False, epubCheckExecuted=False,
                  referenceCoverage=["manifest", "spine", "navigation", "xhtml-img-src"],
                  unverified=["CSS URLs and syntax", "SVG references", "srcset/picture", "xml:base", "image decoding",
                              "font obfuscation", "renderer", "OCF writer", "EPUBCheck"])
    def issue(code, location="", severity="error"):
        report["issues"].append(dict(code=code, location=location, severity=severity))
    try:
        # Immutable bounded snapshot; concurrent path replacement cannot mix entries.
        if snapshot is None:
            with path.open("rb") as source:
                data = source.read(LIMITS["inputBytes"]+1)
        else:
            data = snapshot
        if len(data) > LIMITS["inputBytes"]:
            raise Rejected("input-limit")
        report.update(sha256=hashlib.sha256(data).hexdigest(), inputBytes=len(data))
        with zipfile.ZipFile(io.BytesIO(data)) as book:
            entries, by_name, total = book.infolist(), {}, 0
            if len(entries) > LIMITS["entries"]:
                raise Rejected("entry-count-limit")
            for entry in entries:
                if "\x00" in entry.orig_filename:
                    raise Rejected("unsafe-entry-path", "NUL in ZIP entry name")
                name = canonical_member(entry.filename)
                if name in by_name:
                    raise Rejected("duplicate-entry", name)
                by_name[name] = entry
                if entry.flag_bits & 1:
                    raise Rejected("encrypted-zip-entry", name)
                if stat.S_ISLNK(entry.external_attr >> 16):
                    raise Rejected("symlink-entry", name)
                if entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                    raise Rejected("unsupported-zip-method", name)
                total += entry.file_size
                if entry.file_size > LIMITS["entryBytes"] or total > LIMITS["totalBytes"]:
                    raise Rejected("decompressed-size-limit", name)
                if entry.file_size > max(1, entry.compress_size)*LIMITS["compressionRatio"]:
                    raise Rejected("compression-ratio-limit", name)
            def read(name):
                if name not in by_name or by_name[name].is_dir():
                    raise Rejected("missing-resource", name)
                with book.open(by_name[name]) as stream:
                    result = stream.read(LIMITS["entryBytes"]+1)
                if len(result) > LIMITS["entryBytes"] or len(result) != by_name[name].file_size:
                    raise Rejected("resource-size-mismatch", name)
                return result
            if "mimetype" not in by_name:
                raise Rejected("missing-mimetype")
            mime = by_name["mimetype"]
            if mime.header_offset != 0 or mime.compress_type != zipfile.ZIP_STORED:
                raise Rejected("invalid-mimetype-layout")
            if data[:4] != b"PK\x03\x04" or len(data) < 38:
                raise Rejected("invalid-mimetype-header")
            if struct.unpack_from("<HH", data, 26) != (8, 0) or data[30:38] != b"mimetype":
                raise Rejected("invalid-mimetype-header")
            if read("mimetype") != b"application/epub+zip":
                raise Rejected("invalid-mimetype-value")
            container = xml(read("META-INF/container.xml"), "META-INF/container.xml")
            roots = container.findall("ocf:rootfiles/ocf:rootfile", NS)
            if len(roots) != 1:
                raise Rejected("p0-multiple-or-missing-rootfiles")
            if roots[0].get("media-type") != "application/oebps-package+xml":
                raise Rejected("invalid-rootfile-type")
            rootfile = canonical_member(roots[0].get("full-path", ""))
            package = xml(read(rootfile), rootfile)
            if package.tag != "{"+NS["opf"]+"}package":
                raise Rejected("invalid-package-root", rootfile)
            version = package.get("version", "")
            if version not in ("2.0", "3.0"):
                raise Rejected("unsupported-package-version", version)
            report.update(rootfile=rootfile, packageVersion=version,
                          title=package.findtext("opf:metadata/dc:title", "", NS))
            languages = []
            for node in package.findall("opf:metadata/dc:language", NS):
                language = "".join(node.itertext()).strip()
                if len(language) > 128 or len(languages) >= 32:
                    raise Rejected("language-metadata-limit")
                if language and language not in languages:
                    languages.append(language)
            report["languages"] = languages
            for name, code in [("META-INF/encryption.xml", "encryption-needs-classification"),
                               ("META-INF/signatures.xml", "signed-publication")]:
                if name in by_name:
                    xml(read(name), name)
                    issue(code, name, "restriction")
            for node in package.findall("opf:metadata/opf:meta", NS):
                if node.get("property") == "rendition:layout" and (node.text or "").strip() == "pre-paginated":
                    issue("fixed-layout", rootfile, "restriction")
            manifest, paths = {}, set()
            for node in package.findall("opf:manifest/opf:item", NS):
                ident = node.get("id", "")
                if not ident or ident in manifest:
                    raise Rejected("duplicate-or-empty-manifest-id", ident)
                name = resolve(rootfile, node.get("href", ""))
                if name in paths:
                    raise Rejected("duplicate-manifest-path", name)
                paths.add(name)
                content = read(name)
                resource = dict(id=ident, path=name, mediaType=node.get("media-type", ""),
                                bytes=len(content), sha256=hashlib.sha256(content).hexdigest(),
                                properties=node.get("properties", "").split())
                if not resource["mediaType"]:
                    raise Rejected("missing-media-type", name)
                manifest[ident] = resource
                report["resources"].append(resource)
                if "scripted" in resource["properties"]:
                    issue("scripted-content", name, "restriction")
            spine = package.find("opf:spine", NS)
            if spine is None or not spine.findall("opf:itemref", NS):
                raise Rejected("empty-spine", rootfile)
            progression = spine.get("page-progression-direction", "default") or "default"
            if progression not in ("default", "ltr", "rtl"):
                raise Rejected("invalid-reading-progression")
            report["readingProgression"] = progression
            for item in spine.findall("opf:itemref", NS):
                ident = item.get("idref", "")
                if ident not in manifest:
                    raise Rejected("missing-spine-resource", ident)
                resource = manifest[ident]
                report["spine"].append(dict(id=ident, path=resource["path"], linear=item.get("linear", "yes")))
                if "rendition:layout-pre-paginated" in item.get("properties", "").split():
                    issue("fixed-layout-spine", resource["path"], "restriction")
                if resource["mediaType"] != "application/xhtml+xml":
                    issue("p0-spine-media-unsupported", resource["path"], "restriction")
            for resource in report["resources"]:
                if resource["mediaType"] != "application/xhtml+xml":
                    continue
                document = xml(read(resource["path"]), resource["path"])
                if document.findall(".//x:script", NS) or any(
                        attr.lower().startswith("on") for element in document.iter() for attr in element.attrib):
                    issue("active-content", resource["path"], "restriction")
                for index, image in enumerate(document.findall(".//x:img", NS)):
                    target = resolve(resource["path"], image.get("src", ""))
                    if target not in paths:
                        raise Rejected("missing-image-manifest-reference", target)
                    report["images"].append(dict(document=resource["path"], occurrence=index, resource=target,
                        alt=image.get("alt"), altState="missing" if "alt" not in image.attrib else
                        "decorative" if image.get("alt") == "" else "text"))
                if "nav" in resource["properties"]:
                    for nav in document.findall(".//x:nav", NS):
                        if "toc" not in nav.get("{http://www.idpf.org/2007/ops}type", "").split():
                            continue
                        parents = {child: parent for parent in nav.iter() for child in parent}
                        for link in nav.iter():
                            if link.tag not in (f'{{{NS["x"]}}}a', f'{{{NS["x"]}}}span'):
                                continue
                            if link.tag.endswith('}span') and parents.get(link).tag != f'{{{NS["x"]}}}li':
                                continue
                            parent, depth = parents.get(link), -1
                            while parent is not None:
                                depth += parent.tag == f'{{{NS["x"]}}}li'
                                parent = parents.get(parent)
                            if link.tag.endswith('}span'):
                                report["navigation"].append(dict(label="".join(link.itertext()), path="", depth=max(0, depth)))
                                continue
                            target = resolve(resource["path"], link.get("href", ""))
                            if target not in paths:
                                raise Rejected("missing-navigation-target", target)
                            report["navigation"].append(dict(label="".join(link.itertext()), path=target, fragment=urllib.parse.unquote(urllib.parse.urlsplit(link.get("href", "")).fragment), depth=max(0, depth)))
            if version == "2.0":
                nav_id = spine.get("toc", "")
                if nav_id not in manifest:
                    raise Rejected("missing-ncx", nav_id)
                ncx_path = manifest[nav_id]["path"]
                ncx = xml(read(ncx_path), ncx_path)
                parents = {child: parent for parent in ncx.iter() for child in parent}
                for point in ncx.findall(".//ncx:navPoint", NS):
                    content = point.find("ncx:content", NS)
                    if content is None:
                        raise Rejected("invalid-ncx-point", ncx_path)
                    target = resolve(ncx_path, content.get("src", ""))
                    if target not in paths:
                        raise Rejected("missing-navigation-target", target)
                    parent, depth = parents.get(point), 0
                    while parent is not None:
                        depth += parent.tag == f'{{{NS["ncx"]}}}navPoint'
                        parent = parents.get(parent)
                    report["navigation"].append(dict(label=point.findtext("ncx:navLabel/ncx:text", "", NS), path=target, fragment=urllib.parse.unquote(urllib.parse.urlsplit(content.get("src", "")).fragment), depth=depth))
            if not report["navigation"]:
                raise Rejected("missing-navigation")
            report["status"] = "restricted" if report["issues"] else "structure-checked"
    except Rejected as error:
        issue(error.code, error.location)
    except (OSError, zipfile.BadZipFile, RuntimeError, ValueError, NotImplementedError, EOFError, zlib.error) as error:
        issue("archive-or-io-error", type(error).__name__)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("epub", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = inspect(args.epub)
    text = json.dumps(report, ensure_ascii=False, indent=2)+"\n"
    if args.output:
        if args.output.resolve() == args.epub.resolve() or (
                args.output.exists() and args.epub.exists() and args.output.samefile(args.epub)):
            parser.error("report destination is the input EPUB")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stdout.write(text)
    return 2 if report["status"] == "rejected" else 0


if __name__ == "__main__":
    raise SystemExit(main())
