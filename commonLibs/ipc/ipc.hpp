#ifndef __HDR_CMNLIBS_IPC__
#define __HDR_CMNLIBS_IPC__

#include "cmnContainers.hpp"
#include "win32api/W32API_NtDll.hpp"

namespace nsCmn
{
    namespace nsIPC
    {
        using TyFnIPCCallback = void(WINAPI*)( LPCSTR IPCName, LPCVOID MessageBuf, DWORD MessageLen, LPVOID AnswerBuf, DWORD AnswerLen, LPVOID Context );

        namespace nsDetail
        {
            const int LPC_REQUEST            = 0x01;
            const int LPC_REPLY              = 0x02;
            const int LPC_DATAGRAM           = 0x03;
            const int LPC_LOST_REPLY         = 0x04;
            const int LPC_PORT_CLOSED        = 0x05;
            const int LPC_CLIENT_DIED        = 0x06;
            const int LPC_EXCEPTION          = 0x07;
            const int LPC_DEBUG_EVENT        = 0x08;
            const int LPC_ERROR_EVENT        = 0x09;
            const int LPC_CONNECTION_REQUEST = 0x0A;

            typedef struct tagLpcWorkerThread
            {
                HANDLE                              Handle;
                HANDLE                              ParentSemaphore;
                HANDLE                              Event;
                nsW32API::nsNtDeclare::LPC_MESSAGE* Message;
                DWORD                               LastActive;
                BOOL                                ShuttingDown;
                BOOL *                              Freed;
            } LPC_WORKER_THREAD, *PLPC_WORKER_THREAD;

            typedef struct tagLpcQueue
            {
                LPSTR                                Name;
                TyFnIPCCallback                      Callback;
                LPVOID                               Context;
                int                                  MaxThreadCount;
                int                                  MadQueueLength;
                HANDLE                               Port;
                DWORD                                Counter;
                nsW32API::nsNtDeclare::LPC_MESSAGE*  Message;
                CRITICAL_SECTION                     CriticalSection;
                HANDLE                               Semaphore;
                BOOL                                 Shutdown;
                HANDLE                               PortThread;
                HANDLE                               DispatchThread;
                CCollection<PLPC_WORKER_THREAD>*     WorkerThreads;
            } LPC_QUEUE, *PLPC_QUEUE;

            bool InitializeIPC();
            bool CloseIpc();

            bool CreateLpcQueue( LPCSTR IPCName, TyFnIPCCallback Callback, LPVOID Context, DWORD MaxThreadCount, DWORD MaxQueueLen, PSECURITY_DESCRIPTOR SecurityDesc );
            bool DestroyLpcQueue( LPCSTR IPCName );

            int WINAPI LpcPortThread( LPC_QUEUE* Queue );
            int WINAPI LpcDispatchThread( LPC_QUEUE* Queue );
            int WINAPI LpcWorkerThread( LPC_WORKER_THREAD* WorkerThread );
        }

        bool CreateIpcQueue( LPCSTR IPCName, TyFnIPCCallback Callback, LPVOID Context = nullptr, DWORD MaxThreadCount = 16, DWORD MaxQueueLen = 0x1000, PSECURITY_DESCRIPTOR SecurityDesc = nullptr );
        bool SendIpcMessage( LPCSTR IPCName, PVOID MessageBuf, DWORD MessageLen, PVOID AnswerBuf = nullptr, DWORD AnswerLen = 0, DWORD AnswerTimeout = INFINITE, bool HandleMessage = false );
        bool DestroyIpcQueue( LPCSTR IPCName );

    } // nsIPC

} // nsCmn

//#include "Accctrl.h"
//
//// ----------------------------------------------------------
//// Function ptrs for AddAccessForEveryone
//// ----------------------------------------------------------
//
//typedef DWORD (WINAPI * PFN_GETSECURITYINFO)
//(
//  HANDLE hHandle,
//  SE_OBJECT_TYPE dwObjectType,
//  SECURITY_INFORMATION dwSecurityInfo,
//  PSID *ppSidOwner,
//  PSID *ppSidGroup,
//  PACL *ppDacl,
//  PACL *ppSacl,
//  PSECURITY_DESCRIPTOR *ppSecurityDescriptor
//);
//
//typedef DWORD (WINAPI * PFN_SETSECURITYINFO)
//(
//  HANDLE hHandle,
//  SE_OBJECT_TYPE dwObjectType,
//  SECURITY_INFORMATION dwSecurityInfo,
//  PSID pSidOwner,
//  PSID pSidGroup,
//  PACL pDacl,
//  PACL pSacl
//);
//
//typedef DWORD (WINAPI * PFN_SETENTRIESINACLA)
//(
//  ULONG cCountOfExplicitEntries,
//  PEXPLICIT_ACCESS pListOfExplicitEntries,
//  PACL pOldAcl,
//  PACL *ppNewAcl
//);
//
//// --------------------------------------------------
//// NT Local Procedure Call and IPC structure typedefs
//// --------------------------------------------------
//
//
//
//typedef struct tagSecurityQualityOfService
//{
//  DWORD Size;
//  DWORD Dummy[3];
//  #ifdef _WIN64
//    DWORD Dummy2;
//  #endif
//} SECURITY_QOS;

//// ------------------------------------------------
//// Kernel functions
//// ------------------------------------------------
//
//typedef BOOL (WINAPI * PFN_PROCESSIDTOSESSIONID)(DWORD dwProcessId, DWORD *pdwSessionId);
//
//typedef DWORD (WINAPI * PFN_GETINPUTSESSIONID)(void);
//
//// ------------------------------------------------
//// Struct to hold error information for NT LPC
//// ------------------------------------------------
//
//typedef struct tagLpcErrorInfo
//{
//  DWORD dwErrorCode;
//  wchar_t szSymbolName[60];
//  wchar_t szDescription[512];
//} LPC_ERROR_INFO, *PLPC_ERROR_INFO;
//
//// ------------------------------------------------
//// Struct to hold message types for NT LPC
//// ------------------------------------------------
//
//typedef struct tagLpcMessageType
//{
//  USHORT dwMessageType;
//  wchar_t szDescription[128];
//} LPC_MESSAGE_TYPE, *PLPC_MESSAGE_TYPE;
//

#endif // __HDR_CMNLIBS_IPC__
