set(HC_EPUB_APPROVED_INVENTORY_SHA256 "" CACHE STRING "Native EPUB inventory pin; empty denies product EPUB open")
set(HC_EPUB_PRODUCT_RUNTIME_DIRECTORY "" CACHE PATH "Independently pinned EPUB runtime candidate")
if(NOT HC_EPUB_APPROVED_INVENTORY_SHA256 STREQUAL "")
    string(LENGTH "${HC_EPUB_APPROVED_INVENTORY_SHA256}" HC_EPUB_PIN_LENGTH)
    if(NOT HC_EPUB_PIN_LENGTH EQUAL 64 OR NOT HC_EPUB_APPROVED_INVENTORY_SHA256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "EPUB inventory policy requires 64 lowercase hex digits")
    endif()
endif()
if(HC_EPUB_PRODUCT_RUNTIME_DIRECTORY)
    if(NOT HC_EPUB_APPROVED_INVENTORY_SHA256)
        message(FATAL_ERROR "EPUB runtime requires an independent native inventory pin")
    endif()
    set(HC_EPUB_PYTHON_EXECUTABLE "" CACHE FILEPATH "Python 3.10+ for EPUB runtime qualification")
    if(NOT EXISTS "${HC_EPUB_PYTHON_EXECUTABLE}")
        message(FATAL_ERROR "EPUB runtime staging requires HC_EPUB_PYTHON_EXECUTABLE")
    endif()
    set(HC_EPUB_POLICY_COMMAND "${HC_EPUB_PYTHON_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/epub/deploy_product_runtime.py"
        --runtime "${HC_EPUB_PRODUCT_RUNTIME_DIRECTORY}"
        --manifest-sha256 "${HC_EPUB_APPROVED_INVENTORY_SHA256}"
        --source "${PROJECT_SOURCE_DIR}/tools/epub/web")
    execute_process(COMMAND ${HC_EPUB_POLICY_COMMAND} --verify-only
        RESULT_VARIABLE HC_EPUB_POLICY_RESULT OUTPUT_QUIET ERROR_VARIABLE HC_EPUB_POLICY_ERROR)
    if(NOT HC_EPUB_POLICY_RESULT EQUAL 0)
        message(FATAL_ERROR "EPUB runtime qualification failed: ${HC_EPUB_POLICY_ERROR}")
    endif()
    execute_process(COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tools/epub/Test-ProductRuntime.ps1"
        -RuntimeRoot "${HC_EPUB_PRODUCT_RUNTIME_DIRECTORY}" -ExpectedSha256 "${HC_EPUB_APPROVED_INVENTORY_SHA256}"
        RESULT_VARIABLE HC_EPUB_SIGNED_RESULT OUTPUT_QUIET ERROR_VARIABLE HC_EPUB_SIGNED_ERROR)
    if(NOT HC_EPUB_SIGNED_RESULT EQUAL 0)
        message(FATAL_ERROR "EPUB requires a signed product runtime: ${HC_EPUB_SIGNED_ERROR}")
    endif()
    # A separate application layout is reviewable without mutating shared Release or installation.
    add_custom_target(hc_epub_product_runtime
        COMMAND ${HC_EPUB_POLICY_COMMAND}
            --application "${CMAKE_BINARY_DIR}/epub-product-policy/${HC_EPUB_APPROVED_INVENTORY_SHA256}/$<CONFIG>/application"
        VERBATIM)
endif()
