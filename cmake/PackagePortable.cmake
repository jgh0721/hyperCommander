foreach(NAME IN ITEMS SOURCE ROOT OUTPUT VERSION SYMBOLS)
    if(NOT DEFINED ${NAME} OR "${${NAME}}" STREQUAL "")
        message(FATAL_ERROR "Missing portable package argument: ${NAME}")
    endif()
endforeach()
message(STATUS "Portable package version: ${VERSION}")
# MSBuild can pass an encoded stdin marker to a terminal command. The script
# takes named arguments only; close stdin so it is never bound as pipeline input.
execute_process(COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${SOURCE}/tools/update/New-PortablePackage.ps1"
    -Root "${ROOT}" -OutputRoot "${OUTPUT}" -Version "${VERSION}" -SymbolsList "${SYMBOLS}"
    INPUT_FILE NUL RESULT_VARIABLE RESULT)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Signed portable packaging failed (${RESULT})")
endif()
