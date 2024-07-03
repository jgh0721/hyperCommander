#ifndef __HDR_CMNLIB_W32API_CMNDEFS__
#define __HDR_CMNLIB_W32API_CMNDEFS__

#include <sal.h>
#include <windows.h>

// NT Native 관련 선언
typedef __success( return >= 0 )LONG NTSTATUS;


#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED                  ((NTSTATUS)0xC0000002L)
#endif

#endif // __HDR_CMNLIB_W32API_CMNDEFS__