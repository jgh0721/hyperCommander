Qtitan integration regression harness
=====================================

This standalone CMake project builds the shared ``tests/qtitanRegression.hpp``
checks and the record painter integration test against waterColorStyle and its
licensed Qtitan sources. Keep build output under the repository ``_build`` tree.
The licensed sources must be available locally; they are not part of this harness.

Run these PowerShell commands from the repository root with the prescribed Qt kit::

    $Kit = 'D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-10-10'
    & "$Kit/bin/qt-cmake.bat" -S tests/qtitanIntegration -B _build/qtitan-integration -G 'Visual Studio 18 2026' -A x64 -T 'v145,host=x64' "-DCMAKE_PREFIX_PATH=$Kit" -U QTITAN_UPSTREAM_VERSION
    & "$Kit/bin/qt-cmake.bat" --build _build/qtitan-integration --config Release
    ctest --test-dir _build/qtitan-integration -C Release --output-on-failure

CTest supplies the Qt DLL search path and uses the offscreen platform. The
``hc_qtitan_integration`` Qt Test log is written to
``_build/qtitan-integration/hc-qtitan-Release.txt``.

Verify generated source hashes and the required API contract::

    python tests/qtitanIntegration/verifySources.py --generated _build/qtitan-integration/watercolor/third_party/QtitanDataGrid/src

The verifier also checks the complete official upstream file set against
``integration.json``. The selected version comes from that manifest; the current
integration is DataGrid 10.0.0 / Base 4.0.0. ``compactStyledHeaders`` checks the
10.0 complex header control with a 22-pixel header, caption indentation and both
application sort-arrow directions.
``externalFocusDuringCursorMove`` checks that file-manager cursor and metadata
updates preserve a search field's keyboard focus, followed by keyboard navigation
after returning to the grid.

To also exercise regeneration after upstream and patch changes, add
``--exercise-generator --work _build/qtitan-generator-check``. The work directory
must be new; the script changes only its isolated copies. Use ``--kit`` to select
a different compatible Qt installation.

Generator checks reject changed, added or missing official files, missing or
invalid patches, and incomplete series. They also verify unchanged inputs do not
regenerate, patch content changes do regenerate, and restored inputs produce the
same source hashes. Official vendor bytes stay unchanged; only generated and
``work/`` copies use the normalized patch representation.
