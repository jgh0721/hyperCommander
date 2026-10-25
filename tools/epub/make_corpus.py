"""Generate original deterministic P0 EPUB fixtures in a new directory."""
from __future__ import annotations
import argparse
import base64
import copy
import hashlib
import io
import json
import struct
import warnings
import zipfile
import zlib
from pathlib import Path

LICENSE = "CC0-1.0"


def webp():
    # Original 8x8 solid-color image, encoded losslessly with libwebp.
    return base64.b64decode("UklGRh4AAABXRUJQVlA4TBEAAAAvB8ABAAdQt8oUtP+BiOh/AAA=")


def png():
    def chunk(kind, data):
        return struct.pack(">I", len(data))+kind+data+struct.pack(">I", zlib.crc32(kind+data))
    return (b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 1, 8, 6, 0, 0, 0))+
            chunk(b"IDAT", zlib.compress(b"\x00\x33\x66\x99\xff\x00\x00\x00\x00"))+chunk(b"IEND", b""))


def publication(version="3.0"):
    nav_item = ('<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
                if version == "3.0" else '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>')
    modified = '<meta property="dcterms:modified">2026-10-23T00:00:00Z</meta>' if version == "3.0" else ''
    opf = f'''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="{version}" unique-identifier="book-id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="book-id">urn:uuid:0dcaaf1c-fdcb-4b75-9d68-847c05c46b91</dc:identifier>
<dc:title>골목과 산책 — EPUB 검증 자료</dc:title><dc:language>ko</dc:language>{modified}</metadata>
<manifest><item id="c1" href="Text/첫%20장.xhtml" media-type="application/xhtml+xml"/>
<item id="c2" href="Text/second.xhtml" media-type="application/xhtml+xml"/>
<item id="css" href="Styles/book.css" media-type="text/css"/>
<item id="image" href="Images/street.svg" media-type="image/svg+xml"/>
<item id="png" href="Images/alpha.png" media-type="image/png"/>{nav_item}</manifest>
<spine{' toc="ncx"' if version == '2.0' else ''}><itemref idref="c1"/><itemref idref="c2"/></spine></package>'''
    def chapter(title, alt):
        return f'''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ko"><head><title>{title}</title>
<link rel="stylesheet" type="text/css" href="../Styles/book.css"/></head>
<body><h1 id="start">{title}</h1><p>나무 그늘에서 잠시 쉬었다가 골목을 따라 걸었다.
천천히 걸으며 작은 집과 나뭇잎을 살펴보았다. 긴 문장이 다음 줄로 이어질 때에도
글자 사이와 줄 사이의 간격이 편안하게 유지되는지 확인한다.</p>
<p>새 문단에서는 앞 문단과의 간격을 살핀다. 행간을 바꾸더라도 문단 위아래의 설정은 독립적으로 유지된다.</p>
<div><img src="../Images/street.svg" alt="{alt}"/><p>그림: 골목의 나무</p></div>
<p><img src="../Images/alpha.png" alt=""/>장식과 본문</p></body></html>'''
    result = {
        "mimetype": b"application/epub+zip",
        "META-INF/container.xml": b'<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="EPUB/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
        "EPUB/content.opf": opf.encode(),
        # Deliberately not in spine order.
        "EPUB/Text/second.xhtml": chapter("돌아오는 길", "돌아오는 길에서 본 나무").encode(),
        "EPUB/Text/첫 장.xhtml": chapter("첫 산책", "집 앞 골목의 나무").encode(),
        "EPUB/Styles/book.css": b'body{line-height:1.7}img{max-width:100%;height:auto}p{color:#303840!important}',
        "EPUB/Images/street.svg": b'<svg xmlns="http://www.w3.org/2000/svg" width="600" height="360" viewBox="0 0 600 360"><rect width="600" height="360" fill="#e9dfc6"/><path d="M250 360L300 180L350 360" fill="#a8b9a0"/><path d="M170 250V130" stroke="#796f57" stroke-width="12"/><circle cx="170" cy="110" r="60" fill="#7f9d79"/></svg>',
        "EPUB/Images/alpha.png": png(),
    }
    if version == "3.0":
        result["EPUB/nav.xhtml"] = '''<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>목차</title></head><body><nav epub:type="toc"><ol><li><a href="Text/첫%20장.xhtml#start">첫 산책</a></li><li><a href="Text/second.xhtml#start">돌아오는 길</a></li></ol></nav></body></html>'''.encode()
    else:
        result["EPUB/toc.ncx"] = '''<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:uid" content="urn:uuid:0dcaaf1c-fdcb-4b75-9d68-847c05c46b91"/><meta name="dtb:depth" content="1"/><meta name="dtb:totalPageCount" content="0"/><meta name="dtb:maxPageNumber" content="0"/></head><docTitle><text>골목과 산책</text></docTitle><navMap><navPoint id="n1" playOrder="1"><navLabel><text>첫 산책</text></navLabel><content src="Text/첫%20장.xhtml#start"/></navPoint><navPoint id="n2" playOrder="2"><navLabel><text>돌아오는 길</text></navLabel><content src="Text/second.xhtml#start"/></navPoint></navMap></ncx>'''.encode()
    return result


