include("${CMAKE_CURRENT_SOURCE_DIR}/widgets/scintillaPadding.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/runtime/semanticParser.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/runtime/archiveCodec.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/runtime/textRegex.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/runtime/archiveNative.cmake")
target_sources(hc_compare_semantic_worker PRIVATE runtime/docxWorker.cpp)
target_sources(hc_compare_semantic_worker PRIVATE runtime/nativeArchive.cpp runtime/nativeArchive.hpp)

qt_add_library(hc_compare_core STATIC core/compare.cpp core/compare.hpp core/mergeModel.cpp core/mergeModel.hpp
    core/linearDiff.cpp core/linearDiff.hpp
    core/alignmentMap.cpp core/alignmentMap.hpp core/inlineDiff.cpp core/inlineDiff.hpp
    core/overviewMap.cpp core/overviewMap.hpp)
target_include_directories(hc_compare_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hc_compare_core PRIVATE Qt6::Core)
target_precompile_headers(hc_compare_core PRIVATE stdafx.h)
target_link_libraries(hc_compare_semantic_worker PRIVATE hc_compare_core)

qt_add_library(hc_compare_runtime STATIC
    runtime/largeText.cpp runtime/largeText.hpp
    runtime/largeTextSession.cpp
    runtime/largeTextTools.cpp
    runtime/largeTextRegex.cpp
    runtime/archiveSession.cpp
    runtime/controllerMemory.cpp runtime/controllerMemory.hpp
    runtime/guiIpc.cpp runtime/guiIpc.hpp
    runtime/gitTool.cpp runtime/gitTool.hpp
    runtime/saveRecovery.cpp runtime/saveRecovery.hpp
    runtime/inputEditing.cpp runtime/inputEditing.hpp
    runtime/semantic.cpp runtime/semantic.hpp
    runtime/functionMatch.cpp
    runtime/engineRegistry.cpp runtime/engineRegistry.hpp
    runtime/workerHost.cpp runtime/workerHost.hpp
    runtime/extensionEngine.cpp runtime/extensionEngine.hpp
    runtime/requestFile.cpp runtime/requestFile.hpp
    runtime/batch.cpp runtime/batch.hpp
    runtime/directoryText.cpp runtime/directoryText.hpp
    runtime/directoryApply.cpp runtime/directoryApply.hpp
    runtime/directoryPlan.cpp runtime/directoryPlan.hpp
    runtime/directory.cpp runtime/directory.hpp
    cli/cliParser.cpp runtime/session.cpp runtime/session.hpp runtime/recompare.cpp
    runtime/decisionPlan.cpp runtime/decisionPlan.hpp
    runtime/mergeWork.cpp runtime/mergeWork.hpp
    "${PROJECT_SOURCE_DIR}/UniqueLibs/storageProviders.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/nativeWritableStorage.cpp"
    "${PROJECT_SOURCE_DIR}/UniqueLibs/document/documentTextCodec.cpp"
    "${PROJECT_SOURCE_DIR}/commonLibs/document/documentSaveCoordinator.cpp"
)
target_include_directories(hc_compare_runtime BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
target_include_directories(hc_compare_runtime PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(hc_compare_runtime PUBLIC hc_compare_core Qt6::Core Qt6::Network PRIVATE advapi32 shell32 ole32 uuid netapi32 mpr shlwapi)
target_link_libraries(hc_compare_runtime PRIVATE pcre2-8-static)
target_precompile_headers(hc_compare_runtime PRIVATE runtime/stdafx.h)

qt_add_library(hc_compare_widgets STATIC widgets/directoryMergeDialog.cpp widgets/directoryMergeDialog.hpp widgets/directoryView.cpp widgets/directoryView.hpp widgets/compareWindow.cpp widgets/compareWindow.hpp
    widgets/compareSettings.cpp widgets/compareSettings.hpp
    widgets/compareCommands.cpp widgets/compareCommands.hpp
    widgets/comparePalette.cpp widgets/comparePalette.hpp
    widgets/compareSettingsDialog.cpp widgets/compareSettingsDialog.hpp
    widgets/largeTextView.cpp widgets/largeTextView.hpp
    widgets/largeTextMultiView.cpp widgets/largeTextMultiView.hpp
    widgets/largeTextSyntax.cpp widgets/largeTextSyntax.hpp
    widgets/saveRecoveryDialog.cpp widgets/saveRecoveryDialog.hpp
    widgets/directoryTreeModel.cpp widgets/directoryTreeModel.hpp
    widgets/guiRequestServer.cpp widgets/guiRequestServer.hpp
    widgets/chunkGutter.cpp widgets/chunkGutter.hpp
    widgets/multiPaneAlignment.cpp widgets/multiPaneAlignment.hpp
    widgets/compareInputEditing.cpp
    widgets/semanticView.cpp widgets/semanticView.hpp
    widgets/imageCompareView.cpp widgets/imageCompareView.hpp
    widgets/archiveCompareView.cpp widgets/archiveCompareView.hpp widgets/extensionView.cpp widgets/extensionView.hpp
    widgets/docxCompareView.cpp widgets/docxCompareView.hpp
    widgets/mergeController.cpp widgets/mergeController.hpp widgets/scrollSynchronizer.cpp widgets/scrollSynchronizer.hpp
    widgets/findBar.cpp widgets/findBar.hpp widgets/overviewBar.cpp widgets/overviewBar.hpp)
target_link_libraries(hc_compare_widgets PUBLIC hc_compare_runtime Qt6::Widgets PRIVATE hc_scintilla hc_lexilla fmstyle Fm::widgets Qt6::Concurrent)
target_precompile_headers(hc_compare_widgets PRIVATE stdafx.h)

qt_add_executable(HyperCompare WIN32 main.cpp)
include("${PROJECT_SOURCE_DIR}/cmake/BundledFonts.cmake")
hc_add_bundled_fonts(HyperCompare)
target_link_libraries(HyperCompare PRIVATE hc_compare_widgets fmstyle Fm::widgets)
qt_add_executable(HyperCompareCli cliMain.cpp)
target_link_libraries(HyperCompareCli PRIVATE hc_compare_runtime hc_compare_semantic_worker)
foreach(target IN ITEMS hc_compare_core hc_compare_runtime hc_compare_widgets HyperCompare HyperCompareCli)
    target_compile_definitions(${target} PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
endforeach()
hc_enable_qt_deployment(HyperCompare PLUGINS)
hc_enable_qt_deployment(HyperCompareCli PLUGINS)

if(BUILD_TESTING)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
    qt_add_executable(hc_compare_tests tests/tstCompare.cpp)
    target_link_libraries(hc_compare_tests PRIVATE hc_compare_runtime hc_compare_zip Qt6::Test Qt6::Gui)
    target_compile_definitions(hc_compare_tests PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(hc_compare_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_dependencies(hc_compare_tests HyperCompareCli HyperCompare)
    add_test(NAME hypercompare.core COMMAND hc_compare_tests -o "${CMAKE_CURRENT_BINARY_DIR}/core-results.txt,txt")
    hc_enable_qt_deployment(hc_compare_tests TEST PLUGINS)
    qt_add_executable(hc_compare_ui_tests tests/tstCompareUi.cpp)
    target_link_libraries(hc_compare_ui_tests PRIVATE hc_compare_widgets hc_scintilla hc_lexilla fmstyle Qt6::Test)
    target_compile_definitions(hc_compare_ui_tests PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(hc_compare_ui_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME hypercompare.ui COMMAND hc_compare_ui_tests -o "${CMAKE_CURRENT_BINARY_DIR}/ui-results.txt,txt")
    set_tests_properties(hypercompare.ui PROPERTIES TIMEOUT 300)
    hc_enable_qt_deployment(hc_compare_ui_tests TEST PLUGINS)
endif()
