# Rebuilding the engine

Extract `hc-iso-source.zip`. A C++23 compiler and CMake 3.25 or newer are required.
The engine does not require Qt, Python, Win32 headers or commercial components.

```text
cmake -S packaging -B build -A x64
cmake --build build --config Release
```

On Linux/macOS, omit `-A x64`.

For Total Commander, close TC and replace `hc_iso_core.dll` alongside
`hc_isoimage_tc.wcx64` in its plugin directory. No manifest approval is required.
See [README-TC.md](README-TC.md) in the source archive for the TC build targets.

For HyperCommander, copy the rebuilt engine DLL next to a copy of
`hc_isoimage_wcx.wcx64` in a separate plugin package. Regenerate the engine and
adapter SHA-256 hashes in that package's manifest, then register and review it
through HyperCommander's external plugin settings. This preserves the right to
replace the LGPL engine without granting modified binaries the built-in package's
compiled identity. The adapter uses only the public C ABI header.

`source-lock.json` identifies the upstream source and `PORTING_MAP.md` identifies
the translated modules. `COPYING` contains the applicable LGPL 2.1 license.
All source needed to modify and rebuild the engine is in this archive.