def archive(entries, duplicate=False, compressed_mimetype=False, corrupt=False):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as book, warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        for name, data in list(entries.items())+([("EPUB/content.opf", entries["EPUB/content.opf"])] if duplicate else []):
            info = zipfile.ZipInfo(name, (2026, 10, 23, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED if compressed_mimetype and name == "mimetype" else zipfile.ZIP_STORED
            book.writestr(info, data)
    result = stream.getvalue()
    if corrupt:
        # Change stored payload while retaining its original CRC in both ZIP headers.
        pos = result.index(b'body{line-height:1.7}')
        result = result[:pos]+b'X'+result[pos+1:]
    return result


def generate(output: Path):
    output.mkdir(parents=True, exist_ok=False)
    cases = []
    def add(ident, entries, expected="structure-checked", code=None, **options):
        data = archive(entries, **options)
        (output/(ident+".epub")).write_bytes(data)
        cases.append(dict(id=ident, file=ident+".epub", sha256=hashlib.sha256(data).hexdigest(),
                          expectedStatus=expected, expectedCode=code, bytes=len(data)))
    base = publication()
    add("epub3-images", base)
    add("epub2-ncx", publication("2.0"))
    def mutation(ident, key, old, new, code, status="rejected"):
        entries = copy.deepcopy(base)
        assert old in entries[key]
        entries[key] = entries[key].replace(old, new)
        add(ident, entries, status, code)
    mutation("fixed-layout", "EPUB/content.opf", b'</metadata>', b'<meta property="rendition:layout">pre-paginated</meta></metadata>', "fixed-layout", "restricted")
    mutation("scripted", "EPUB/Text/second.xhtml", b'</body>', b'<script>alert(1)</script></body>', "active-content", "restricted")
    mutation("missing-image", "EPUB/Text/second.xhtml", b'street.svg', b'absent.svg', "missing-image-manifest-reference")
    mutation("remote-image", "EPUB/Text/second.xhtml", b'../Images/street.svg', b'https://invalid.example/image.svg', "external-resource-url")
    mutation("encoded-escape", "EPUB/content.opf", b'Images/street.svg', b'%2e%2e/%2e%2e/escape.svg', "unsafe-resource-path")
    mutation("invalid-spine", "EPUB/content.opf", b'idref="c2"', b'idref="unknown"', "missing-spine-resource")
    mutation("duplicate-id", "EPUB/content.opf", b'id="c2"', b'id="c1"', "duplicate-or-empty-manifest-id")
    mutation("dtd-entity", "EPUB/Text/second.xhtml", b'<html', b'<!DOCTYPE html [<!ENTITY secret SYSTEM "file:///C:/private">]><html', "xml-dtd-or-entity")
    mutation("missing-nav-target", "EPUB/nav.xhtml", b'Text/second.xhtml', b'Text/absent.xhtml', "missing-navigation-target")
    mutation("missing-nav", "EPUB/nav.xhtml", b'epub:type="toc"', b'epub:type="landmarks"', "missing-navigation")
    mutation("malformed-xml", "EPUB/content.opf", b'</package>', b'</broken>', "invalid-xml")
    mutation("bad-mimetype", "mimetype", b'application/epub+zip', b'application/zip', "invalid-mimetype-value")
    add("duplicate-entry", base, "rejected", "duplicate-entry", duplicate=True)
    add("compressed-mimetype", base, "rejected", "invalid-mimetype-layout", compressed_mimetype=True)
    add("crc-error", base, "rejected", "archive-or-io-error", corrupt=True)
    for ident, name, data, status, code in [
        ("zip-traversal", "../outside.txt", b'outside', "rejected", "unsafe-entry-path"),
        ("font-encryption", "META-INF/encryption.xml", b'<encryption/>', "restricted", "encryption-needs-classification"),
        ("signed", "META-INF/signatures.xml", b'<signatures/>', "restricted", "signed-publication")]:
        entries = copy.deepcopy(base)
        entries[name] = data
        add(ident, entries, status, code)
    manifest = dict(schemaVersion=1, origin="Original synthetic text and vector/raster artwork generated by HyperCommander EPUB P0 tools",
                    license=LICENSE, attribution="HyperCommander EPUB P0 fixtures; no third-party book content",
                    reproducibility="Fixed member order, timestamps, stored bytes; Python/zlib version recorded by test run",
                    epubCheckExecuted=False, cases=cases)
    (output/"manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    print(json.dumps(generate(arguments.output), ensure_ascii=True, indent=2))
