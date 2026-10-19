# Adapt the pinned upstream CMake project to the workspace's MSVC runtime.
function(hc_configure_comparison_simd)
    if(TARGET Simd)
        set_property(TARGET Simd PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
        target_compile_options(Simd PRIVATE /utf-8 /MP6)
        target_link_options(Simd PRIVATE /NODEFAULTLIB:libucrt.lib /DEFAULTLIB:ucrt.lib)
    endif()
endfunction()
cmake_language(DEFER CALL hc_configure_comparison_simd)
