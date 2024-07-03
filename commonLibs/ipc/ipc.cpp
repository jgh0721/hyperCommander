#include "stdafx.h"
#include "ipc.hpp"

#include "win32api/W32API_NtDll.hpp"

#ifndef _CRT_SECURE_NO_DEPRECATE
#define _CRT_SECURE_NO_DEPRECATE
#endif

using namespace nsW32API;
using namespace nsW32API::nsNtDeclare;

namespace nsCmn
{
    namespace nsIPC
    {
        namespace nsDetail
        {
            static bool LPCReady = false;
            static bool InitIPCOnce = false;
            static CRITICAL_SECTION LPCSection = { 0, };
            static CCollection<PLPC_QUEUE> LPCList;

            bool InitializeIPC()
            {
                InitIPCOnce = false;
                bool result = false;

                __try
                {
                    InitializeCriticalSection(&LPCSection);
                    result = true;
                    InitIPCOnce = true;
                }
                __except( EXCEPTION_EXECUTE_HANDLER )
                {
                    result = false;
                }

                return result;
            }

            //bool CloseIpc(void)
            //{
            //  // allow close when init wasn't previously called
            //  if (!InitCalled)
            //    return true;
            //
            //  bool result = false;
            //  DeleteCriticalSection(&LpcSection);
            //  if (LpcCounterBuf != NULL)
            //    UnmapViewOfFile(LpcCounterBuf);
            //  result = true;
            //  InitCalled = false;
            //  return result;
            //}

            bool CreateLpcQueue( LPCSTR IPCName, TyFnIPCCallback Callback, LPVOID Context, DWORD MaxThreadCount, DWORD MaxQueueLen, PSECURITY_DESCRIPTOR SecurityDesc )
            {
                bool   result = false;
                HANDLE hProcess;
                if( DuplicateHandle( GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &hProcess, 0, FALSE, DUPLICATE_SAME_ACCESS ) )
                {
                    // NOTE:  GetLastError returns 122 after coming out of this function, although no errors were raised
                    AddAccessForEveryone( hProcess, SYNCHRONIZE );
                    SetLastError( 0 ); // clear 122 to avoid confusion

                    CloseHandle( hProcess );
                }

                if( !LPCReady )
                {
                    if( !InitIPCOnce )
                        InitializeIPC();

                    LPCReady = true;
                }

                result = true;
                EnterCriticalSection( &LPCSection );
                __try
                {
                    for( int i = 0; i < LPCList.GetCount(); i++ )
                    {
                        if( _stricmp( LPCList[i]->Name, IPCName ) == 0 )
                        {
                            result = false;
                            break;
                        }
                    }
                }
                __finally
                {
                    LeaveCriticalSection( &LPCSection );
                }

                if( result )
                {
                    UNICODE_STRING unicodeIpcName;
                    ZeroMemory( &unicodeIpcName, sizeof( UNICODE_STRING) );
                    InitLpcFuncs();
                    InitLpcName( LPCReady, &unicodeIpcName );

                    SECURITY_ATTRIBUTES securityAttr;
                    SECURITY_DESCRIPTOR securityDesc;
                    POBJECT_ATTRIBUTES  pObjectAttr;
                    if( !SecurityDesc )
                        InitSecurityAttributes( &securityAttr, &securityDesc, (GetMadCHookOption( LIMITED_IPC_PORT ) != NULL) ? TRUE : FALSE, FALSE );
                    else
                        securityDesc.Dacl = nullptr;
                    pObjectAttr                     = (OBJECT_ATTRIBUTES *)LocalAlloc( LPTR, sizeof(OBJECT_ATTRIBUTES) );
                    pObjectAttr->Length             = sizeof(OBJECT_ATTRIBUTES);
                    pObjectAttr->ObjectName         = &unicodeIpcName;
                    pObjectAttr->SecurityDescriptor = (SecurityDesc) ? SecurityDesc : (&securityDesc);
                    HANDLE port                     = (void *)-1;

                    // Notes about NtCreatePort:
                    // - if it fails, it returns a DWORD which indicates the error--GetLastError doesn't show the error code.  CheckNtFunction will look the error up and log
                    //   additional information about it
                    // - the object name you specify must begin with \, or else it fails.
                    // - contrary to the information on /www.windowsitlibrary.com, you CANNOT pass 0 for the 2 'max' length fields.  If you do, then the ActualMessageLength
                    //   field in the lpc message received by NtReplyWaitReceive will always be 0 and the data buffer will always contain NULLs.  The last param is passed
                    //   in as 0
                    result = CheckNtFunction( CNtCreatePort, pNtCreatePort( &port, pObjectAttr, 260, 40 + 260, 0 ) );

                    LocalFree( unicodeIpcName.Buffer );
                    LocalFree( pObjectAttr );
                    if( securityDesc.Dacl )
                        LocalFree( securityDesc.Dacl );

                    if( !result )
                    {
                        return false;
                    }

                    if( result )
                    {
                        if( MaxThreadCount > 64 )
                            MaxThreadCount = 64;

                        DWORD newThreadId;

                        // create a new LPC queue
                        PLPC_QUEUE newLpcQueue = (PLPC_QUEUE)LocalAlloc( LPTR, sizeof(LPC_QUEUE) );
                        newLpcQueue->Name       = (LPSTR)LocalAlloc( LPTR, lstrlenA( IPCName ) + 1 );
                        lstrcpyA( newLpcQueue->Name, IPCName );
                        newLpcQueue->WorkerThreads  = new CCollection<PLPC_WORKER_THREAD>;
                        newLpcQueue->Callback       = Callback;
                        newLpcQueue->Context        = Context;
                        newLpcQueue->MaxThreadCount = MaxThreadCount;
                        newLpcQueue->MadQueueLength = MaxThreadCount;
                        newLpcQueue->Port           = port;
                        newLpcQueue->Semaphore      = CreateSemaphoreW( nullptr, 0, INT_MAX, nullptr );
                        InitializeCriticalSection( &newLpcQueue->CriticalSection );
                        newLpcQueue->PortThread     = CreateThread( nullptr, 0, (LPTHREAD_START_ROUTINE)&LpcPortThread, newLpcQueue, 0, &newThreadId );
                        newLpcQueue->DispatchThread = CreateThread( nullptr, 0, (LPTHREAD_START_ROUTINE)&LpcDispatchThread, newLpcQueue, 0, &newThreadId );
                        SetThreadPriority( newLpcQueue->PortThread, 7 );
                        SetThreadPriority( newLpcQueue->DispatchThread, THREAD_PRIORITY_ABOVE_NORMAL );

                        EnterCriticalSection( &LPCSection );

                        __try
                        {
                            // add it to the global list
                            LPCList.Add( newLpcQueue );
                        }
                        __finally
                        {
                            LeaveCriticalSection( &LPCSection );
                        }
                    }
                }

                return result;
            }

