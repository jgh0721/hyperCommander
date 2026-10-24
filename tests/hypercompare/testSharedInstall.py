"""Exercise signed component installation against isolated host directories.

Usage: python tests/hypercompare/testSharedInstall.py SIGNED_PACKAGE
"""
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(sys.argv.pop(1)).resolve()
MODULE = Path(__file__).resolve().parents[2] / "tools/hypercompare/HyperCompareDeployment.psm1"
MANIFEST = json.loads((SOURCE / "hypercompare-inventory.json").read_text(encoding="utf-8-sig"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quote(path):
    return "'" + str(path).replace("'", "''") + "'"


class SharedInstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hc-shared-install-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "HyperCommander"
        self.root.mkdir()

    def install(self, success=True):
        result = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "$ErrorActionPreference='Stop'; Import-Module " + quote(MODULE) +
             "; Install-HyperCompare -SourceDirectory " + quote(SOURCE) +
             " -Destination " + quote(self.root)], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=90)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def host(self, entries):
        (self.root / "product-inventory.json").write_text(json.dumps(
            {"product": "HyperCommander", "files": entries}), encoding="utf-8")

    def test_flat_migration_and_idempotence(self):
        legacy = self.root / "HyperCompare"
        shutil.copytree(SOURCE, legacy)
        (legacy / "user.ini").write_bytes(b"keep")
        shared = []
        for entry in MANIFEST["files"]:
            if entry["path"].endswith(".dll"):
                target = self.root / entry["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(SOURCE / entry["path"], target)
                shared.append(entry)
        self.host(shared)
        before = {str(p): (digest(p), p.stat().st_mtime_ns) for p in self.root.rglob("*") if p.is_file() and legacy not in p.parents}
        self.install()
        for entry in MANIFEST["files"]:
            self.assertEqual(digest(self.root / entry["path"]), entry["sha256"].lower())
            self.assertFalse((legacy / entry["path"]).exists())
        self.assertEqual((legacy / "user.ini").read_bytes(), b"keep")
        self.assertFalse((legacy / "hypercompare-inventory.json").exists())
        self.install()
        for p, value in before.items():
            self.assertEqual((digest(Path(p)), Path(p).stat().st_mtime_ns), value)

    def test_empty_legacy_directory_removed(self):
        shutil.copytree(SOURCE, self.root / "HyperCompare")
        self.install()
        self.assertFalse((self.root / "HyperCompare").exists())

    def test_modified_legacy_file_preserved(self):
        legacy = self.root / "HyperCompare"
        shutil.copytree(SOURCE, legacy)
        (legacy / "Licenses/Scintilla.txt").write_bytes(b"user edit")
        self.install()
        self.assertEqual((legacy / "Licenses/Scintilla.txt").read_bytes(), b"user edit")
        self.assertTrue((legacy / "hypercompare-inventory.json").exists())

    def test_host_runtime_mismatch_rejected_before_writes(self):
        entry = next(e for e in MANIFEST["files"] if e["path"] == "Qt6Core.dll")
        shutil.copy2(SOURCE / entry["path"], self.root / entry["path"])
        self.host([{**entry, "sha256": "0" * 64}])
        self.install(False)
        self.assertFalse((self.root / "HyperCompare.exe").exists())
        self.assertEqual(digest(self.root / entry["path"]), entry["sha256"].lower())

    def test_modified_shared_runtime_rejected(self):
        entry = next(e for e in MANIFEST["files"] if e["path"] == "Qt6Core.dll")
        self.host([entry])
        (self.root / entry["path"]).write_bytes(b"user edit")
        self.install(False)
        self.assertFalse((self.root / "HyperCompare.exe").exists())
        self.assertEqual((self.root / entry["path"]).read_bytes(), b"user edit")

    def test_unowned_file_collision_rejected(self):
        (self.root / "HyperCompare.exe").write_bytes(b"user file")
        self.install(False)
        self.assertEqual((self.root / "HyperCompare.exe").read_bytes(), b"user file")
        self.assertFalse((self.root / "HyperCompareCli.exe").exists())

    def test_legacy_path_traversal_rejected_before_writes(self):
        legacy = self.root / "HyperCompare"
        legacy.mkdir()
        bad = json.loads(json.dumps(MANIFEST))
        bad["files"][0]["path"] = "../user.ini"
        (legacy / "hypercompare-inventory.json").write_text(json.dumps(bad), encoding="utf-8")
        self.install(False)
        self.assertFalse((self.root / "HyperCompare.exe").exists())


if __name__ == "__main__":
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout))
