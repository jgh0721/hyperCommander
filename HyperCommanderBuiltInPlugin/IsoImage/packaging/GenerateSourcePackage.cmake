# Package only rebuild inputs; local plans, reports and Markdown are not inputs.
function(hc_iso_source_package SOURCE OUTPUT)
    file(GLOB_RECURSE SOURCES LIST_DIRECTORIES false RELATIVE "${SOURCE}"
        "${SOURCE}/include/*.h"
        "${SOURCE}/src/*.cpp"
        "${SOURCE}/src/*.hpp"
        "${SOURCE}/adapters/*.cpp"
    )
    list(APPEND SOURCES
        packaging/CMakeLists.txt
        packaging/COPYING
        packaging/THIRD_PARTY_NOTICES.txt
        packaging/source-lock.json
    )
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUTPUT}" --format=zip ${SOURCES}
        WORKING_DIRECTORY "${SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
endfunction()