            int WINAPI LpcPortThread( LPC_QUEUE *lpcQueue )
            {
                int result = 0;
                HANDLE hPort = 0;
                HANDLE hPrevPort = 0;
                PLPC_SECTION_MAP_INFO pMapInfo = NULL;
                PLPC_MESSAGE pMsg = NULL;

                hPrevPort = 0;
                while (true)
                {
                    pMsg = (PLPC_MESSAGE) LocalAlloc(LPTR, sizeof(LPC_MESSAGE) + sizeof(LPC_MESSAGE_PRIVATE) + 128);

                    NTSTATUS retVal;
                    LARGE_INTEGER timeOut;
                    timeOut.QuadPart = -100000;

                    // block waiting for the client to send a message with NtConnectPort()
                    if ((pNtReplyWaitReceivePortEx) && (!GetMadCHookOption(DISABLE_IPC_FIX_1)))
                    {
                        do
                        {
                            retVal = pNtReplyWaitReceivePortEx(lpcQueue->Port, NULL, NULL, (PLPC_MESSAGE) &(pMsg->ActualMessageLength), &timeOut);
                        } while ((retVal == STATUS_TIMEOUT) && (!lpcQueue->Shutdown));
                    }
                    else
                    retVal = pNtReplyWaitReceivePort(lpcQueue->Port, NULL, NULL, (PLPC_MESSAGE) &(pMsg->ActualMessageLength));

                    // resource cleanup
                    if (hPrevPort)
                    {
                        CloseHandle(hPrevPort);
                        hPrevPort = 0;
                    }

                    if ((!retVal) && (CheckLpcMessageType(pMsg->MessageType)) && (pMsg->ActualMessageLength > 0))
                    {
                        Log(L"LpcPortThread: Got message from client\n");

                        // some newer OSs have additional fields in the LPC_MESSAGE_PRIVATE structure (ouch)
                        // so we have to calculate where our private data block is located
                        pMsg->PrivateOffset = (DWORD) ((ULONG_PTR) (&(pMsg->ActualMessageLength)) - (ULONG_PTR) pMsg + pMsg->TotalMessageLength - sizeof(LPC_MESSAGE_PRIVATE));
                        LPC_MESSAGE_PRIVATE *Private = (LPC_MESSAGE_PRIVATE*) ((ULONG_PTR) pMsg + pMsg->PrivateOffset);

                        if (lpcQueue->Shutdown)
                        {
                            // reject the connection request (acceptIt = 0)
                            Log(L"LpcPortThread: Rejecting connection request because queue is shutting down\n");
                            CheckNtFunction(CNtAcceptConnectPort, pNtAcceptConnectPort(&hPort, 0, (PLPC_MESSAGE) &(pMsg->ActualMessageLength), 0, 0, NULL));
                            LocalFree(pMsg);

                            // break out of thread while loop
                            break;
                        }

                        if (!IsBadReadPtr(Private, 20))
                        {
                            // due to 32/64 bit LPC incompatabilities, pMsg->ClientProcessId is not reliable, so we extract the PID from our own transport buffer
                            if (Private->ProcessId)
                                pMsg->ClientProcessId = Private->ProcessId;

                            // set the process ID.  The client checks this value as verification that the command completed
                            Private->ProcessId = GetCurrentProcessId() + 1;

                            hPort = 0;

                            #ifdef _WIN64
                            pMapInfo = (PLPC_SECTION_MAP_INFO) LocalAlloc(LPTR, sizeof(LPC_SECTION_MAP_INFO));
                            pMapInfo->Size = sizeof(LPC_SECTION_MAP_INFO);
                            #else
                            pMapInfo = (P_LPC_SECTION_MAP_INFO) LocalAlloc(LPTR, sizeof(LPC_SECTION_MAP_INFO64));
                            if (Is64bitOS())
                            // it doesn't make much sense, but when running on a 64bit OS even a 32bit
                            // process has to use this 64bit structure when calling NtAcceptConnectPort()
                            pMapInfo->Size = sizeof(LPC_SECTION_MAP_INFO64);
                            else
                            pMapInfo->Size = sizeof(LPC_SECTION_MAP_INFO);
                            #endif

                            // accept the connection request (acceptIt = 1)
                            Log(L"LpcPortThread: Accepting connection request\n");
                            if (CheckNtFunction(CNtAcceptConnectPort, pNtAcceptConnectPort(&hPort, 0, (PLPC_MESSAGE) &(pMsg->ActualMessageLength), 1, 0, pMapInfo)))
                            {
                                #ifndef _WIN64
                                if (Is64bitOS())
                                {
                                pMapInfo->SectionSize       = ((LPC_SECTION_MAP_INFO64*) pMapInfo)->SectionSize;
                                pMapInfo->ServerBaseAddress = ((LPC_SECTION_MAP_INFO64*) pMapInfo)->ServerBaseAddress;
                                }
                                #endif

                                if ((pMapInfo->SectionSize > 0) && (Private->MessageLength > 0))
                                {
                                    if (Private->MessageLength > pMapInfo->SectionSize)
                                        Private->MessageLength = pMapInfo->SectionSize;
                                    PLPC_MESSAGE pMsg2 = NULL;
                                    pMsg2 = (PLPC_MESSAGE) LocalAlloc(LPTR, (ULONG_PTR) &Private->Data - (ULONG_PTR) pMsg + Private->MessageLength);
                                    memcpy(pMsg2, pMsg, (ULONG_PTR) &Private->Data - (ULONG_PTR) pMsg);
                                    Private = (LPC_MESSAGE_PRIVATE*) ((ULONG_PTR) pMsg2 + pMsg2->PrivateOffset);
                                    memcpy(&Private->Data, pMapInfo->ServerBaseAddress, Private->MessageLength);
                                    LocalFree(pMsg);
                                    pMsg = pMsg2;
                                }
                                else
                                if (Private->MessageLength > sizeof(Private->Data))
                                    Private->MessageLength = sizeof(Private->Data);

                                // complete the connection.  Client will unblock after this function completes
                                Log(L"LpcPortThread: Completing connection request\n");

                                pNtCompleteConnectPort(hPort);

                                // if we close the port here, the client will get into trouble
                                // SendIpcMessage might then fail (especially in Windows 7)
                                // so we have to delay closing the port
                                // we simply close it the next time something happens
                                // usually that should be a LPC_CLOSE_PORT message
                                hPrevPort = hPort;

                                pMsg->Answer.Length = Private->AnswerLength;
                                if (InitIpcAnswer(false, lpcQueue->Name, Private->Counter, pMsg->ClientProcessId, &pMsg->Answer, Private->Session))
                                {
                                    // notify client that we've received the message
                                    if (pMsg->Answer.Length != 0)
                                    SetEvent(pMsg->Answer.Event1);

                                    // copy the message to the lpc queue where the dispatch thread can get to it
                                    strcpy(pMsg->Name, lpcQueue->Name);

                                    pMsg->Callback = lpcQueue->Callback;
                                    pMsg->Context = lpcQueue->Context;
                                    EnterCriticalSection(&lpcQueue->CriticalSection);
                                    __try
                                    {
                                        PLPC_MESSAGE *ppLpcMsg = &(lpcQueue->Message);
                                        while (*ppLpcMsg != NULL)
                                        {
                                            ppLpcMsg = &((*ppLpcMsg)->Next);
                                        }

                                        *ppLpcMsg = pMsg;
                                    }
                                    __finally
                                    {
                                        LeaveCriticalSection(&lpcQueue->CriticalSection);
                                    }

                                    ReleaseSemaphore(lpcQueue->Semaphore, 1, NULL);
                                }
                                else
                                LocalFree(pMsg);
                            }
                            else
                            {
                                // reject the connection request (acceptIt = 0)
                                Log(L"LpcPortThread: Rejecting connection request because accepting it failed\n");
                                Private->ProcessId = 0;
                                CheckNtFunction(CNtAcceptConnectPort, pNtAcceptConnectPort(&hPort, 0, (PLPC_MESSAGE) &(pMsg->ActualMessageLength), 0, 0, NULL));
                                LocalFree(pMsg);
                            }

                            LocalFree(pMapInfo);
                        }
                        else
                        {
                            LocalFree(pMsg);
                            if ((lpcQueue->Shutdown) && (!GetMadCHookOption(DISABLE_IPC_FIX_1)))
                                break;
                        }
                    }
                    else
                    {
                        LocalFree(pMsg);
                        if ((lpcQueue->Shutdown) && (!GetMadCHookOption(DISABLE_IPC_FIX_1)))
                            break;
                    }
                }

                return result;
            }

        } // nsDetail

