include_directories( ${CMAKE_CURRENT_LIST_DIR} )

set(EXTERNAL_SRC "")

# Everything SDK
include_directories( ${CMAKE_CURRENT_LIST_DIR}/Everything/src/include )
include_directories( ${CMAKE_CURRENT_LIST_DIR}/Everything/src/ipc )
# Only hc-search-index uses the SDK, built from source there; nothing links Everything64.dll.

# QDirectoryEdit
set( EXTERNAL_SRC ${EXTERNAL_SRC}
        ../externalLibs/QDirectoryEdit/QDirectoryEdit.cpp
        ../externalLibs/QDirectoryEdit/QDirectoryEdit.hpp
)

# TotalCommander
include_directories( ${CMAKE_CURRENT_LIST_DIR}/TotalCommander )
set( EXTERNAL_SRC ${EXTERNAL_SRC}
        ../externalLibs/TotalCommander/tc_listplug.h
)

