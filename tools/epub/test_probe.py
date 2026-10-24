"""P0 package admission, immutable-input and reproducible-corpus regression tests."""
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

import make_corpus
import package_probe


class EpubProbeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="hc-epub-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.corpus = self.root/"corpus"
        self.manifest = make_corpus.generate(self.corpus)

    def test_corpus_expectations_and_original_bytes(self):
        for case in self.manifest["cases"]:
            with self.subTest(case=case["id"]):
                path = self.corpus/case["file"]
                before = path.read_bytes()
                report = package_probe.inspect(path)
                self.assertEqual(report["status"], case["expectedStatus"], report)
                if case["expectedCode"]:
                    self.assertIn(case["expectedCode"], [item["code"] for item in report["issues"]])
                self.assertEqual(path.read_bytes(), before)
                self.assertEqual(report["sha256"], case["sha256"])
                self.assertFalse(report["rendererVerified"])
                self.assertFalse(report["epubCheckExecuted"])

    def test_spine_order_shared_images_and_alt_scope(self):
        for name in ["epub3-images.epub", "epub2-ncx.epub"]:
            report = package_probe.inspect(self.corpus/name)
            self.assertEqual([s["path"] for s in report["spine"]], ["EPUB/Text/첫 장.xhtml", "EPUB/Text/second.xhtml"])
            self.assertEqual(len(report["navigation"]), 2)
            shared = [i for i in report["images"] if i["resource"].endswith("street.svg")]
            self.assertEqual(len(shared), 2)
            self.assertNotEqual(shared[0]["alt"], shared[1]["alt"])
            self.assertEqual(len([i for i in report["images"] if i["altState"] == "decorative"]), 2)

    def test_reproducible_bytes(self):
        other = self.root/"second"
        manifest = make_corpus.generate(other)
        self.assertEqual(self.manifest, manifest)
        for case in manifest["cases"]:
            self.assertEqual((self.corpus/case["file"]).read_bytes(), (other/case["file"]).read_bytes())

    def test_limits_before_decompression(self):
        path = self.corpus/"epub3-images.epub"
        for key in ["inputBytes", "entries", "entryBytes", "totalBytes", "xmlBytes"]:
            with self.subTest(limit=key), patch.dict(package_probe.LIMITS, {key: 1}):
                self.assertEqual(package_probe.inspect(path)["status"], "rejected")
        bomb = self.root/"ratio.epub"
        with zipfile.ZipFile(bomb, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("long.txt", b'x'*100000)
        self.assertEqual(package_probe.inspect(bomb)["issues"][0]["code"], "compression-ratio-limit")

    def test_url_resolution_and_case_identity(self):
        self.assertEqual(package_probe.resolve("EPUB/Text/ch.xhtml", "../Images/a.svg#x"), "EPUB/Images/a.svg")
        self.assertEqual(package_probe.resolve("EPUB/content.opf", "Text/첫%20장.xhtml"), "EPUB/Text/첫 장.xhtml")
        self.assertNotEqual(package_probe.resolve("EPUB/a", "Image.svg"), package_probe.resolve("EPUB/a", "image.svg"))
        for href in ["../../outside", "/root", "file:///C:/x", "//example/x", "a%00b", "a%5cb", "bad%zz", "C:/x", "a?x=1"]:
            with self.subTest(href=href), self.assertRaises(package_probe.Rejected):
                package_probe.resolve("EPUB/a", href)

    def test_missing_alt_is_not_decorative(self):
        entries = make_corpus.publication()
        entries["EPUB/Text/second.xhtml"] = entries["EPUB/Text/second.xhtml"].replace(b' alt=""', b'')
        path = self.root/"missing-alt.epub"
        path.write_bytes(make_corpus.archive(entries))
        report = package_probe.inspect(path)
        self.assertEqual(report["status"], "structure-checked")
        self.assertEqual(len([i for i in report["images"] if i["altState"] == "missing"]), 1)

    def test_cli_does_not_overwrite_input_or_existing_report(self):
        path = self.corpus/"epub3-images.epub"
        before = hashlib.sha256(path.read_bytes()).hexdigest()
        script = str(Path(package_probe.__file__))
        result = subprocess.run([sys.executable, script, str(path), "--output", str(path)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), before)
        report_path = self.root/"existing.json"
        report_path.write_text("keep", encoding="utf-8")
        result = subprocess.run([sys.executable, script, str(path), "--output", str(report_path)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report_path.read_text(), "keep")
        result = subprocess.run([sys.executable, script, str(path)], capture_output=True, check=True)
        self.assertEqual(json.loads(result.stdout)["status"], "structure-checked")


if __name__ == "__main__":
    unittest.main(verbosity=2)
