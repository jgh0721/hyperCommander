include("${CMAKE_CURRENT_SOURCE_DIR}/widgets/scintillaPadding.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/runtime/semanticParser.cmake")

qt_add_library(hc_compare_core STATIC core/compare.cpp core/compare.hpp core/mergeModel.cpp core/mergeModel.hpp
    core/alignmentMap.cpp core/alignmentMap.hpp core/inlineDiff.cpp core/inlineDiff.hpp
    core/overviewMap.cpp core/overviewMap.hpp)
target_include_directories(hc_compare_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_precompile_headers(hc_compare_core PRIVATE stdafx.h)

qt_add_library(hc_compare_runtime STATIC
    runtime/semantic.cpp runtime/semantic.hpp
    runtime/functionMatch.cpp
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
target_link_libraries(hc_compare_runtime PUBLIC hc_compare_core Qt6::Core PRIVATE advapi32 shell32 ole32 uuid netapi32 mpr shlwapi)
target_precompile_headers(hc_compare_runtime PRIVATE runtime/stdafx.h)

qt_add_library(hc_compare_widgets STATIC widgets/directoryMergeDialog.cpp widgets/directoryMergeDialog.hpp widgets/directoryView.cpp widgets/directoryView.hpp widgets/compareWindow.cpp widgets/compareWindow.hpp
    widgets/semanticView.cpp widgets/semanticView.hpp
    widgets/mergeController.cpp widgets/mergeController.hpp widgets/scrollSynchronizer.cpp widgets/scrollSynchronizer.hpp
    widgets/findBar.cpp widgets/findBar.hpp widgets/overviewBar.cpp widgets/overviewBar.hpp)
target_link_libraries(hc_compare_widgets PUBLIC hc_compare_runtime Qt6::Widgets PRIVATE hc_scintilla hc_lexilla fmstyle Qt6::Concurrent)
target_precompile_headers(hc_compare_widgets PRIVATE stdafx.h)

qt_add_executable(HyperCompare WIN32 main.cpp)
target_link_libraries(HyperCompare PRIVATE hc_compare_widgets fmstyle)
qt_add_executable(HyperCompareCli cliMain.cpp)
target_link_libraries(HyperCompareCli PRIVATE hc_compare_runtime hc_compare_semantic_worker)
foreach(target IN ITEMS hc_compare_core hc_compare_runtime hc_compare_widgets HyperCompare HyperCompareCli)
    target_compile_definitions(${target} PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
endforeach()
hc_enable_qt_deployment(HyperCompare PLUGINS)
hc_enable_qt_deployment(HyperCompareCli)

if(BUILD_TESTING)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
    qt_add_executable(hc_compare_tests tests/tstCompare.cpp)
    target_link_libraries(hc_compare_tests PRIVATE hc_compare_runtime Qt6::Test)
    target_compile_definitions(hc_compare_tests PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(hc_compare_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_dependencies(hc_compare_tests HyperCompareCli HyperCompare)
    add_test(NAME hypercompare.core COMMAND hc_compare_tests -o "${CMAKE_CURRENT_BINARY_DIR}/core-results.txt,txt")
    hc_enable_qt_deployment(hc_compare_tests TEST)
    qt_add_executable(hc_compare_ui_tests tests/tstCompareUi.cpp)
    target_link_libraries(hc_compare_ui_tests PRIVATE hc_compare_widgets hc_scintilla fmstyle Qt6::Test)
    target_compile_definitions(hc_compare_ui_tests PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(hc_compare_ui_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME hypercompare.ui COMMAND hc_compare_ui_tests -o "${CMAKE_CURRENT_BINARY_DIR}/ui-results.txt,txt")
    set_tests_properties(hypercompare.ui PROPERTIES TIMEOUT 60)
    hc_enable_qt_deployment(hc_compare_ui_tests TEST PLUGINS)
endif()
