qt_add_library(hc_archive_creation STATIC
    creationPlan.cpp creationPlan.hpp
    creationJob.cpp creationJob.hpp
    creatorRegistry.cpp creatorRegistry.hpp
    creationSettings.cpp creationSettings.hpp
    creationDialog.cpp creationDialog.hpp
    stdafx.h
)
target_include_directories(hc_archive_creation PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(hc_archive_creation PUBLIC hc_builtin_sevenzip hc_builtin_iso hc_plugin_process hc_viewer_core Qt6::Widgets)
target_link_libraries(hc_archive_creation PRIVATE Qt6::Concurrent Fm::widgets)
target_compile_options(hc_archive_creation PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_precompile_headers(hc_archive_creation PRIVATE stdafx.h)
