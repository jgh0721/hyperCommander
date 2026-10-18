# HyperCommander ISO Image for Total Commander

This package requires **64-bit Total Commander on Windows**. Both `.wcx` and
`.wcx64` files contain the same x64 binary; there is no 32-bit plugin in this ZIP.
Use the Release package for distribution. Debug builds require the development
runtime of the configured Visual Studio kit.

## Build targets

From the HyperCommander repository root, use the prescribed x64 Qt/Visual Studio
kit for both configuration and building:

```powershell
& 'D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-10-10/bin/qt-cmake.bat' --preset vc2026-qt612
& 'D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-10-10/bin/qt-cmake.bat' --build --preset vc2026-release --target hc_iso_tc_package
```

`hc_isoimage_tc_wcx` builds `Release/hc_isoimage_tc.wcx` in the build directory.
`hc_iso_tc_package` builds that target and its engine, then creates
`_build/vc2026-qt612/packages/Release/hc-isoimage-tc-x64.zip`.
Use `vc2026-debug` for the corresponding `Debug` artifacts. The TC targets are
explicit opt-in targets; the application keeps its built-in `.wcx64` package.

## Installation and use

Open the ZIP in Total Commander to start plugin installation. The root
`pluginst.inf` registers `hc_isoimage_tc.wcx64` for `iso`. Keep `hc_iso_core.dll`
beside the plugin. Manual installation is also available in Configuration >
Options > Plugins > Packer plugins. Existing ISO associations remain the user's
choice in Total Commander's installation dialog.

The plugin reads, extracts, creates and rewrites ISO 9660, Joliet and UDF 1.02
bridge images. The default creation profile includes ISO 9660, Joliet and UDF.
To change it, add the following section to the `pkplugin.ini` path supplied by
Total Commander (normally beside `wincmd.ini`):

```ini
[IsoImage]
Profile=3
VolumeLabel=MY_DISC
```

Profiles: `0` = ISO 9660; `1` = ISO + Joliet; `2` = ISO + Joliet + UDF 1.02;
`3` = ISO + UDF 1.02 for longer names. Label limits are 32 ASCII characters for
profile 0, 16 for profiles 1/2 and 30 for profile 3. A default INI is intentionally
not shipped, so installing updates does not overwrite user settings.

Images with boot or primary system-use metadata can be read but cannot be
rewritten. Creation produces data images, not bootable Windows installation
media. File timestamps use the reproducible 2000-01-01 default. UDF metadata,
virtual and sparable partitions and sparse/continuation allocations are not
supported. The engine limits images to 128 GiB and inventories to 100,000 entries.

No Qt, Python, HyperCommander host executable or commercial component is needed
at runtime. The C++ engine is LGPL-2.1-only; see COPYING, THIRD_PARTY_NOTICES.md,
source-lock.json and PORTING_MAP.md. Extract `hc-iso-source.zip` and follow
REBUILD.md to rebuild the engine. With Total Commander closed, a rebuilt
`hc_iso_core.dll` can replace the DLL alongside this plugin directly; no
HyperCommander manifest approval is involved in Total Commander.

Installation format reference:
https://www.ghisler.ch/wiki/index.php/Plugins_Automated_Installation