        bool CreateIpcQueue( LPCSTR IPCName, TyFnIPCCallback Callback, LPVOID Context, DWORD MaxThreadCount, DWORD  MaxQueueLen, PSECURITY_DESCRIPTOR SecurityDesc )
        {
            BOOL result = false;
            EnableAllPrivileges();

            if( MaxThreadCount == 0 )
                MaxThreadCount = 16;

            if( Callback != nullptr )
                result = nsDetail::CreateLpcQueue( IPCName, Callback, Context, MaxThreadCount, MaxQueueLen, SecurityDesc );

            return result;
        }

        bool SendIpcMessage( LPCSTR IPCName, PVOID MessageBuf, DWORD MessageLen, PVOID AnswerBuf, DWORD AnswerLen, DWORD AnswerTimeout, bool HandleMessage )
        {
            DWORD dwError = GetLastError();
            BOOL result = false;
            HANDLE hProcess = NULL;
            PIPED_IPC_REC pipedIpcRec = {0};
            BOOL answerInitialized = false;

            if (UseLpcTransport())
            {
                LPC_MESSAGE_PRIVATE lpcMessagePrv;
                SECURITY_QOS securityQos;

                ZeroMemory(&lpcMessagePrv, sizeof(lpcMessagePrv));
                lpcMessagePrv.Counter = GetLpcCounter();
                lpcMessagePrv.ProcessId = GetCurrentProcessId();

                pipedIpcRec.Answer.Length = answerLen;
                answerInitialized = InitIpcAnswer(true, IPCName, lpcMessagePrv.Counter, GetCurrentProcessId(), &pipedIpcRec.Answer);
                if (answerInitialized)
                {
                    LPC_SECTION_INFO lpcSectionInfo;
                    PLPC_SECTION_INFO pLpcSectionInfo = NULL;
                    LPC_SECTION_MAP_INFO lpcSectionMapInfo, *pLpcSectionMapInfo = NULL;
                    DWORD bufLen = 0;
                    HANDLE port = NULL;

                    InitLpcFuncs();
                    UNICODE_STRING uniStr;
                    InitLpcName(IPCName, &uniStr);

                    ZeroMemory(&lpcSectionInfo, sizeof(lpcSectionInfo));

                    ZeroMemory(&securityQos, sizeof(securityQos));
                    lpcMessagePrv.MessageLength = MessageLen;
                    lpcMessagePrv.Session = GetProcessSessionId(GetCurrentProcessId());
                    lpcMessagePrv.AnswerLength = answerLen;

                    // see if the message is short enough to be sent in full directly with the API. Messages that are too long are copied to mapped memory
                    // and the map info is sent to the server instead of the message itself
                    if (MessageLen > sizeof(lpcMessagePrv.Data))
                    {
                        // copy message contents to shared memory
                        //bufLen = sizeof(lpcMessagePrv) - sizeof(lpcMessagePrv.Data);
                        ZeroMemory(&lpcSectionMapInfo, sizeof(lpcSectionMapInfo));
                        lpcSectionInfo.Size = sizeof(lpcSectionInfo);
                        lpcSectionInfo.SectionHandle = CreateFileMapping((HANDLE) -1, NULL, PAGE_READWRITE, 0, MessageLen, NULL);
                        lpcSectionInfo.SectionSize = MessageLen;
                        lpcSectionInfo.ClientBaseAddress = MapViewOfFile(lpcSectionInfo.SectionHandle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
                        if (lpcSectionInfo.ClientBaseAddress != NULL)
                        {
                        memcpy(lpcSectionInfo.ClientBaseAddress, MessageBuf, MessageLen);
                        UnmapViewOfFile(lpcSectionInfo.ClientBaseAddress);
                        lpcSectionInfo.ClientBaseAddress = NULL;
                        }
                        lpcSectionMapInfo.Size = sizeof(lpcSectionMapInfo);
                        lpcSectionMapInfo.SectionSize = MessageLen;
                        pLpcSectionInfo = &lpcSectionInfo;
                        pLpcSectionMapInfo = &lpcSectionMapInfo;
                    }
                    else
                    {
                        // short message--send directly using API
                        //bufLen = sizeof(lpcMessagePrv) - sizeof(lpcMessagePrv.Data) + messageLen;
                        memcpy(lpcMessagePrv.Data, MessageBuf, MessageLen);
                        pLpcSectionInfo = NULL;
                        pLpcSectionMapInfo = NULL;
                    }
                    bufLen = sizeof(lpcMessagePrv);

                    // Calling NtConnectPort can sometimes fail with "unsufficient resources".
                    // This can happen if a *lot* of threads send IPCs at the same time.
                    // So we call it in a loop until it succeeds, timeout = 250ms.
                    DWORD t1 = GetTickCount();
                    while (true)
                    {
                        port = NULL;
                        NTSTATUS result = pNtConnectPort(&port, &uniStr, &securityQos, pLpcSectionInfo, pLpcSectionMapInfo, NULL, &lpcMessagePrv, &bufLen);
                        // There is a race condition on closing the handle between this thread and the Lpc port thread.
                        // If the other thread closes the handle first, then NtConnectPort returns C0000034, but still works.
                        if ((result == 0) || (result == 0xC0000034) || (port != NULL))
                        break;
                        DWORD t2 = GetTickCount();
                        if ((t2 < t1) || (t2 - t1 >= 300))
                        break;
                        Sleep(0);
                    }

                    if (port != 0)
                        CloseHandle(port);

                    Log(L"SendIpcMessage: NtConnectPort complete\n");

                    LocalFree(uniStr.Buffer);

                    if (lpcSectionInfo.SectionHandle != NULL)
                        CloseHandle(lpcSectionInfo.SectionHandle);

                    // after NtConnectPort has executed, ProcessId will have been filled in by the server
                    result = (lpcMessagePrv.ProcessId != 0) && (lpcMessagePrv.ProcessId != GetCurrentProcessId());
                    if (result && (AnswerLen != 0))
                        hProcess = OpenProcess(SYNCHRONIZE, FALSE, lpcMessagePrv.ProcessId - 1);
                    else
                        Log(L"SendIpcMessage: *** LpcPortThread did not fill in the process ID\n");
                }
            }

            // now wait for the answer...
            if (hProcess != NULL)
            {
                if (result && ( AnswerLen != 0))
                {
                    // LPC: Event1 is set after NtCompleteConnectPort is done (port thread)
                    if (WaitFor(pipedIpcRec.Answer.Event1, hProcess, (DWORD) (ULONG_PTR) GetMadCHookOption(SET_INTERNAL_IPC_TIMEOUT), HandleMessage))
                    {
                        // LPC: Event2 is set after the worker thread invokes the callback function
                        if (WaitFor(pipedIpcRec.Answer.Event2, hProcess, AnswerTimeout, HandleMessage))
                            memcpy(AnswerBuf, pipedIpcRec.Answer.Buffer, AnswerLen);
                        else
                            result = false;
                    }
                    else
                        result = false;
                }
                CloseHandle(hProcess);
            }
            else
                Log(L"*** SendIpcMessage: ProcessID was not filled in by server\n");

            // cleanup
            if (answerInitialized)
            {
                CloseIpcAnswer(&pipedIpcRec.Answer);
            }

            SetLastError(dwError);
            return result;
        }

        bool DestroyIpcQueue( LPCSTR IPCName )
        {
            return nsDetail::DestroyLpcQueue( IPCName );
        }

    } // nsIPC
} // nsCmn

//#include "SystemIncludes.h"
//#include "Systems.h"
//#include "SystemsInternal.h"
//
//
//// globals
//static int *LpcCounterBuf = NULL;
//
//// list of possible message types that can be received with NtReplyWaitReceivePort
//static LPC_MESSAGE_TYPE LpcMessageTypes[] = {
//  {0x01, L"LPC_REQUEST"},
//  {0x02, L"LPC_REPLY"},
//  {0x03, L"LPC_DATAGRAM"},
//  {0x04, L"LPC_LOST_REPLY"},
//  {0x05, L"LPC_PORT_CLOSED"},
//  {0x06, L"LPC_CLIENT_DIED"},
//  {0x07, L"LPC_EXCEPTION"},
//  {0x08, L"LPC_DEBUG_EVENT"},
//  {0x09, L"LPC_ERROR_EVENT"},
//  {0x0A, L"LPC_CONNECTION_REQUEST"}
//};
//
//// list of errors that the Nt* functions could return along with the symbol name and a brief description
//static LPC_ERROR_INFO LpcErrorDefinitions[] = {
//   {0xC0000005, L"STATUS_ACCESS_VIOLATION",        L"An Access Violation occured."},
//   {0xC0000008, L"STATUS_INVALID_HANDLE",          L"An invalid handle was specified."},
//   {0xC000000D, L"STATUS_INVALID_PARAMETER",       L"An invalid parameter was passed to a service or function."},
//   {0xC000002E, L"STATUS_INVALID_PORT_ATTRIBUTES", L"Invalid Object Attributes specified to NtCreatePort or invalid Port Attributes specified to NtConnectPort"},
//   {0xC000002F, L"STATUS_PORT_MESSAGE_TOO_LONG",   L"Length of message passed to NtRequestPort or NtRequestWaitReplyPort was longer than the maximum message allowed by the port"},
//   {0xC0000030, L"STATUS_INVALID_PARAMETER_MIX",   L"An invalid combination of parameters was specified."},
//   {0xC0000031, L"STATUS_INVALID_QUOTA_LOWER",     L"An attempt was made to lower a quota limit below the current usage."},
//   {0xC0000032, L"STATUS_DISK_CORRUPT_ERROR",      L"The file system structure on the disk is corrupt and unusable.  Please run the Chkdsk utility on the volume"},
//   {0xC0000033, L"STATUS_OBJECT_NAME_INVALID",     L"Object Name invalid."},
//   {0xC0000034, L"STATUS_OBJECT_NAME_NOT_FOUND",   L"Object Name not found."},
//   {0xC0000035, L"STATUS_OBJECT_NAME_COLLISION",   L"Object Name already exists."},
//   {0xC0000037, L"STATUS_PORT_DISCONNECTED",       L"Attempt to send a message to a disconnected communication port."},
//   {0xC0000038, L"STATUS_DEVICE_ALREADY_ATTACHED", L"An attempt was made to attach to a device that was already attached to another device."},
//   {0xC0000039, L"STATUS_OBJECT_PATH_INVALID",     L"Object Path Component was not a directory object."},
//   {0xC000003A, L"STATUS_OBJECT_PATH_NOT_FOUND",   L"The path does not exist."},
//   {0xC000003B, L"STATUS_OBJECT_PATH_SYNTAX_BAD",  L"Object Path Component was not a directory object."},
//   {0xC000003C, L"STATUS_DATA_OVERRUN",            L"A data overrun error occurred."},
//   {0xC000003D, L"STATUS_DATA_LATE_ERROR",         L"A data late error occurred."},
//   {0xC000003E, L"STATUS_DATA_ERROR",              L"An error in reading or writing data occurred."},
//   {0xC000003F, L"STATUS_CRC_ERROR",               L"A cyclic redundancy check (CRC) checksum error occurred."},
//   {0xC0000040, L"STATUS_SECTION_TOO_BIG",         L"The specified section is too big to map the file."},
//   {0xC0000041, L"STATUS_PORT_CONNECTION_REFUSED", L"The NtConnectPort request is refused."},
//   {0xC0000042, L"STATUS_INVALID_PORT_HANDLE",     L"The type of port handle is invalid for the operation requested."},
//   {0xC0000043, L"STATUS_SHARING_VIOLATION",       L"A file cannot be opened because the share access flags are incompatible."},
//   {0xC0000044, L"STATUS_QUOTA_EXCEEDED",          L"Insufficient quota exists to complete the operation"},
//   {0xC0000045, L"STATUS_INVALID_PAGE_PROTECTION", L"The specified page protection was not valid."},
//   {0xC0000048, L"STATUS_PORT_ALREADY_SET",        L"An attempt to set a processes DebugPort or ExceptionPort was made, but a port already exists in the process"}
//};
//
//static VOID InitLpcName(LPCSTR ipc, UNICODE_STRING *uniString);
//static VOID InitLpcFuncs(void);
//static bool CheckNtFunction(LPCSTR szFunctionName, DWORD dwReturnCode);
//static VOID Log(LPCWSTR szFmt, ...);
//
//
//// Send Message Support
//static bool InitIpcAnswer(bool create, LPCSTR name, DWORD counter, DWORD processId, IPC_ANSWER *answer, DWORD session = 0);
//static void CloseIpcAnswer(IPC_ANSWER *answer);
//static bool WaitFor(HANDLE handle1, HANDLE handle2, DWORD timeout, BOOL handleMessages);
//static int InterlockedIncrementEx(int *value);
//static DWORD GetLpcCounter(void);
//
//// ------------------ Library Initialization Routines ------------------------------------
//
//
//// ------------------ Exported functions ------------------------------------
//
//SYSTEMS_API BOOL WINAPI AddAccessForEveryone(HANDLE processOrService, DWORD access)
//{
//  BOOL result = false;
//  char buffer[24];
//  HMODULE dll = LoadLibraryA(DecryptStr(CAdvApi32, buffer, 24));
//  SECURITY_DESCRIPTOR *pSecurityDescriptor = NULL;
//  EXPLICIT_ACCESS ea[2];
//  ACL *oldDacl = NULL;
//  ACL *newDacl = NULL;
//  PSID pSid1 = NULL;
//  PSID pSid2 = NULL;
//  SE_OBJECT_TYPE type;
//
//  PFN_GETSECURITYINFO pGetSecurityInfo = (PFN_GETSECURITYINFO) GetProcAddress(dll, DecryptStr(CGetSecurityInfo, buffer, 24));
//  PFN_SETSECURITYINFO pSetSecurityInfo = (PFN_SETSECURITYINFO) GetProcAddress(dll, DecryptStr(CSetSecurityInfo, buffer, 24));
//  PFN_SETENTRIESINACLA pSetEntriesInACL = (PFN_SETENTRIESINACLA) GetProcAddress(dll, DecryptStr(CSetEntriesInAclA, buffer, 24));
//
//  if ((pGetSecurityInfo != NULL) && (pSetSecurityInfo != NULL) && (pSetEntriesInACL != NULL))
//  {
//    // NOTE:  The call below to GetSecurityInfo succeeds, but GetLastError will return 122 if you check it.  Doesn't seem to
//    // make a difference though.
//    if (pGetSecurityInfo(processOrService, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &oldDacl, NULL, (PSECURITY_DESCRIPTOR *) &pSecurityDescriptor) == 0)
//      type = SE_KERNEL_OBJECT;
//    else
//      if (pGetSecurityInfo(processOrService, SE_SERVICE, DACL_SECURITY_INFORMATION, NULL, NULL, &oldDacl, NULL, (PSECURITY_DESCRIPTOR *) &pSecurityDescriptor) == 0)
//        type = SE_SERVICE;
//      else
//        type = SE_UNKNOWN_OBJECT_TYPE;
//
//    if (type != SE_UNKNOWN_OBJECT_TYPE)
//    {
//      int count = 1;
//      if (AllocateAndInitializeSid((PSID_IDENTIFIER_AUTHORITY) &CEveryoneSia, 1, 0, 0, 0, 0, 0, 0, 0, 0, &pSid1))
//      {
//        ZeroMemory(&ea, sizeof(ea));
//        ea[0].grfAccessPermissions = access;
//        ea[0].grfAccessMode = GRANT_ACCESS;
//        ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
//        ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
//        ea[0].Trustee.ptstrName = (LPWSTR) pSid1;
//        count = 1;
//        if ((IsMetro()) && (AllocateAndInitializeSid((PSID_IDENTIFIER_AUTHORITY) &CSecurityAppPackageAuthority, 2, 2, 1, 0, 0, 0, 0, 0, 0, &pSid2)))
//        {
//          // this adds access for metro "AppContainer" apps
//          ea[1].grfAccessPermissions = access;
//          ea[1].grfAccessMode = GRANT_ACCESS;
//          ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
//          ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
//          ea[1].Trustee.ptstrName = (LPWSTR) pSid2;
//          count++;
//        }
//        if (pSetEntriesInACL(count, ea, oldDacl, &newDacl) == 0)
//        {
//          result = (pSetSecurityInfo(processOrService, type, DACL_SECURITY_INFORMATION, NULL, NULL, newDacl, NULL) == 0);
//          LocalFree(newDacl);
//        }
//        FreeSid(pSid1);
//        if (pSid2 != NULL)
//          FreeSid(pSid2);
//      }
//    }
//    LocalFree(pSecurityDescriptor);
//  }
//  FreeLibrary(dll);
//  return result;
//}
//
//// -------------------------------------------------------------------------------------------------



//
//static bool CheckLpcMessageType(USHORT dwMessageType)
//{
//  if (dwMessageType == LPC_CONNECTION_REQUEST)
//    return true;
//
//  if (dwMessageType == LPC_PORT_CLOSED)
//    return false;  // no message since this is a sporadic normal event
//
//  #ifdef _DEBUG
//    for (int i = 0; i < (int) _countof(LpcMessageTypes); i++)
//    {
//      PLPC_MESSAGE_TYPE pMessageType = &LpcMessageTypes[i];
//      if (pMessageType->dwMessageType == dwMessageType)
//      {
//        Log(L"*** LPC message type other than LPC_CONNECTION_REQUEST received: (%d) %s\n", dwMessageType, pMessageType->szDescription);
//        return false;
//      }
//    }
//
//    // message type other than 1-10 found.  Only 1-10 are valid, so this is corrupted data
//    Log(L"*** LPC message type other than LPC_CONNECTION_REQUEST received, and type is invalid (not 1-10): %d\n", dwMessageType);
//  #endif
//
//  return false;
//}
//
//#ifdef _DEBUG
//  static bool CheckNtFunction(LPCSTR szFunctionName, DWORD dwReturnCode)
//#else
//  static bool CheckNtFunction(LPCSTR, DWORD dwReturnCode)
//#endif
//{
//  ASSERT(szFunctionName != NULL);
//
//  if (dwReturnCode != 0)
//  {
//    #ifdef _DEBUG
//      char buffer[64];
//      DecryptStr(szFunctionName, buffer, 64);
//
//      for (int i = 0; i < (int) _countof(LpcErrorDefinitions); i++)
//      {
//        PLPC_ERROR_INFO pError = &LpcErrorDefinitions[i];
//        if (pError->dwErrorCode == dwReturnCode)
//        {
//          Log(L"*** %S failed with return code %X.  Error is \"%s\" (%s)\n", buffer, dwReturnCode, pError->szDescription, pError->szSymbolName);
//          return false;
//        }
//      }
//      // not found
//      Log(L"*** %S failed with return code %X. No additional information available. See http://www.freepascal.org/svn/fpc/trunk/packages/extra/winunits/jwantstatus.pas for other error descriptions.\n", buffer, dwReturnCode);
//    #endif
//
//    return false;
//  }
//  else
//    return true;
//}
//
//static VOID InitLpcName(LPCSTR ipc, UNICODE_STRING *uniString)
//{
//  char buffer[32];
//  SString s(DecryptStr(CRpcControlIpc, buffer, 32));
//  s += ipc;
//
//  LPWSTR ws = (LPWSTR) LocalAlloc(LPTR, s.Length() * 2 + 2);
//  wcscpy(ws, s.GetBuffer());
//  uniString->Buffer = ws;
//  uniString->Length = (WORD) s.Length() * 2;
//  uniString->MaximumLength = (WORD) s.Length() * 2 + 2;
//}
//
//static VOID InitLpcFuncs(void)
//{
//  if (pNtCreatePort == NULL)
//  {
//    pNtCreatePort = (PFN_NTCREATEPORT) NtProc(CNtCreatePort);
//    pNtConnectPort = (PFN_NTCONNECTPORT) NtProc(CNtConnectPort);
//    pNtReplyWaitReceivePort = (PFN_NTREPLYWAITRECEIVEPORT) NtProc(CNtReplyWaitReceivePort);
//    pNtReplyWaitReceivePortEx = (PFN_NTREPLYWAITRECEIVEPORTEX) NtProc(CNtReplyWaitReceivePortEx);
//    pNtAcceptConnectPort = (PFN_NTACCEPTCONNECTPORT) NtProc(CNtAcceptConnectPort);
//    pNtCompleteConnectPort = (PFN_NTCOMPLETECONNECTPORT) NtProc(CNtCompleteConnectPort);
//  }
//}
//
//// ----------------------------------------
//// CreateIpcQueue and supporting functions
//// ----------------------------------------
//
//int WINAPI LpcWorkerThread(LPC_WORKER_THREAD *workerThread)
//{
//  void *msgTemp;
//  while (true)
//  {
//    if ( (workerThread->ShuttingDown) ||
//         (WaitForSingleObject(workerThread->Event, INFINITE) != WAIT_OBJECT_0) ||
//         (workerThread->ShuttingDown) ||
//         (workerThread->Message == NULL) )
//      break;
//
//    LPC_MESSAGE_PRIVATE *Private = (LPC_MESSAGE_PRIVATE*) ((ULONG_PTR) workerThread->Message + workerThread->Message->PrivateOffset);
//    workerThread->Message->Callback(workerThread->Message->Name, Private->Data, Private->MessageLength,
//                                    workerThread->Message->Answer.Buffer, workerThread->Message->Answer.Length, workerThread->Message->Context);
//
//    if (workerThread->Message->Answer.Length != 0)
//    {
//      // notify SendIpcMessage that the answer is ready
//      SetEvent(workerThread->Message->Answer.Event2);
//      CloseIpcAnswer(&(workerThread->Message->Answer));
//    }
//
//    workerThread->LastActive = GetTickCount();
//    msgTemp = workerThread->Message;
//    workerThread->Message = NULL;
//
//    LocalFree(msgTemp);
//    ReleaseSemaphore(workerThread->ParentSemaphore, 1, NULL);
//  }
//
//  EnterCriticalSection(&LpcSection);
//  if (workerThread->Freed)
//    *workerThread->Freed = true;
//  CloseHandle(workerThread->Event);
//  CloseHandle(workerThread->Handle);
//  LocalFree(workerThread);
//  LeaveCriticalSection(&LpcSection);
//
//  return 0;
//}
//
//static int WINAPI LpcDispatchThread(LPC_QUEUE *lpcQueue)
//{
//  LPC_MESSAGE *pMsg = NULL;
//  DWORD threadId = 0;
//
//  while (true)
//  {
//    WaitForSingleObject(lpcQueue->Semaphore, INFINITE);
//    if (lpcQueue->Shutdown)
//      break;
//
//    EnterCriticalSection(&lpcQueue->CriticalSection);
//    __try
//    {
//      // get the current message and move the pointer to the next one in line
//      pMsg = lpcQueue->Message;
//      if (pMsg != NULL)
//        lpcQueue->Message = lpcQueue->Message->Next;
//    }
//    __finally
//    {
//       LeaveCriticalSection(&lpcQueue->CriticalSection);
//    }
//
//    while (pMsg != NULL)
//    {
//      // loop until we find someone who is willing to handle this message
//      for (int i = 0; i < lpcQueue->WorkerThreads->GetCount(); i++)
//      {
//        if (lpcQueue->WorkerThreads->operator [](i)->Message == NULL)
//        {
//          lpcQueue->WorkerThreads->operator [](i)->Message = pMsg;
//          SetEvent(lpcQueue->WorkerThreads->operator [](i)->Event);
//          pMsg = NULL;
//          break;
//        }
//      }
//      if (pMsg != NULL)
//      {
//        // there is a message to handle, but all worker threads are busy
//        if (lpcQueue->MaxThreadCount > lpcQueue->WorkerThreads->GetCount())
//        {
//          // create a new worker thread
//          LPC_WORKER_THREAD *newThread = (LPC_WORKER_THREAD *) LocalAlloc(LPTR, sizeof(LPC_WORKER_THREAD));
//          lpcQueue->WorkerThreads->Add(newThread);
//          newThread->ParentSemaphore = lpcQueue->Semaphore;
//          newThread->Event = CreateEvent(NULL, FALSE, true, NULL);
//          newThread->Message = pMsg;
//          newThread->Handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) &LpcWorkerThread, newThread, 0, &threadId);
//          if (newThread->Handle != 0)
//          {
//            // thread successfully created
//            SetThreadPriority(newThread->Handle, THREAD_PRIORITY_NORMAL);
//            pMsg = NULL;
//          }
//          else
//          {
//            // couldn't allocate new thread, so free up allocated resources.  Message
//            // will have to wait for an available thread
//            CloseHandle(newThread->Event);
//            LocalFree(newThread);
//            lpcQueue->WorkerThreads->Remove(newThread);
//          }
//        }
//
//        if (pMsg != NULL)
//        {
//          // message hasn't been handled yet, so wait for an available worker thread
//          WaitForSingleObject(lpcQueue->Semaphore, INFINITE);
//          if (lpcQueue->Shutdown)
//            break;
//        }
//      }
//    }
//    if (lpcQueue->Shutdown)
//      break;
//
//
//    // now prune any worker threads that have been sitting idle
//    int workerThreadCount = lpcQueue->WorkerThreads->GetCount();
//    for (int i = workerThreadCount - 1; i >= 0; i--)
//    {
//      if (((i > 0) || (workerThreadCount > 1)) && (lpcQueue->WorkerThreads->operator [](i)->Message == NULL) &&
//         (GetTickCount() - lpcQueue->WorkerThreads->operator [](i)->LastActive > 100))
//      {
//        // this thread was idle for more than 100 ms, so close it down
//        Log(L"Dispatch thread: Removing idle thread from worker pool\n");
//
//        lpcQueue->WorkerThreads->operator [](i)->ShuttingDown = true;
//        SetEvent(lpcQueue->WorkerThreads->operator [](i)->Event);
//        workerThreadCount--;
//        lpcQueue->WorkerThreads->RemoveAt(i);
//      }
//    }
//  }
//  return 0;
//}

//// ----------------------------------------
//// DestroyIpcQueue and supporting functions
//// ----------------------------------------
//
//static bool DestroyLpcQueue(LPCSTR ipcName)
//{
//  HANDLE hPort;
//  SECURITY_QUALITY_OF_SERVICE qualityOfService;
//  DWORD dwBufferLength;
//  UNICODE_STRING unicodeIpcName;
//  P_LPC_QUEUE pLpcQueue = NULL;
//  int i;
//  bool result = false;
//
//  if (LpcReady)
//  {
//    EnterCriticalSection(&LpcSection);
//    __try
//    {
//      pLpcQueue = NULL;
//      int lpcCount = LpcList.GetCount();
//
//      // see if the name is present in the list of LPC connections
//      for (i = 0; i < lpcCount; i++)
//      {
//        if (_stricmp(LpcList[i]->Name, ipcName) == 0)
//        {
//          pLpcQueue = LpcList[i];
//          LpcList.RemoveAt(i);
//          break;
//        }
//      }
//    }
//    __finally
//    {
//      LeaveCriticalSection(&LpcSection);
//    }
//
//    if (pLpcQueue != NULL)
//    {
//      // found a match.  Destroy it and remove from the LPC list
//      pLpcQueue->Shutdown = true;
//      ReleaseSemaphore(pLpcQueue->Semaphore, 1, NULL);
//      InitLpcName(pLpcQueue->Name, &unicodeIpcName);
//      ZeroMemory(&qualityOfService, sizeof(qualityOfService));
//      if (GetMadCHookOption(DISABLE_IPC_FIX_1))
//      {
//        hPort = 0;
//        dwBufferLength = 0;
//        CheckNtFunction(CNtConnectPort, pNtConnectPort(&hPort, &unicodeIpcName, &qualityOfService, NULL, NULL, NULL, NULL, &dwBufferLength));
//      }
//      LocalFree(unicodeIpcName.Buffer);
//      if (WaitForSingleObject(pLpcQueue->PortThread, 1000) == WAIT_TIMEOUT)
//        TerminateThread(pLpcQueue->PortThread, 0);
//      CloseHandle(pLpcQueue->PortThread);
//
//      if (WaitForSingleObject(pLpcQueue->DispatchThread, 1000) == WAIT_TIMEOUT)
//        TerminateThread(pLpcQueue->DispatchThread, 0);
//      CloseHandle(pLpcQueue->DispatchThread);
//
//      if (pLpcQueue->WorkerThreads != NULL)
//      {
//        for (i = 0; i < pLpcQueue->WorkerThreads->GetCount(); i++)
//        {
//          if ((*pLpcQueue->WorkerThreads)[i] != NULL)
//          {
//            LPC_WORKER_THREAD *workerThread = (*pLpcQueue->WorkerThreads)[i];
//            BOOL freed = false;
//            workerThread->Freed = &freed;
//            workerThread->ShuttingDown = true;
//            HANDLE workerTh = NULL;
//            if (DuplicateHandle(GetCurrentProcess(), workerThread->Handle, GetCurrentProcess(), &workerTh, 0, FALSE, DUPLICATE_SAME_ACCESS))
//            {
//              SetEvent(workerThread->Event);
//              WaitForSingleObject(workerTh, 1000);
//              CloseHandle(workerTh);
//            }
//            else
//            {
//              SetEvent(workerThread->Event);
//              WaitForSingleObject(workerThread->Handle, 1000);
//            }
//            if (!freed)
//            {
//              EnterCriticalSection(&LpcSection);
//              if (!freed)
//              {
//                TerminateThread(workerThread->Handle, 0);
//                CloseHandle(workerThread->Event);
//                CloseHandle(workerThread->Handle);
//                if (workerThread->Message != NULL)
//                  LocalFree(workerThread->Message);
//                LocalFree(workerThread);
//              }
//              LeaveCriticalSection(&LpcSection);
//            }
//          }
//        }
//        delete pLpcQueue->WorkerThreads;
//        CloseHandle(pLpcQueue->Port);
//        CloseHandle(pLpcQueue->Semaphore);
//        DeleteCriticalSection(&pLpcQueue->CriticalSection);
//        LocalFree(pLpcQueue->Name);
//        LocalFree(pLpcQueue);
//        result = true;
//      }
//    }
//  }
//
//  return result;
//}
//
//// ----------------------------------------
//// SendIpcMessage and supporting functions
//// ----------------------------------------
//
//#define TokenBnoIsolation_ (TOKEN_INFORMATION_CLASS) 0x2c
//
//typedef struct _TOKEN_BNO_ISOLATION_INFORMATION_MCH {
//    PWSTR       IsolationPrefix;
//    BOOLEAN     IsolationEnabled;
//} TOKEN_BNO_ISOLATION_INFORMATION_MCH, *PTOKEN_BNO_ISOLATION_INFORMATION_MCH;
//
//static PFN_NT_QUERY_INFORMATION_TOKEN pfnNtQueryInformationToken = NULL;
//
//bool GetMetroPath(DWORD processId, LPSTR path)
//{
//  bool result = false;
//  PFN_GET_APP_CONTAINER_NAMED_OBJECT_PATH gacnop = (PFN_GET_APP_CONTAINER_NAMED_OBJECT_PATH) GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetAppContainerNamedObjectPath");
//  if (gacnop)
//  {
//    HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION, false, processId);
//    if (ph)
//    {
//      HANDLE th;
//      if (OpenProcessToken(ph, TOKEN_QUERY, &th))
//      {
//        WCHAR arrCh[MAX_PATH + 1];
//        DWORD len;
//        if (gacnop(th, NULL, MAX_PATH, arrCh, &len))
//        {
//          // Metro apps run under a special private namespace.
//          // Fortunately there's on official API which gives us the necessary information.
//          result = true;
//          WideToAnsi(arrCh, path);
//        }
//        else
//          if (!GetMadCHookOption(DISABLE_IPC_FIX_2))
//          {
//            #ifdef _WIN64
//              ULONG len = 0x120;
//            #else
//              ULONG len = 0x118;
//            #endif
//            if (!pfnNtQueryInformationToken)
//              pfnNtQueryInformationToken = (PFN_NT_QUERY_INFORMATION_TOKEN) NtProc(CNtQueryInformationToken);
//            if (pfnNtQueryInformationToken)
//            {
//              PTOKEN_BNO_ISOLATION_INFORMATION_MCH buf = (PTOKEN_BNO_ISOLATION_INFORMATION_MCH) LocalAlloc(LPTR, len);
//              if ((pfnNtQueryInformationToken(th, TokenBnoIsolation_, buf, len, &len) == 0) && (buf->IsolationEnabled))
//              {
//                // RuntimeBroker.exe in Windows 10 runs under private object namespace.
//                // It's all rather weird. After some disassembling I found "TokenBnoSolution".
//                // It's in the latest "winnt.h", but otherwise completely undocumented.
//                // It gives us the private namespace prefix of the target process.
//                result = true;
//                WideToAnsi(buf->IsolationPrefix, path);
//              }
//              LocalFree(buf);
//            }
//          }
//        CloseHandle(th);
//      }
//      CloseHandle(ph);
//    }
//  }
//  return result;
//}
//
//static bool InitIpcAnswer(bool create, LPCSTR name, DWORD counter, DWORD processId, IPC_ANSWER *answer, DWORD session)
//{
//  bool result = false;
//
//  ASSERT(name != NULL);
//  ASSERT(name[0] != L'\0');
//
//  if ((name == NULL) || (name[0] == L'\0'))
//    return false;
//
//  if (answer->Length > 0)
//  {
//    char answerBufName[16];
//    DecryptStr(CAnswerBuf, answerBufName, 16);
//    char nameBuf[350];
//
//    if (processId != 0)
//      sprintf_s(nameBuf, 350, "%s%s%d$%x", name, answerBufName, counter, processId);
//    else
//      sprintf_s(nameBuf, 350, "%s%s%d", name, answerBufName, counter);
//    int nameBufLen = lstrlenA(nameBuf);
//
//    if (create)
//    {
//      Log(L"InitIpcAnswer: Creating Answer file mapping.  name=%S\n", nameBuf);
//
//      DecryptStr(CMap, &nameBuf[nameBufLen], 8);
//      answer->Map = InternalCreateFileMapping(nameBuf, answer->Length, true, false);
//      DecryptStr(CEvent, &nameBuf[nameBufLen], 8);
//      lstrcatA(nameBuf, "1");
//      answer->Event1 = CreateGlobalEvent(nameBuf, FALSE, FALSE);
//      DecryptStr(CEvent, &nameBuf[nameBufLen], 8);
//      lstrcatA(nameBuf, "2");
//      answer->Event2 = CreateGlobalEvent(nameBuf, FALSE, FALSE);
//    }
//    else
//    {
//      Log(L"InitIpcAnswer: Opening existing answer file mapping.  name=%S\n", nameBuf);
//      char sessionName[16];
//      char sessionPath[MAX_PATH];
//      DecryptStr(CSession, sessionName, 16);
//      sprintf_s(sessionPath, MAX_PATH, "%s%d\\", sessionName, session);
//      DecryptStr(CMap, &nameBuf[nameBufLen], 8);
//      answer->Map = OpenGlobalFileMapping(nameBuf, true);
//      if (answer->Map == NULL)
//      {
//        // if the IPC sender doesn't have the SeCreateGlobalPrivilege in w2k3
//        // or in xp sp2 or higher then the objects were not created in global
//        // namespace but in the namespace of the sender
//        // so if opening of the file map didn't work, we try to open the
//        // objects in the session of the sender
//        if (processId != 0)
//          sprintf_s(nameBuf, 350, "%s%s%s%d$%x", sessionPath, name, answerBufName, counter, processId);
//        else
//          sprintf_s(nameBuf, 350, "%s%s%s%d", sessionPath, name, answerBufName, counter);
//        nameBufLen = lstrlenA(nameBuf);
//        DecryptStr(CMap, &nameBuf[nameBufLen], 8);
//        answer->Map = OpenGlobalFileMapping(nameBuf, true);
//        if ((answer->Map == NULL) && (IsMetro()))
//        {
//          char metroPath[MAX_PATH];
//          if (GetMetroPath(processId, metroPath))
//          {
//            sprintf_s(sessionPath, MAX_PATH, "%s%d\\%s\\", sessionName, session, metroPath);
//            sprintf_s(nameBuf, 350, "%s%s%s%d$%x", sessionPath, name, answerBufName, counter, processId);
//            nameBufLen = lstrlenA(nameBuf);
//            DecryptStr(CMap, &nameBuf[nameBufLen], 8);
//            answer->Map = OpenGlobalFileMapping(nameBuf, true);
//          }
//        }
//        // restore global names
//        if (processId != 0)
//          sprintf_s(nameBuf, 350, "%s%s%d$%x", name, answerBufName, counter, processId);
//        else
//          sprintf_s(nameBuf, 350, "%s%s%d", name, answerBufName, counter);
//        nameBufLen = lstrlenA(nameBuf);
//      }
//      DecryptStr(CEvent, &nameBuf[nameBufLen], 8);
//      lstrcatA(nameBuf, "1");
//      answer->Event1 = OpenGlobalEvent(nameBuf);
//      DecryptStr(CEvent, &nameBuf[nameBufLen], 8);
//      lstrcatA(nameBuf, "2");
//      answer->Event2 = OpenGlobalEvent(nameBuf);
//      if (answer->Event1 == NULL)
//      {
//        if (processId != 0)
//          sprintf_s(nameBuf, 350, "%s%s%s%d$%x", sessionPath, name, answerBufName, counter, processId);
//        else
//          sprintf_s(nameBuf, 350, "%s%s%s%d", sessionPath, name, answerBufName, counter);
//        nameBufLen = lstrlenA(nameBuf);
//        DecryptStr(CEvent, &nameBuf[nameBufLen], 8);
//        lstrcatA(nameBuf, "1");
//        answer->Event1 = OpenGlobalEvent(nameBuf);
//        DecryptStr(CEvent, &nameBuf[nameBufLen], 8);
//        lstrcatA(nameBuf, "2");
//        answer->Event2 = OpenGlobalEvent(nameBuf);
//      }
//    }
//
//    if (answer->Map != NULL)
//      answer->Buffer = MapViewOfFile(answer->Map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
//
//    result = (answer->Event1 != NULL) && (answer->Event2 != NULL) && (answer->Buffer != NULL);
//    if (result)
//    {
//      if (create)
//        ZeroMemory((void *) answer->Buffer, answer->Length);
//    }
//    else
//      CloseIpcAnswer(answer);
//  }
//  else
//  {
//    answer->Map = NULL;
//    answer->Buffer = NULL;
//    answer->Event1 = NULL;
//    answer->Event2 = NULL;
//    result = true;
//  }
//
//  return result;
//}
//
//static void CloseIpcAnswer(IPC_ANSWER *answer)
//{
//  if (answer->Buffer != NULL)
//  {
//    if (!UnmapViewOfFile(answer->Buffer))
//    {
//      Log(L"CloseIpcAnswer: UnmapViewOfFile failed: %d\n", GetLastError());
//    }
//  }
//
//  if (answer->Map != NULL)
//  {
//    //printf("maphandle=%d\n", answer->Map);
//    if (!CloseHandle(answer->Map))
//    {
//      Log(L"CloseIpcAnswer: CloseHandle(Map) failed: %d\n", GetLastError());
//    }
//    answer->Map = NULL;
//  }
//
//  if (answer->Event1 != NULL)
//  {
//    if (!CloseHandle(answer->Event1))
//    {
//      Log(L"CloseIpcAnswer: CloseHandle(Event1) failed: %d\n", GetLastError());
//    }
//    answer->Event1 = NULL;
//  }
//
//  if (answer->Event2 != NULL)
//  {
//    if (!CloseHandle(answer->Event2))
//    {
//      Log(L"CloseIpcAnswer: CloseHandle(Event2) failed: %d\n", GetLastError());
//    }
//    answer->Event2 = NULL;
//  }
//}
//
//static bool WaitFor(HANDLE handle1, HANDLE handle2, DWORD timeout, BOOL handleMessages)
//{
//  HANDLE handleArray[2];
//  bool result = false;
//  MSG msg;
//
//  handleArray[0] = handle1;
//  handleArray[1] = handle2;
//
//  if (handleMessages)
//  {
//    bool breakLoop = false;
//    while (!breakLoop)
//    {
//      switch (MsgWaitForMultipleObjects(2, handleArray, FALSE, timeout, QS_ALLINPUT))
//      {
//        case WAIT_OBJECT_0:
//        {
//          result = true;
//          breakLoop = true;
//          break;
//        }
//
//        case WAIT_OBJECT_0 + 2:
//        {
//          while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
//          {
//            TranslateMessage(&msg);
//            DispatchMessage(&msg);
//          }
//          break;
//        }
//
//        case WAIT_TIMEOUT:
//        {
//          breakLoop = true;
//          break;
//        }
//
//        default:
//        {
//          breakLoop = true;
//          break;
//        }
//      }
//    }
//    return result;
//  }
//  else
//  {
//    return WaitForMultipleObjects(2, handleArray, FALSE, timeout) == WAIT_OBJECT_0;
//  }
//}
//
//#ifdef _WIN64
//  extern "C" int _InterlockedIncrementEx(int *value);
//
//  static int InterlockedIncrementEx(int *value)
//  {
//    return _InterlockedIncrementEx(value);
//  }
//#else
//  static int InterlockedIncrementEx(int *value)
//  {
//    __asm
//    {
//      mov edx, value
//      mov eax, [edx]
//     Again:
//      mov ecx, eax
//      inc ecx
//      lock cmpxchg [edx], ecx
//      jnz Again
//     }
//  }
//#endif
//
//static DWORD GetLpcCounter(void)
//{
//  if (LpcCounterBuf == NULL)
//  {
//    SString mutexName, fileMapName;
//    char buffer1[16], buffer2[16], buffer3[8];
//    mutexName.Format(L"%S%S$%x%S", DecryptStr(CIpc, buffer1, 16), DecryptStr(CCounter, buffer2, 16), GetCurrentProcessId(), DecryptStr(CMutex, buffer3, 8));
//    HANDLE mutex = CreateMutexW(NULL, false, mutexName.GetBuffer());
//    if (mutex)
//    {
//      WaitForSingleObject(mutex, INFINITE);
//      fileMapName.Format(L"%S%S$%x", DecryptStr(CIpc, buffer1, 16), DecryptStr(CCounter, buffer2, 16), GetCurrentProcessId());
//      HANDLE map = CreateFileMappingW((HANDLE) -1, NULL, PAGE_READWRITE, 0, 4, fileMapName.GetBuffer());
//      if (map)
//      {
//        BOOL newMap = (GetLastError() != ERROR_ALREADY_EXISTS);
//        LpcCounterBuf = (int *) MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
//        if ((newMap) && (LpcCounterBuf))
//          *LpcCounterBuf = 0;
//        CloseHandle(map);
//      }
//      ReleaseMutex(mutex);
//      CloseHandle(mutex);
//    }
//  }
//
//  if (LpcCounterBuf != NULL)
//    return (DWORD) InterlockedIncrementEx(LpcCounterBuf);
//  else
//    return 0;
//}

