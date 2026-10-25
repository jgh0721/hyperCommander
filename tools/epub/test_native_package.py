"""Native parser parity and hostile ZIP/XML tests; Python only creates fixtures."""
from __future__ import annotations
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from make_corpus import archive, generate, publication, webp
from prepare_render import prepare

EXECUTABLE = Path(sys.argv.pop(1)).resolve()


def compressed(entries, descriptor=False, legacy=False):
    class ForwardOnly(io.BytesIO):
        def seekable(self):
            return False
        def seek(self, *_):
            raise io.UnsupportedOperation("forward only")
    stream = ForwardOnly() if descriptor else io.BytesIO()
    with zipfile.ZipFile(stream, "w") as book:
        for name, raw in entries.items():
            book.writestr(name, raw, compress_type=zipfile.ZIP_STORED if name == "mimetype" and not legacy else zipfile.ZIP_DEFLATED)
    if not descriptor or legacy:
        return stream.getvalue()
    data = bytearray(stream.getvalue())
    end = data.rindex(b"PK\x05\x06")
    central = struct.unpack_from("<I", data, end + 16)[0]
    # Mimetype must be descriptor-free. Remove its descriptor and fix offsets.
    first_size = 30 + 8 + len(entries["mimetype"])
    crc, size, raw_size = struct.unpack_from("<III", data, central + 16)
    struct.pack_into("<H", data, 6, 0)
    struct.pack_into("<III", data, 14, crc, size, raw_size)
    struct.pack_into("<H", data, central + 8, 0)
    at = central + 46 + 8
    while data[at:at+4] == b"PK\x01\x02":
        offset = struct.unpack_from("<I", data, at + 42)[0]
        struct.pack_into("<I", data, at + 42, offset - 16)
        names, extra, comment = struct.unpack_from("<HHH", data, at + 28)
        at += 46 + names + extra + comment
    struct.pack_into("<I", data, end + 16, central - 16)
    del data[first_size:first_size+16]
    return bytes(data)


class NativePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hc-epub-native-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.counter = 0

    def run_bytes(self, data, error=None, cancel=0):
        self.counter += 1
        path = self.root / f"case-{self.counter}.epub"
        path.write_bytes(data)
        before = hashlib.sha256(data).digest()
        run = subprocess.run([str(EXECUTABLE), str(path), "--cancel-after", str(cancel)], capture_output=True, timeout=15)
        self.assertEqual(hashlib.sha256(path.read_bytes()).digest(), before)
        self.assertEqual(run.returncode, 2 if error else 0, run.stderr.decode(errors="replace") + run.stdout[:300].decode(errors="replace"))
        result = json.loads(run.stdout)
        if error:
            self.assertEqual(result["error"], error)
            self.assertNotIn("resources", result)
        return result

    def test_corpus_and_python_snapshot_parity(self):
        manifest = generate(self.root / "corpus")
        codes = {"scripted": "renderer-active-markup", "crc-error": "zip-crc"}
        for case in manifest["cases"]:
            with self.subTest(case=case["id"]):
                path = self.root / "corpus" / case["file"]
                error = codes.get(case["id"], case["expectedCode"])
                # The Python research inspector keeps strict EPUB conformance;
                # the native reader also admits safe legacy ZIP layouts.
                if case["id"] == "invalid-spine":
                    native = self.run_bytes(path.read_bytes())
                    self.assertEqual(native["recovery"]["missingSpineItems"], 1)
                    self.assertEqual(len(native["spine"]), 1)
                    continue
                if case["id"] == "compressed-mimetype":
                    native = self.run_bytes(path.read_bytes())
                    self.assertEqual(len(native["spine"]), 2)
                    continue
                native = self.run_bytes(path.read_bytes(), error)
                if not error:
                    oracle = prepare(path)
                    for field in ("schemaVersion", "phase", "policy", "sourceSha256", "title", "languages", "readingProgression", "packageVersion", "rootfile", "spine", "navigation", "images", "resources"):
                        self.assertEqual(native[field], oracle[field], field)

    def test_compression_and_descriptors(self):
        for descriptor in (False, True):
            with self.subTest(descriptor=descriptor):
                result = self.run_bytes(compressed(publication(), descriptor))
                self.assertEqual(len(result["images"]), 4)
                self.assertEqual(len(result["spine"]), 2)
        entries = publication(); entries["EPUB/Styles/book.css"] = b""
        self.run_bytes(compressed(entries))

    def test_legacy_zip_and_passive_doctypes(self):
        entries = publication()
        chapter = "EPUB/Text/second.xhtml"
        declaration = b'<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">'
        entries[chapter] = entries[chapter].replace(b'<html', declaration + b'<html', 1).replace(b'</body>', b'<p>space&nbsp;kept</p></body>')
        base = compressed(entries, descriptor=True, legacy=True)
        for padding in range(17):
            with self.subTest(padding=padding):
                self.run_bytes(base + bytes([padding]) * padding)
        # An ordinary ZIP comment that happens to end in padding is not trimmed.
        data = bytearray(archive(publication()))
        struct.pack_into("<H", data, len(data) - 2, 4)
        self.run_bytes(bytes(data) + b'\x04' * 4)
        maximum = bytearray(archive(publication()))
        struct.pack_into("<H", maximum, len(maximum) - 2, 65535)
        self.run_bytes(bytes(maximum) + b'c' * 65535 + b'\x10' * 16)
        for suffix in (b'x', b'\x04' * 3, b'\x11' * 17, b'\x00' * 4):
            self.run_bytes(base + suffix, "zip-end-missing")
        # CRC enforcement still applies to the compatibility layout.
        broken = bytearray(base)
        central = broken.index(b'PK\x01\x02')
        broken[central + 16] ^= 1
        self.run_bytes(bytes(broken), "zip-descriptor")

    def test_single_nul_terminator(self):
        data = archive(publication())
        self.run_bytes(data + b'\x00')
        for suffix in (b'\x00' * 2, b'\x00' * 16, b'x\x00'):
            self.run_bytes(data + suffix, "zip-end-missing")
        self.run_bytes(data[:-2] + b'\x00', "zip-end-missing")

    def test_legacy_manifest_roles_and_utf8_meta(self):
        entries = publication("2.0")
        opf = entries["EPUB/content.opf"].replace(b'application/x-dtbncx+xml', b'application/xhtml+xml').replace(b'media-type="text/css"', b'media-type="application/xhtml+xml"')
        opf = opf.replace(b'</manifest>', b'<item id="font" href="Fonts/test.ttf" media-type="application/x-font-ttf"/></manifest>')
        entries["EPUB/content.opf"] = opf
        entries["EPUB/Fonts/test.ttf"] = b'font parser fixture'
        chapter = "EPUB/Text/second.xhtml"
        entries[chapter] = entries[chapter].replace(b'<head>', b'<head><meta http-equiv="Content-Type" content="text/html; charset=UTF-8"/>')
        result = self.run_bytes(archive(entries))
        self.assertEqual(result["recovery"]["normalizedMedia"], 3)
        resources = {r["path"]:r for r in result["resources"]}
        self.assertEqual(resources["EPUB/toc.ncx"]["mediaType"], "application/x-dtbncx+xml")
        self.assertEqual(resources["EPUB/Styles/book.css"]["mediaType"], "text/css")
        self.assertEqual(resources["EPUB/Fonts/test.ttf"]["mediaType"], "font/ttf")
        import base64, xml.etree.ElementTree as ET
        normalized = ET.fromstring(base64.b64decode(resources["EPUB/content.opf"]["base64"]))
        media = {r.get("id"):r.get("media-type") for r in normalized.findall("{*}manifest/{*}item")}
        self.assertEqual(media["ncx"], "application/x-dtbncx+xml")
        entries["EPUB/toc.ncx"] = entries[chapter]
        self.run_bytes(archive(entries), "invalid-ncx-root")
        for equiv, content in [(b'refresh', b'0;url=https://invalid.example/'), (b'content-type', b'text/html;charset=windows-1252')]:
            bad = publication()
            bad[chapter] = bad[chapter].replace(b'<head>', b'<head><meta http-equiv="'+equiv+b'" content="'+content+b'"/>')
            self.run_bytes(archive(bad), "renderer-active-markup")

    def test_webp_and_missing_spine_recovery(self):
        entries = publication()
        entries["EPUB/Images/alpha.webp"] = webp()
        del entries["EPUB/Images/alpha.png"]
        for name in list(entries):
            if name.endswith(('.xhtml', '.opf')):
                entries[name] = entries[name].replace(b'alpha.png', b'alpha.webp').replace(b'image/png', b'image/webp')
        result = self.run_bytes(archive(entries))
        self.assertTrue(any(r["mediaType"]=="image/webp" for r in result["resources"]))
        opf = entries["EPUB/content.opf"]
        entries["EPUB/content.opf"] = opf.replace(b'<itemref idref="c2"/>', b'<itemref idref="absent"/><itemref idref="c2" linear="no"/>')
        result = self.run_bytes(archive(entries))
        self.assertEqual(result["recovery"]["missingSpineItems"], 1)
        self.assertTrue(all(i["linear"]=="yes" for i in result["spine"]))
        import base64, xml.etree.ElementTree as ET
        raw = next(r for r in result["resources"] if r["path"]=="EPUB/content.opf")
        reader = ET.fromstring(base64.b64decode(raw["base64"]))
        self.assertEqual([i.get("idref") for i in reader.findall("{*}spine/{*}itemref")], ["c1","c2"])
        entries["EPUB/content.opf"] = opf.replace(b'idref="c1"', b'idref="absent1"').replace(b'idref="c2"', b'idref="absent2"')
        self.run_bytes(archive(entries), "missing-spine-resource")

    def test_passive_epub2_ncx_doctype(self):
        entries = publication("2.0")
        entries["EPUB/toc.ncx"] = b'<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">' + entries["EPUB/toc.ncx"]
        result = self.run_bytes(compressed(entries, descriptor=True, legacy=True) + b'\x04' * 4)
        self.assertEqual(result["packageVersion"], "2.0")
        self.assertEqual(len(result["navigation"]), 2)

    def test_doctype_allowlist_rejects_entities_and_custom_dtd(self):
        chapter = "EPUB/Text/second.xhtml"
        good = b'<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">'
        for declaration in (
            b'<!DOCTYPE html SYSTEM "https://invalid.example/evil.dtd">',
            b'<!DOCTYPE html [<!ENTITY local "EXPANDED">]>',
            good[:-1] + b' [<!ENTITY secret SYSTEM "file:///C:/private">]>',
            b'<!DOCTYPE html [<!ENTITY % remote SYSTEM "https://invalid.example/evil.dtd">%remote;]>',
        ):
            entries = publication()
            entries[chapter] = entries[chapter].replace(b'<html', declaration + b'<html', 1)
            self.run_bytes(archive(entries), "xml-dtd-or-entity")
        for declaration in (b'<!DOCTYPE html>', good.replace(b'"', b"'")):
            entries = publication()
            entries[chapter] = entries[chapter].replace(b'<html', declaration + b'<html', 1)
            self.run_bytes(archive(entries))

    def test_zip_header_mutations(self):
        base = archive(publication())
        central = base.index(b"PK\x01\x02")
        second = central + 46 + len("mimetype")
        local = struct.unpack_from("<I", base, second + 42)[0]
        cases = [
            ("local-name", local + 30, b"X", "zip-local-name"),
            ("local-method", local + 8, struct.pack("<H", 8), "zip-local-header"),
            ("local-size", local + 22, struct.pack("<I", 99), "zip-local-size"),
            ("local-range", second + 42, struct.pack("<I", len(base) - 1), "zip-truncated"),
            ("input-encryption", second + 8, struct.pack("<H", 1), "encrypted-zip-entry"),
            ("unknown-flags", second + 8, struct.pack("<H", 0x40), "zip-flags-unsupported"),
            ("unsupported-method", second + 10, struct.pack("<H", 99), "unsupported-zip-method"),
            ("entry-limit", second + 24, struct.pack("<I", 16*1024*1024+1), "decompressed-size-limit"),
            ("ratio", second + 24, struct.pack("<I", 1024*1024), "compression-ratio-limit"),
            ("symlink", second + 38, struct.pack("<I", 0o120777 << 16), "zip-special-entry"),
            ("disk", len(base) - 18, struct.pack("<H", 1), "zip-multidisk-unsupported"),
            ("count", len(base) - 12, struct.pack("<H", 4097), "zip-multidisk-unsupported"),
        ]
        for name, offset, value, error in cases:
            with self.subTest(case=name):
                data = bytearray(base); data[offset:offset+len(value)] = value
                self.run_bytes(bytes(data), error)
        self.run_bytes(base[:-1], "zip-end-missing")
        self.run_bytes(b"", "zip-end-missing")
        self.run_bytes(b"PK", "zip-end-missing")

    def test_xml_and_reference_policy(self):
        chapter = "EPUB/Text/second.xhtml"
        cases = [
            (chapter, b'</body>', b'<iframe src="file:///c:/private"/></body>', "renderer-active-markup"),
            (chapter, b'<body>', b'<body onclick="alert(1)">', "renderer-active-attribute"),
            (chapter, b'<body>', b'<body xml:base="https://invalid.example/">', "renderer-active-attribute"),
            (chapter, b'</body>', b'<a href="javascript:alert(1)">a</a></body>', "renderer-active-url"),
            (chapter, b'</body>', b'<div>'*65 + b'x' + b'</div>'*65 + b'</body>', "xml-complexity-limit"),
            (chapter, b'</body>', b'\xff</body>', "invalid-utf8"),
            (chapter, b'encoding="UTF-8"', b'encoding="windows-1252"', "xml-encoding-unsupported"),
            ("EPUB/content.opf", b'version="3.0"', b'version="4.0"', "unsupported-package-version"),
            ("EPUB/content.opf", b'Images/street.svg', b'Images/%FF.svg', "invalid-utf8"),
            ("EPUB/content.opf", b'Images/street.svg', b'Images/%ZZ.svg', "invalid-resource-url"),
            ("EPUB/nav.xhtml", b'Text/second.xhtml#start', b'Text/second.xhtml?query=1', "external-resource-url"),
            ("EPUB/Images/street.svg", b'</svg>', b'<script>alert(1)</script></svg>', "renderer-active-markup"),
        ]
        for path, old, new, error in cases:
            with self.subTest(path=path, error=error):
                entries = publication(); self.assertIn(old, entries[path]); entries[path] = entries[path].replace(old, new)
                self.run_bytes(archive(entries), error)
        entries = publication(); entries["epub/content.opf"] = b"duplicate"
        self.run_bytes(archive(entries), "duplicate-entry")
        for path in ("/root.txt", "a/./b.txt", "a//b.txt", "a:b.txt"):
            entries = publication(); entries[path] = b"invalid"
            self.run_bytes(archive(entries), "unsafe-entry-path")
        # ZipInfo normalizes backslashes on Windows; mutate both on-disk names.
        entries = publication(); entries["a/b.txt"] = b"invalid"
        self.run_bytes(archive(entries).replace(b"a/b.txt", b"a\\b.txt"), "unsafe-entry-path")

    def test_resource_budget_and_xml_complexity(self):
        entries = publication()
        for index in range(3):
            entries[f"EPUB/Images/large{index}.png"] = b"x" * (11 * 1024 * 1024)
            item = f'<item id="large{index}" href="Images/large{index}.png" media-type="image/png"/>'.encode()
            entries["EPUB/content.opf"] = entries["EPUB/content.opf"].replace(b'</manifest>', item + b'</manifest>')
        self.run_bytes(archive(entries), "renderer-snapshot-limit")
        entries = publication()
        entries["EPUB/Text/second.xhtml"] = entries["EPUB/Text/second.xhtml"].replace(b'</body>', b'<p/>' * 32768 + b'</body>')
        self.run_bytes(archive(entries), "xml-complexity-limit")

    def test_nested_local_overlap_and_deflate_size(self):
        import zlib
        entries = publication()
        name, raw = b"hidden", b"abc"
        local = struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, 0, 0, 0, 0, zlib.crc32(raw), len(raw), len(raw), len(name), 0) + name + raw
        entries["EPUB/Styles/book.css"] = local
        entries["hidden"] = raw
        data = bytearray(archive(entries))
        with zipfile.ZipFile(io.BytesIO(data)) as book:
            css = book.getinfo("EPUB/Styles/book.css")
            embedded = css.header_offset + 30 + len(css.filename.encode())
        central = data.index(b'PK\x01\x02')
        while data[central:central+4] == b'PK\x01\x02':
            n, x, c = struct.unpack_from('<HHH', data, central + 28)
            if data[central+46:central+46+n] == name:
                struct.pack_into('<I', data, central + 42, embedded)
                break
            central += 46+n+x+c
        self.run_bytes(bytes(data), "zip-overlap")
        data = bytearray(compressed(publication()))
        central = data.index(b'PK\x01\x02')
        while data[central:central+4] == b'PK\x01\x02':
            n, x, c = struct.unpack_from('<HHH', data, central + 28)
            if data[central+46:central+46+n] == b'EPUB/Styles/book.css':
                offset = struct.unpack_from('<I', data, central + 42)[0]
                struct.pack_into('<I', data, offset + 22, 2)
                struct.pack_into('<I', data, central + 24, 2)
                break
            central += 46+n+x+c
        self.run_bytes(bytes(data), "zip-inflate-size")

    def test_cancellation_and_input_limit(self):
        for checkpoint in (1, 5, 50, 200):
            with self.subTest(checkpoint=checkpoint):
                self.run_bytes(compressed(publication()), "cancelled", checkpoint)
        path = self.root / "large.epub"
        with path.open("wb") as stream:
            stream.truncate(64*1024*1024+1)
        run = subprocess.run([str(EXECUTABLE), str(path)], capture_output=True, timeout=15)
        self.assertEqual(run.returncode, 2)
        self.assertEqual(json.loads(run.stdout)["error"], "input-limit")

    def test_repeated_reference_budget(self):
        entries = publication()
        entries["EPUB/content.opf"] = entries["EPUB/content.opf"].replace(b'<itemref idref="c1"/>', b'<itemref idref="c1"/>' * 4097)
        self.run_bytes(archive(entries), "snapshot-record-limit")
        entries = publication()
        long_name = 'Text/' + 'x' * 1800 + '.xhtml'
        entries['EPUB/' + long_name] = entries.pop('EPUB/Text/첫 장.xhtml')
        for path in ('EPUB/content.opf', 'EPUB/nav.xhtml'):
            entries[path] = entries[path].replace('Text/첫%20장.xhtml'.encode(), long_name.encode())
        entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'<itemref idref="c1"/>', b'<itemref idref="c1"/>' * 3000)
        self.run_bytes(archive(entries), "snapshot-metadata-limit")
        entries = publication(); entries['x' * 4097] = b'too long'
        self.run_bytes(archive(entries), "entry-path-limit")

    def test_mixed_navigation_text(self):
        entries = publication()
        entries['EPUB/nav.xhtml'] = entries['EPUB/nav.xhtml'].replace('돌아오는 길</a>'.encode(), '돌아<span>오는</span> 길</a>'.encode())
        result = self.run_bytes(archive(entries))
        self.assertEqual(result['navigation'][1]['label'], '돌아오는 길')

    def test_language_and_reading_progression(self):
        entries = publication()
        entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'<dc:language>ko</dc:language>', b'<dc:language>ar</dc:language><dc:language>en</dc:language>')
        entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'<spine>', b'<spine page-progression-direction="rtl">')
        result = self.run_bytes(archive(entries))
        self.assertEqual(result['languages'], ['ar', 'en'])
        self.assertEqual(result['readingProgression'], 'rtl')
        oracle = prepare(self.root / f'case-{self.counter}.epub')
        self.assertEqual(result['languages'], oracle['languages'])
        self.assertEqual(result['readingProgression'], oracle['readingProgression'])
        entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'page-progression-direction="rtl"', b'page-progression-direction="up"')
        self.run_bytes(archive(entries), 'invalid-reading-progression')
        for metadata in (b'<dc:language>' + b'x' * 129 + b'</dc:language>',
                         b''.join(f'<dc:language>x-{i}</dc:language>'.encode() for i in range(33))):
            entries = publication()
            entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'<dc:language>ko</dc:language>', metadata)
            self.run_bytes(archive(entries), 'language-metadata-limit')
        entries = publication()
        entries['EPUB/content.opf'] = entries['EPUB/content.opf'].replace(b'<dc:language>ko</dc:language>', b'')
        result = self.run_bytes(archive(entries))
        self.assertEqual(result['languages'], [])
        self.assertEqual(result['readingProgression'], 'default')


if __name__ == "__main__":
    unittest.main(verbosity=2)
