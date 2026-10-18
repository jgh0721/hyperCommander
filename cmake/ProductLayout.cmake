# Reads commonLibs/layout/productLayout.hpp so CMake scripts and C++ share one layout.
# Each `inline constexpr auto Name = "value";` becomes HC_LAYOUT_<Name>.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../commonLibs/layout/productLayout.hpp" HC_LAYOUT_LINES
    REGEX "inline constexpr auto [A-Za-z]+ *= *\"")
foreach(HC_LAYOUT_LINE IN LISTS HC_LAYOUT_LINES)
    if(HC_LAYOUT_LINE MATCHES "auto ([A-Za-z]+) *= *\"([^\"]*)\"")
        set(HC_LAYOUT_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()
foreach(HC_LAYOUT_REQUIRED IN ITEMS PluginHost IsoImagePackage SevenZipPackage AlzEggPackage HyperViewerPackage RhwpRuntime RhwpBundle)
    if(NOT DEFINED HC_LAYOUT_${HC_LAYOUT_REQUIRED})
        message(FATAL_ERROR "productLayout.hpp does not define ${HC_LAYOUT_REQUIRED}")
    endif()
endforeach()
