"""Explicit signed-runtime positive and fail-closed manifest regression checks."""
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest

ROOT = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None
SCRIPT = Path(__file__).with_name('Test-ProductRuntime.ps1').resolve()

class ProductSignatures(unittest.TestCase):
    def run_check(self, root, pin):
        return subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
            '-File', str(SCRIPT), '-RuntimeRoot', str(root), '-ExpectedSha256', pin, '-AsJson'],
            capture_output=True, timeout=120)

    def test_signed_runtime(self):
        manifest = ROOT/'epub-runtime-inventory.json'
        run = self.run_check(ROOT, hashlib.sha256(manifest.read_bytes()).hexdigest())
        self.assertEqual(run.returncode, 0, run.stderr.decode(errors='replace'))
        self.assertEqual(len(json.loads(run.stdout)['files']), len(json.loads(manifest.read_bytes())['files'])+1)

    def test_wrong_independent_pin(self):
        self.assertNotEqual(self.run_check(ROOT, '0'*64).returncode, 0)

    def test_rejected_manifests(self):
        base = json.loads((ROOT/'epub-runtime-inventory.json').read_bytes())
        cases = [dict(diagnosticOnly=True), dict(releaseEligible=False), dict(readOnly=False),
                 dict(readerEngine='readium'), dict(helper='other.exe'),
                 dict(files=[dict(path='../escape', sha256='0'*64)]),
                 dict(files=[dict(path='/absolute', sha256='0'*64)]),
                 dict(files=[dict(path='con.dll', sha256='0'*64)])]
        for patch in cases:
            with self.subTest(patch=patch), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                content = json.dumps(base | patch).encode()
                (root/'epub-runtime-inventory.json').write_bytes(content)
                self.assertNotEqual(self.run_check(root, hashlib.sha256(content).hexdigest()).returncode, 0)

    def test_unsigned_pe(self):
        base = json.loads((ROOT/'epub-runtime-inventory.json').read_bytes())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = bytearray((ROOT/'HyperEpubWeb.exe').read_bytes())
            pe = struct.unpack_from('<I', payload, 0x3c)[0]
            optional = pe+24
            directories = optional+(112 if struct.unpack_from('<H', payload, optional)[0] == 0x20b else 96)
            certificate, size = struct.unpack_from('<II', payload, directories+32)
            self.assertGreater(size, 0)
            struct.pack_into('<II', payload, directories+32, 0, 0)
            payload = payload[:certificate]
            (root/'untrusted.exe').write_bytes(payload)
            base['files'] = [dict(path='untrusted.exe', sha256=hashlib.sha256(payload).hexdigest())]
            content = json.dumps(base).encode()
            (root/'epub-runtime-inventory.json').write_bytes(content)
            run = self.run_check(root, hashlib.sha256(content).hexdigest())
            self.assertNotEqual(run.returncode, 0)
            self.assertIn(b'signature', run.stderr.lower())

if __name__ == '__main__':
    if ROOT is None:
        raise SystemExit('Pass an independently staged signed product runtime directory')
    unittest.main()
