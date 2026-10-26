"""Run the two-engine Qt renderer matrix with new, non-overwriting evidence directories."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from prepare_render import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--kit", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--native", action="store_true", help="Open original EPUB with the C++ package reader")
    parser.add_argument("--compatibility-manifest", type=Path, help="Owned compatibility corpus including edited Save As copies")
    parser.add_argument("--typography", action="store_true", help="Observe typography corpus capabilities; not a compatibility pass")
    parser.add_argument("--require-reading-overrides", action="store_true", help="Fail unless typography override and restoration capabilities pass")
    args = parser.parse_args()
    if args.require_reading_overrides and not args.typography:
        parser.error("--require-reading-overrides requires --typography")
    if args.typography and not args.native:
        parser.error("--typography requires --native")
    args.output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.update(PATH=str(args.kit/"bin")+os.pathsep+env["PATH"], QT_PLUGIN_PATH=str(args.kit/"plugins"),
               QT_QPA_PLATFORM="windows", QTWEBENGINEPROCESS_PATH=str(args.kit/"bin/QtWebEngineProcess.exe"),
               QTWEBENGINE_RESOURCES_PATH=str(args.kit/"resources"),
               QTWEBENGINE_LOCALES_PATH=str(args.kit/"translations/qtwebengine_locales"))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    cases = []
    filenames = ["epub3-images.epub"] if args.quick else ["epub3-images.epub", "epub2-ncx.epub"]
    if args.compatibility_manifest:
        manifest = json.loads(args.compatibility_manifest.read_text(encoding="utf-8"))
        filenames = [case["file"] for case in manifest["cases"]]
        if not args.native or args.typography or not filenames or len(filenames) > 32:
            parser.error("Compatibility matrix requires native mode, 1..32 cases and no typography mode")
        for case in manifest["cases"]:
            name = case["file"]
            if Path(name).name != name or not name.endswith(".epub") or hashlib.sha256((args.corpus/name).read_bytes()).hexdigest() != case["sha256"]:
                parser.error("Invalid compatibility corpus path or hash")
    if args.typography:
        filenames = [case["file"] for case in json.loads((args.corpus/"manifest.json").read_text(encoding="utf-8"))["cases"]]
        if any(Path(name).name != name or not name.endswith(".epub") for name in filenames):
            parser.error("Invalid typography corpus path")
    for filename in filenames:
        source = args.corpus/filename
        original = source.read_bytes()
        prepared = args.output/(source.stem+".json")
        if not args.native:
            prepared.write_text(json.dumps(prepare(source), ensure_ascii=False), encoding="utf-8")
        for engine in ["epubjs", "readium"]:
            for theme in (["light"] if args.quick else ["light", "dark"]):
                name = f"{source.stem}-{engine}-{theme}"
                evidence = args.output/name
                command = [str(args.executable.resolve()), "--epub" if args.native else "--publication", str((source if args.native else prepared).resolve()),
                           "--bundle", str(args.bundle.resolve()), "--engine", engine, "--theme", theme,
                           "--evidence", str(evidence.resolve())]
                started = time.monotonic()
                run = subprocess.run(command, env=env, capture_output=True, startupinfo=startup, timeout=75)
                (args.output/(name+".log")).write_bytes(run.stdout+run.stderr)
                result = json.loads((evidence/"result.json").read_text(encoding="utf-8")) if (evidence/"result.json").exists() else {}
                record = dict(case=name, exitCode=run.returncode, status=result.get("status", "no-result"),
                              elapsedSeconds=round(time.monotonic()-started, 3), checks=result.get("checks", {}),
                              sourcePositionChecks=result.get("sourcePositionChecks", {}),
                              sourcePreserved=source.read_bytes() == original)
                if args.typography:
                    record["capabilities"] = result.get("capabilities", {})
                    record["compatibilityAccepted"] = False
                    if args.require_reading_overrides:
                        required = ["typographyOverride", "independentSpacing", "publisherRestored",
                                    "publisherInlineRestored", "invalidProfilePreserved"]
                        if result.get("fixture") == "css-priority":
                            required.append("nestedInlineOverride")
                        record["readingOverridesAccepted"] = all(record["capabilities"].get(key) is True for key in required)
                cases.append(record)
                print(json.dumps(record, ensure_ascii=False), flush=True)
    summary = dict(schemaVersion=1, phase="compatibility-save-as" if args.compatibility_manifest else "P0-B-typography" if args.typography else "P0-B", inputMode="native-epub" if args.native else "python-snapshot", cases=cases,
                   executableSha256=hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                   bundle=json.loads((args.bundle/"bundle.json").read_text(encoding="utf-8")))
    (args.output/"validation.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0 if all(c["exitCode"] == 0 and c["status"] == "passed" and c["sourcePreserved"]
                    and c.get("readingOverridesAccepted", not args.require_reading_overrides) for c in cases) else 1


if __name__ == "__main__":
    raise SystemExit(main())
