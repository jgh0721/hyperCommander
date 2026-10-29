# Opt-in diagnostic tools and authenticated product helper candidate.
set(HC_EPUB_PYTHON_EXECUTABLE "" CACHE FILEPATH "Python 3.10+ for EPUB P0 package probes")
if(HC_BUILD_EPUB_POC AND NOT EXISTS "${HC_EPUB_PYTHON_EXECUTABLE}")
    message(FATAL_ERROR "HC_BUILD_EPUB_POC requires HC_EPUB_PYTHON_EXECUTABLE")
endif()
file(TO_CMAKE_PATH "${HC_EPUB_PYTHON_EXECUTABLE}" HC_EPUB_PYTHON_PATH)
if(HC_BUILD_EPUB_POC)
    qt_add_executable(hc_epub_native_probe poc/nativeProbe.cpp poc/stdafx.h)
    target_precompile_headers(hc_epub_native_probe PRIVATE poc/stdafx.h)
    target_link_libraries(hc_epub_native_probe PRIVATE hc_epub_package Qt6::Widgets)
    target_compile_options(hc_epub_native_probe PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(hc_epub_native_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/epub-poc")
    qt_add_executable(hc_epub_poc WIN32
        poc/main.cpp
        poc/stdafx.h
        "${PROJECT_SOURCE_DIR}/UniqueLibs/document/documentThemeAdapter.cpp"
    )
    target_precompile_headers(hc_epub_poc PRIVATE poc/stdafx.h)
    target_include_directories(hc_epub_poc PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(hc_epub_poc PRIVATE Qt6::Widgets Fm::style)
    target_compile_definitions(hc_epub_poc PRIVATE
        "HC_EPUB_PYTHON=\"${HC_EPUB_PYTHON_PATH}\""
        "HC_EPUB_PROBE=\"${CMAKE_CURRENT_SOURCE_DIR}/package_probe.py\""
    )
    target_compile_options(hc_epub_poc PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(hc_epub_poc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/epub-poc"
    )
endif()
find_package(Qt6 6.12.0 REQUIRED COMPONENTS WebEngineWidgets)
set(HC_EPUB_RENDER_TARGETS)
if(HC_BUILD_EPUB_POC)
    list(APPEND HC_EPUB_RENDER_TARGETS hc_epub_render_poc)
endif()
if(HC_BUILD_EPUB_PRODUCT)
    list(APPEND HC_EPUB_RENDER_TARGETS HyperEpubWeb)
endif()
foreach(HC_EPUB_RENDER_TARGET IN LISTS HC_EPUB_RENDER_TARGETS)
    qt_add_executable(${HC_EPUB_RENDER_TARGET} WIN32
        render/main.cpp
        render/stdafx.h
        render/readingSettings.hpp
        render/readingSettings.cpp
        render/readingStore.cpp
        render/readingNavigation.cpp
        render/readingNavigation.hpp
        render/readingAssets.cpp
        render/readingAssets.hpp
        render/readingProfiles.cpp render/readingProfiles.hpp
        render/readingProfilePanel.cpp
        render/readingAnnotations.cpp render/readingAnnotations.hpp
        render/readingAnnotationPanel.cpp
        render/ownerMode.hpp
        render/ownerMode.cpp
        render/inputTransfer.hpp
        render/inputTransfer.cpp
        render/publicationResources.hpp
        "${PROJECT_SOURCE_DIR}/UniqueLibs/document/documentWebPeer.cpp"
        "${PROJECT_SOURCE_DIR}/UniqueLibs/document/documentWebPeer.hpp"
        "${PROJECT_SOURCE_DIR}/UniqueLibs/document/documentThemeAdapter.cpp"
    )
    target_precompile_headers(${HC_EPUB_RENDER_TARGET} PRIVATE render/stdafx.h)
    target_include_directories(${HC_EPUB_RENDER_TARGET} PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(${HC_EPUB_RENDER_TARGET} PRIVATE hc_epub_package Qt6::Widgets Qt6::WebEngineWidgets Qt6::Concurrent Qt6::Network Fm::style Fm::widgets shell32)
    target_compile_options(${HC_EPUB_RENDER_TARGET} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(${HC_EPUB_RENDER_TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/epub-poc"
    )
    if(HC_EPUB_RENDER_TARGET STREQUAL "HyperEpubWeb")
        target_compile_definitions(HyperEpubWeb PRIVATE HC_EPUB_PRODUCT_HELPER=1)
        set_target_properties(HyperEpubWeb PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/epub-product")
    endif()
endforeach()
if(BUILD_TESTING AND HC_BUILD_EPUB_POC)
    find_package(Qt6 6.12.0 REQUIRED COMPONENTS Test)
    qt_add_executable(hc_epub_input_tests render/testInputTransfer.cpp render/inputTransfer.cpp render/inputTransfer.hpp render/stdafx.h)
    target_precompile_headers(hc_epub_input_tests PRIVATE render/stdafx.h)
    target_link_libraries(hc_epub_input_tests PRIVATE hc_epub_package Qt6::Widgets Qt6::Test)
    target_compile_options(hc_epub_input_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(hc_epub_input_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/epub-poc")
    add_test(NAME hc.epub.p0.input COMMAND hc_epub_input_tests -platform offscreen)
    set_tests_properties(hc.epub.p0.input PROPERTIES TIMEOUT 60)
    qt_add_executable(hc_epub_owner_tests render/testOwner.cpp render/stdafx.h)
    target_precompile_headers(hc_epub_owner_tests PRIVATE render/stdafx.h)
    target_include_directories(hc_epub_owner_tests PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(hc_epub_owner_tests PRIVATE hc_epub_host Qt6::Widgets Qt6::Concurrent Qt6::Test)
    target_compile_options(hc_epub_owner_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(hc_epub_owner_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/epub-poc")
    qt_add_executable(hc_epub_workspace_tests render/testWorkspace.cpp render/stdafx.h)
    target_precompile_headers(hc_epub_workspace_tests PRIVATE render/stdafx.h)
    target_link_libraries(hc_epub_workspace_tests PRIVATE hc_document_widgets hc_epub_host Qt6::Test Fm::style)
    target_compile_options(hc_epub_workspace_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(hc_epub_workspace_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/epub-poc")
    # Run explicitly with independently pinned development runtime and evidence paths.
    qt_add_executable(hc_epub_settings_tests
        render/testReadingSettings.cpp
        render/readingSettings.hpp
        render/readingSettings.cpp
        render/readingStore.cpp
        render/readingNavigation.cpp
        render/readingNavigation.hpp
        render/readingAssets.cpp
        render/readingAssets.hpp
        render/readingProfiles.cpp render/readingProfiles.hpp
        render/readingProfilePanel.cpp
        render/readingAnnotations.cpp render/readingAnnotations.hpp
        render/readingAnnotationPanel.cpp
        render/stdafx.h
    )
    target_precompile_headers(hc_epub_settings_tests PRIVATE render/stdafx.h)
    target_link_libraries(hc_epub_settings_tests PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Test Fm::widgets)
    target_compile_options(hc_epub_settings_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(hc_epub_settings_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/epub-poc")
    add_test(NAME hc.epub.p0.settings COMMAND hc_epub_settings_tests -platform offscreen)
    set_tests_properties(hc.epub.p0.settings PROPERTIES TIMEOUT 60)
    add_test(NAME hc.epub.p0.probe
        COMMAND "${HC_EPUB_PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_probe.py"
    )
    set_tests_properties(hc.epub.p0.probe PROPERTIES TIMEOUT 90)
    add_test(NAME hc.epub.p0.render.prepare
        COMMAND "${HC_EPUB_PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_prepare_render.py"
    )
    set_tests_properties(hc.epub.p0.render.prepare PROPERTIES TIMEOUT 90)
    add_test(NAME hc.epub.p0.package.native
        COMMAND "${HC_EPUB_PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_native_package.py" "$<TARGET_FILE:hc_epub_native_probe>"
    )
    set_tests_properties(hc.epub.p0.package.native PROPERTIES TIMEOUT 120)
endif()
