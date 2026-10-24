"""Create a new P0 corpus and evidence directory, then verify expected outcomes."""
import argparse
import hashlib
import json
import platform
import subprocess
import sys
import zlib
from pathlib import Path
import make_corpus
import package_probe


def run(output):
    output.mkdir(parents=True, exist_ok=False)
    corpus = output/"corpus"
    manifest = make_corpus.generate(corpus)
    reports = output/"reports"
    reports.mkdir()
    results = []
    for case in manifest["cases"]:
        path = corpus/case["file"]
        report = package_probe.inspect(path)
        unchanged = hashlib.sha256(path.read_bytes()).hexdigest() == case["sha256"]
        passed = report["status"] == case["expectedStatus"] and unchanged
        if case["expectedCode"]:
            passed = passed and case["expectedCode"] in [issue["code"] for issue in report["issues"]]
        results.append(dict(id=case["id"], passed=passed, status=report["status"], originalPreserved=unchanged))
        (reports/(case["id"]+".json")).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    unit = subprocess.run([sys.executable, str(Path(__file__).with_name("test_probe.py"))], capture_output=True)
    (output/"unit-tests.log").write_bytes(unit.stdout+unit.stderr)
    evidence = dict(schemaVersion=1, phase="P0-A", python=platform.python_version(), zlib=zlib.ZLIB_RUNTIME_VERSION,
                    cases=results, unitTestsExitCode=unit.returncode,
                    rendererComparison="not-run", productIntegration="not-started", epubCheck="not-run",
                    sourceHashes={name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                                  for name in ["package_probe.py", "make_corpus.py", "test_probe.py", "run_validation.py"]})
    (output/"validation.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(dict(cases=len(results), passed=sum(r["passed"] for r in results), unitTestsExitCode=unit.returncode)))
    return 0 if unit.returncode == 0 and all(r["passed"] for r in results) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    raise SystemExit(run(parser.parse_args().output))
