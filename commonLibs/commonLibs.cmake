
include_directories( ${CMAKE_CURRENT_LIST_DIR} )
include_directories( ${CMAKE_CURRENT_LIST_DIR}/fmt-10.2.0/include )

set( COMMONLIBS_SRC
    ../commonLibs/cmnBase.hpp
    ../commonLibs/cmnCompilerDetection.hpp
    ../commonLibs/cmnConcurrent.cpp
    ../commonLibs/cmnConcurrent.hpp
    ../commonLibs/cmnContainers.hpp
    ../commonLibs/cmnContainers_mpmc_bounded_q.hpp
    ../commonLibs/cmnDateTime.cpp
    ../commonLibs/cmnDateTime.hpp
    ../commonLibs/cmnIncludeLibrary.hpp
    ../commonLibs/cmnOAL_Defs.hpp
    ../commonLibs/cmnPath.cpp
    ../commonLibs/cmnPath.hpp
    ../commonLibs/cmnProcess.cpp
    ../commonLibs/cmnProcess.hpp
    ../commonLibs/cmnSecurity.cpp
    ../commonLibs/cmnSecurity.hpp
    ../commonLibs/cmnSystemDetection.hpp
    ../commonLibs/cmnTextUtils.cpp
    ../commonLibs/cmnTextUtils.hpp
    ../commonLibs/cmnTypes.hpp
    ../commonLibs/cmnUtils.cpp
    ../commonLibs/cmnUtils.hpp
    ../commonLibs/concurrent/cmnConditionVariable.cpp
    ../commonLibs/concurrent/cmnConditionVariable.hpp
    ../commonLibs/concurrent/cmnMutex.cpp
    ../commonLibs/concurrent/cmnMutex.hpp
    ../commonLibs/concurrent/cmnSingleton.hpp
    ../commonLibs/ipc/ipc.cpp
    ../commonLibs/ipc/ipc.hpp
    ../commonLibs/win/winLIBMgr.hpp
    ../commonLibs/win/winUtils.cpp
    ../commonLibs/win/winUtils.hpp
    ../commonLibs/win32api/W32API_CmnDefs.hpp
    ../commonLibs/win32api/W32API_NtDll.hpp
    ../commonLibs/win32api/W32API_NtDll_Defs.hpp
)

set( COMMONLIBS_SRC
        ${COMMONLIBS_SRC}
        ../commonLibs/Qt/cmnQtDelegator.hpp
        ../commonLibs/Qt/cmnQtUtils.cpp
        ../commonLibs/Qt/cmnQtUtils.hpp
)

set( COMMONLIBS_SRC
        ${COMMONLIBS_SRC}
        ../commonLibs/win/winLIBMgr.hpp
)