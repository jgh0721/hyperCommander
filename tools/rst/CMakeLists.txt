# reStructuredText preview helper: shows the HTML Sphinx built, in a process of its own (as the Markdown preview).
# It is placed beside HyperCommander.exe; the host starts it from QCoreApplication::applicationDirPath().
find_package(Qt6 6.12.0 REQUIRED COMPONENTS WebEngineWidgets WebChannel Network)
qt_add_executable(hc-rst-preview WIN32 render/main.cpp render/previewBridge.cpp render/previewBridge.hpp render/stdafx.h)
target_link_libraries(hc-rst-preview PRIVATE hc_startup_fonts)
set_target_properties(hc-rst-preview PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
target_precompile_headers(hc-rst-preview PRIVATE render/stdafx.h)
# documentWebProcess.hpp: the resize message shared with the host.
target_include_directories(hc-rst-preview PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(hc-rst-preview PRIVATE Qt6::WebEngineWidgets Qt6::WebChannel Qt6::Network)
target_compile_options(hc-rst-preview PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
# Scroll synchronisation and preview fonts are injected into every page (ported from MultiRoot reST Editor).
qt_add_resources(hc-rst-preview "rst_preview_assets" PREFIX "/rst" BASE "${CMAKE_CURRENT_SOURCE_DIR}/web" FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/web/mrr_preview.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/mrr_fonts.js")
hc_add_version_resource(hc-rst-preview "reStructuredText preview")
hc_enable_qt_deployment(hc-rst-preview PLUGINS)
add_custom_command(TARGET hc-rst-preview POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/MultiRootEsbonIo"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE-MultiRootEsbonIo.txt"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/MultiRootEsbonIo/LICENSE.txt"
    VERBATIM)
