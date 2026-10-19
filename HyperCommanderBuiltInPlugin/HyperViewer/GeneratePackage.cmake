if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
# The package holds HyperViewer.dll and its manifest. The in-process Qt viewer uses
# product DLLs from ROOT; HyperEditor.exe remains a separate F4 application.
if(NOT PACKAGE OR PACKAGE MATCHES "^/" OR PACKAGE MATCHES "[.][.]" OR PACKAGE MATCHES "[\\]")
    message(FATAL_ERROR "HyperViewer package path is invalid: ${PACKAGE}")
endif()
file(MAKE_DIRECTORY "${OUTPUT}")
file(GLOB STALE LIST_DIRECTORIES TRUE "${OUTPUT}/*")
foreach(ENTRY IN LISTS STALE)
    get_filename_component(NAME "${ENTRY}" NAME)
    if(NOT NAME MATCHES "^(HyperViewer[.]dll|hyperviewer[.]json|source-lock[.]json)$")
        file(REMOVE_RECURSE "${ENTRY}")
    endif()
endforeach()
file(COPY_FILE "${VIEWER}" "${OUTPUT}/HyperViewer.dll" ONLY_IF_DIFFERENT)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/ProductSigning.cmake")
hc_sign_packaged_file("${OUTPUT}/HyperViewer.dll")
file(SHA256 "${OUTPUT}/HyperViewer.dll" VIEWER_HASH)
# Qt plugins loaded by the viewer from the product runtime (Plugins=Libs).
file(GLOB PLUGINS "${RUNTIME}/Libs/platforms/qwindows*.dll" "${RUNTIME}/Libs/imageformats/*.dll"
    "${RUNTIME}/Libs/iconengines/*.dll" "${RUNTIME}/Libs/styles/*.dll" "${RUNTIME}/Libs/printsupport/*.dll")
file(GET_RUNTIME_DEPENDENCIES LIBRARIES "${VIEWER}" ${PLUGINS}
    DIRECTORIES "${RUNTIME}" "${RUNTIME}/Libs/imageformats" RESOLVED_DEPENDENCIES_VAR RESOLVED UNRESOLVED_DEPENDENCIES_VAR UNRESOLVED
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-")
file(REAL_PATH "${RUNTIME}" ROOT)
set(INPUTS ${PLUGINS})
foreach(INPUT IN LISTS RESOLVED)
    file(REAL_PATH "${INPUT}" INPUT)
    get_filename_component(FOLDER "${INPUT}" DIRECTORY)
    # System DLLs resolve outside ROOT and are covered by the OS, not the package.
    if(FOLDER STREQUAL ROOT)
        list(APPEND INPUTS "${INPUT}")
    endif()
endforeach()
set(DEPENDENCIES "{}")
set(COUNT 0)
foreach(INPUT IN LISTS INPUTS)
    file(RELATIVE_PATH NAME "${ROOT}" "${INPUT}")
    file(SHA256 "${INPUT}" HASH)
    string(JSON DEPENDENCIES SET "${DEPENDENCIES}" "${NAME}" "\"${HASH}\"")
    math(EXPR COUNT "${COUNT}+1")
endforeach()
if(COUNT GREATER 64)
    message(FATAL_ERROR "HyperViewer pins ${COUNT} dependencies; the manifest allows 64")
endif()
set(MANIFEST [=[{
  "schemaVersion":1, "id":"builtin.hyperviewer", "version":"0.6.0",
  "binary":"@PACKAGE@/HyperViewer.dll", "sha256":"@VIEWER_HASH@",
  "format":"dopus-viewer", "architecture":"x64", "execution":"isolated",
  "protocol":{"major":1,"minimumMinor":0,"maximumMinor":0},
  "extensions":["viewer"], "permissions":["content.read","content.write","ui","process.child"],
  "compatibility":"hyperviewer-v1",
  "dependencies":@DEPENDENCIES@
}
]=])
file(CONFIGURE OUTPUT "${OUTPUT}/hyperviewer.json" CONTENT "${MANIFEST}" @ONLY NEWLINE_STYLE CRLF)
file(CONFIGURE OUTPUT "${OUTPUT}/source-lock.json" CONTENT [=[{"product":"HyperViewer","implementationStage":"P5","protocolVersion":2,"sdkUrl":"https://cdn2.gpsoft.com.au/files/Misc/opus_sdk.zip","sdkSha256":"b908d0f9cf34cc93e2e89ea48856bd192cc5d2e45abb42c1d581500f928d59b9","crt":"v145 x64 Hybrid CRT"}
]=] NEWLINE_STYLE CRLF)

file(READ "${OUTPUT}/source-lock.json" SOURCE_LOCK)
file(READ "${ENGINE_LOCK}" ENGINES)
string(JSON SOURCE_LOCK SET "${SOURCE_LOCK}" "engines" "${ENGINES}")
file(CONFIGURE OUTPUT "${OUTPUT}/source-lock.json" CONTENT "${SOURCE_LOCK}\n" NEWLINE_STYLE CRLF)
