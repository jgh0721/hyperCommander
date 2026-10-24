"""Negative admission checks with small synthetic inventories; no process launch."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from qualify_runtime import audit, digest


class QualificationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'runtime'
        self.source = Path(self.temp.name) / 'source'
        (self.root / 'bundle').mkdir(parents=True)
        self.source.mkdir()
        (self.source / 'package-lock.json').write_bytes(b'{}')
        (self.source / 'reader.js').write_bytes(b'fixture')
        for name in ('hc_epub_render_poc.exe', 'QtWebEngineProcess.exe', 'bundle/reader.js', 'bundle/THIRD-PARTY-NOTICES.txt'):
            (self.root / name).write_bytes(b'fixture')
        bundle = dict(packageLockSha256=digest(self.source / 'package-lock.json'), dependencies={},
                      sourceHashes={'reader.js': digest(self.source / 'reader.js')},
                      files=[dict(path='reader.js', sha256=digest(self.root / 'bundle/reader.js'))])
        (self.root / 'bundle/bundle.json').write_text(json.dumps(bundle), encoding='utf-8')
        self.data = dict(schemaVersion=2, diagnosticOnly=True, releaseEligible=False,
                         profile='epub-passive-read-only-poc', protocolVersion=1, helper='hc_epub_render_poc.exe',
                         bundleManifestSha256=digest(self.root / 'bundle/bundle.json'),
                         files=[dict(path=p.relative_to(self.root).as_posix(), sha256=digest(p))
                                for p in self.root.rglob('*') if p.is_file()])

    def run_audit(self):
        manifest = self.root / 'poc-runtime.json'
        manifest.write_text(json.dumps(self.data), encoding='utf-8')
        return audit(self.root, digest(manifest), self.source)

    def test_valid_is_never_product_approval(self):
        result = self.run_audit()
        self.assertFalse(result['releaseEligible'])
        self.assertTrue(result['pendingReleaseGates'])

    def test_promoted_profile_rejected(self):
        self.data['releaseEligible'] = True
        with self.assertRaisesRegex(ValueError, 'profile'):
            self.run_audit()

    def test_duplicate_and_traversal_rejected(self):
        original = list(self.data['files'])
        for path in (original[0]['path'].upper(), '../outside.exe'):
            with self.subTest(path=path):
                self.data['files'] = original + [dict(path=path, sha256='0' * 64)]
                with self.assertRaisesRegex(ValueError, 'path'):
                    self.run_audit()

    def test_tamper_rejected(self):
        (self.root / 'hc_epub_render_poc.exe').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'integrity'):
            self.run_audit()

    def test_unlisted_file_rejected(self):
        (self.root / 'extra.dll').write_bytes(b'fixture')
        with self.assertRaisesRegex(ValueError, 'Unlisted'):
            self.run_audit()

    def test_source_drift_rejected(self):
        (self.source / 'reader.js').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'source drift'):
            self.run_audit()

    def test_product_candidate_is_not_release_approval(self):
        (self.root / 'hc_epub_render_poc.exe').rename(self.root / 'HyperEpubWeb.exe')
        for row in self.data['files']:
            if row['path'] == 'hc_epub_render_poc.exe':
                row['path'] = 'HyperEpubWeb.exe'
        self.data.update(schemaVersion=3, diagnosticOnly=False, releaseEligible=True,
                         profile='epub-passive-read-only', helper='HyperEpubWeb.exe',
                         readOnly=True, readerEngine='epubjs', readerVersion='0.3.93')
        manifest = self.root / 'epub-runtime-inventory.json'
        manifest.write_text(json.dumps(self.data), encoding='utf-8')
        result = audit(self.root, digest(manifest), self.source, product_candidate=True)
        self.assertTrue(result['productProfile'])
        self.assertFalse(result['releaseEligible'])
        with self.assertRaisesRegex(ValueError, 'pin mismatch'):
            audit(self.root, '0' * 64, self.source, product_candidate=True)
        (self.root / 'HyperEpubWeb.exe').write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'integrity'):
            audit(self.root, digest(manifest), self.source, product_candidate=True)

    def test_wrong_pin_rejected(self):
        self.run_audit()
        with self.assertRaisesRegex(ValueError, 'pin mismatch'):
            audit(self.root, '0' * 64, self.source)


if __name__ == '__main__':
    unittest.main()
