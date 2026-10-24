"""Admission and snapshot tests for the P0 renderer resource boundary."""
import base64
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import make_corpus
import package_probe
import prepare_render


class RenderPreparationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hc-epub-render-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def prepare_entries(self, entries):
        path = self.root/"book.epub"
        path.write_bytes(make_corpus.archive(entries))
        return prepare_render.prepare(path)

    def test_same_snapshot_and_exact_resource_bytes(self):
        entries = make_corpus.publication()
        result = self.prepare_entries(entries)
        self.assertEqual(len(result["spine"]), 2)
        for resource in result["resources"]:
            raw = base64.b64decode(resource["base64"])
            self.assertEqual(raw, entries[resource["path"]])
            self.assertEqual(hashlib.sha256(raw).hexdigest(), resource["sha256"])
        snapshot = make_corpus.archive(entries)
        with patch.object(Path, "open", side_effect=AssertionError("snapshot reopened")):
            self.assertEqual(package_probe.inspect(self.root/"absent.epub", snapshot=snapshot)["status"], "structure-checked")

    def test_corpus_only_admits_two_passive_books(self):
        manifest = make_corpus.generate(self.root/"corpus")
        for case in manifest["cases"]:
            path = self.root/"corpus"/case["file"]
            if case["id"] in {"epub3-images", "epub2-ncx"}:
                self.assertTrue(prepare_render.prepare(path)["resources"])
            else:
                with self.subTest(case=case["id"]), self.assertRaises(package_probe.Rejected):
                    prepare_render.prepare(path)

    def test_svg_active_content_and_xml_base_rejected(self):
        for payload in [b'<script>alert(1)</script>', b'<foreignObject/>', b'<set attributeName="href"/>',
                        b'<g onload="alert(1)"/>', b'<g xml:base="file:///C:/"/>']:
            entries = make_corpus.publication()
            entries["EPUB/Images/street.svg"] = b'<svg xmlns="http://www.w3.org/2000/svg">'+payload+b'</svg>'
            with self.subTest(payload=payload), self.assertRaises(package_probe.Rejected):
                self.prepare_entries(entries)

    def test_forms_and_embedded_markup_rejected(self):
        for markup in [b'<iframe srcdoc="&lt;script&gt;alert(1)&lt;/script&gt;"/>', b'<form/>',
                       b'<base href="https://example.invalid/"/>', b'<meta http-equiv="refresh" content="0"/>']:
            entries = make_corpus.publication()
            name = "EPUB/Text/second.xhtml"
            entries[name] = entries[name].replace(b"</body>", markup+b"</body>")
            with self.subTest(markup=markup), self.assertRaises(package_probe.Rejected):
                self.prepare_entries(entries)


if __name__ == "__main__":
    unittest.main(verbosity=2)
