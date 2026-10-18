# Also included by package scripts. Signing happens before their hashes become native pins.
function(hc_sign_packaged_file Path)
    if(NOT HC_SIGNING_SCRIPT OR NOT CONFIG STREQUAL "Release")
        return()
    endif()
    execute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${HC_SIGNING_SCRIPT}" -Path "${Path}" -CacheRoot "${HC_SIGNING_CACHE}"
        RESULT_VARIABLE RESULT)
    if(NOT RESULT EQUAL 0)
        message(FATAL_ERROR "Product code signing failed: ${Path}")
    endif()
endfunction()
