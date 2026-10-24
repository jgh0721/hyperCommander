"""Product snapshot copy must preserve unrelated files and fail closed on drift."""
import json
from pathlib import Path
import unittest
from test_qualify_runtime import QualificationTests as Fixture
from qualify_runtime import digest
from deploy_product_runtime import deploy


class DeploymentTests(unittest.TestCase):
    def setUp(self):
        Fixture.setUp(self)
        (self.root / 'hc_epub_render_poc.exe').rename(self.root / 'HyperEpubWeb.exe')
        for row in self.data['files']:
            if row['path'] == 'hc_epub_render_poc.exe':
                row['path'] = 'HyperEpubWeb.exe'
        self.data.update(schemaVersion=3, diagnosticOnly=False, releaseEligible=True,
                         profile='epub-passive-read-only', helper='HyperEpubWeb.exe',
                         readOnly=True, readerEngine='epubjs', readerVersion='0.3.93')
        manifest = self.root / 'epub-runtime-inventory.json'
        manifest.write_text(json.dumps(self.data), encoding='utf-8')
        self.pin = digest(manifest)
        self.application = Path(self.temp.name) / 'application'
        self.destination = self.application / 'Libs/Epub'

    def test_copy_and_repeat_preserve_siblings(self):
        self.application.mkdir()
        settings = self.application / 'settings.json'
        settings.write_bytes(b'user settings')
        for _ in range(2):
            result = deploy(self.root, self.pin, self.source, self.application)
            self.assertFalse(result['releaseEligible'])
            self.assertEqual((self.destination / 'HyperEpubWeb.exe').read_bytes(), b'fixture')
            self.assertEqual(settings.read_bytes(), b'user settings')

    def test_wrong_pin_creates_nothing(self):
        with self.assertRaisesRegex(ValueError, 'pin mismatch'):
            deploy(self.root, '0' * 64, self.source, self.application)
        self.assertFalse(self.application.exists())

    def test_existing_tamper_is_not_overwritten(self):
        deploy(self.root, self.pin, self.source, self.application)
        helper = self.destination / 'HyperEpubWeb.exe'
        helper.write_bytes(b'local changed file')
        with self.assertRaisesRegex(ValueError, 'integrity'):
            deploy(self.root, self.pin, self.source, self.application)
        self.assertEqual(helper.read_bytes(), b'local changed file')

    def test_source_drift_creates_nothing(self):
        (self.source / 'reader.js').write_bytes(b'changed source')
        with self.assertRaisesRegex(ValueError, 'source drift'):
            deploy(self.root, self.pin, self.source, self.application)
        self.assertFalse(self.application.exists())

    def test_nested_destination_rejected(self):
        with self.assertRaisesRegex(ValueError, 'separate'):
            deploy(self.root, self.pin, self.source, self.root / 'nested')
        self.assertFalse((self.root / 'nested').exists())


if __name__ == '__main__':
    unittest.main(defaultTest='DeploymentTests')
