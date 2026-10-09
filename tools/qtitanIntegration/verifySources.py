"""Verify the integrated source contract and isolated CMake regeneration failures."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess

Root = Path(__file__).resolve().parents[2]
Vendor = Root / "externalLibs/waterColorStyle/third_party/QtitanDataGrid"

def main():
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--generated", type=Path, required=True)
    Parser.add_argument("--exercise-generator", action="store_true")
    Parser.add_argument("--work", type=Path, default=Root / "_build/p2-qtitan/generator-check")
    Parser.add_argument("--kit", type=Path, default=Path("D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-09-04"))
    Args = Parser.parse_args()
    Manifest = json.loads((Vendor / "integration.json").read_text(encoding="utf-8"))
    Contract = json.loads((Root / "tools/qtitan-compatibility.json").read_text(encoding="utf-8"))["integrated"]
    Failures = []
    for Name, Expected in Manifest["generatedSources"].items():
        File = Args.generated / Name
        if not File.is_file() or hashlib.sha256(File.read_bytes()).hexdigest() != Expected:
            Failures.append(Name)
    for Name, Symbols in Contract["requiredApis"].items():
        Text = (Args.generated / Name).read_text(encoding="utf-8")
        Failures.extend(Name + ": " + Symbol for Symbol in Symbols if Symbol not in Text)
    if Failures:
        raise RuntimeError("Source contract mismatch: " + ", ".join(Failures))
    Results = {"generatedSources": len(Manifest["generatedSources"]), "apiContract": "passed"}
    if Args.exercise_generator:
        Work = Args.work.resolve()
        if not Work.is_relative_to((Root / "_build").resolve()) or Work.exists():
            raise RuntimeError("Use a new directory under the repository _build directory.")
        Source = Work / "source"
        Copied = Source / "qtitan"
        Copied.mkdir(parents=True)
        for Name in ("CMakeLists.txt", "integration.json"):
            shutil.copy2(Vendor / Name, Copied / Name)
        shutil.copytree(Vendor / "patches", Copied / "patches")
        shutil.copytree(Vendor / "upstream/9.3.0", Copied / "upstream/9.3.0")
        (Source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(QtitanGeneratorCheck LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
            "find_package(Qt6 6.12 REQUIRED COMPONENTS Widgets PrintSupport Sql)\n"
            "qt_standard_project_setup()\nadd_subdirectory(qtitan)\n",
            encoding="utf-8", newline="\r\n")
        Build = Work / "build"
        Stamp = Build / "qtitan/patched.stamp"
        Generated = Build / "qtitan/src"
        Checks = []
        def configure(Name, Success=True):
            Command = [str(Args.kit / "bin/qt-cmake.bat"), "-S", str(Source), "-B", str(Build),
                       "-G", "Visual Studio 18 2026", "-A", "x64", "-T", "v145,host=x64",
                       "-DCMAKE_PREFIX_PATH=" + Args.kit.as_posix()]
            if Name in ("upstream-content", "upstream-add", "upstream-remove", "patch-content"):
                Command = [str(Args.kit / "bin/qt-cmake.bat"), "--build", str(Build),
                           "--config", "Release", "--target", "ZERO_CHECK"]
            Run = subprocess.run(Command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (Work / (Name + ".log")).write_bytes(Run.stdout)
            if (Run.returncode == 0) != Success:
                raise RuntimeError("Unexpected configure result: " + Name)
            Checks.append(Name)
        configure("fresh")
        for Name, Expected in Manifest["generatedSources"].items():
            assert hashlib.sha256((Generated / Name).read_bytes()).hexdigest() == Expected, Name
        Before = Stamp.stat().st_mtime_ns
        configure("unchanged")
        assert Stamp.stat().st_mtime_ns == Before
        Input = Copied / "upstream/9.3.0/src/grid/QtnGridDef.h"
        Original = Input.read_bytes()
        Input.write_bytes(Original + b"\n// generator content-change probe\n")
        configure("upstream-content")
        assert b"content-change probe" in (Generated / "src/grid/QtnGridDef.h").read_bytes()
        Input.write_bytes(Original)
        Added = Copied / "upstream/9.3.0/probe.txt"
        Added.write_bytes(b"new input")
        configure("upstream-add")
        assert (Generated / "probe.txt").is_file()
        Added.unlink()
        configure("upstream-remove")
        assert not (Generated / "probe.txt").exists()
        Patch = Copied / "patches" / Manifest["patches"][0]
        OriginalPatch = Patch.read_bytes()
        Before = Stamp.read_bytes()
        Patch.write_bytes(OriginalPatch + b"\n")
        configure("patch-content")
        assert Stamp.read_bytes() != Before
        Patch.unlink()
        configure("missing-patch", False)
        Patch.write_bytes(b"invalid patch\n")
        configure("invalid-patch", False)
        assert not Stamp.exists()
        Patch.write_bytes(OriginalPatch)
        Series = Copied / "patches/series"
        OriginalSeries = Series.read_bytes()
        Series.write_bytes(OriginalSeries.replace(Manifest["patches"][0].encode(), b""))
        configure("missing-series-entry", False)
        Series.write_bytes(OriginalSeries)
        configure("restored")
        for Name, Expected in Manifest["generatedSources"].items():
            assert hashlib.sha256((Generated / Name).read_bytes()).hexdigest() == Expected, Name
        Results["generatorChecks"] = Checks
        (Work / "results.json").write_text(json.dumps(Results, indent=2) + "\n",
                                            encoding="utf-8", newline="\r\n")
    print(json.dumps(Results, indent=2))

if __name__ == "__main__":
    main()
