foreach(REQUIRED IN ITEMS CORE WCX STAGE ZIP SOURCE)
    if(NOT DEFINED ${REQUIRED} OR "${${REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${REQUIRED} for TC package")
    endif()
endforeach()
file(MAKE_DIRECTORY "${STAGE}")
get_filename_component(ZIP_DIRECTORY "${ZIP}" DIRECTORY)
file(MAKE_DIRECTORY "${ZIP_DIRECTORY}")
file(COPY_FILE "${WCX}" "${STAGE}/hc_isoimage_tc.wcx" ONLY_IF_DIFFERENT)
# The .wcx artifact is x64 too. Register the conventional .wcx64 name to select
# the 64-bit plugin explicitly; neither binary supports 32-bit Total Commander.
file(COPY_FILE "${WCX}" "${STAGE}/hc_isoimage_tc.wcx64" ONLY_IF_DIFFERENT)
file(COPY_FILE "${CORE}" "${STAGE}/hc_iso_core.dll" ONLY_IF_DIFFERENT)
file(CONFIGURE OUTPUT "${STAGE}/pluginst.inf" CONTENT
"[plugininstall]\ndescription=HyperCommander ISO Image (64-bit Unicode)\nversion=1.0.0\ntype=wcx\nfile=hc_isoimage_tc.wcx64\ndefaultdir=hcIsoImage\ndefaultextension=iso\n"
    NEWLINE_STYLE CRLF)
foreach(FILE IN ITEMS COPYING THIRD_PARTY_NOTICES.txt source-lock.json)
    file(COPY_FILE "${SOURCE}/packaging/${FILE}" "${STAGE}/${FILE}" ONLY_IF_DIFFERENT)
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${STAGE}/hc-iso-source.zip" --format=zip
    include src adapters packaging CMakeLists.txt
    WORKING_DIRECTORY "${SOURCE}" COMMAND_ERROR_IS_FATAL ANY)
# An explicit list excludes stale files and all user configuration from updates.
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${ZIP}" --format=zip
    pluginst.inf hc_isoimage_tc.wcx hc_isoimage_tc.wcx64 hc_iso_core.dll
    COPYING THIRD_PARTY_NOTICES.txt
    source-lock.json hc-iso-source.zip
    WORKING_DIRECTORY "${STAGE}" COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "Total Commander x64 plugin package: ${ZIP}")
