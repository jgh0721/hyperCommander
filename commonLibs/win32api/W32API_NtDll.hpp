#ifndef __HDR_CMNLIB_W32API_NTDLL__
#define __HDR_CMNLIB_W32API_NTDLL__

#include "cmnBase.hpp"
#include "concurrent/cmnSingleton.hpp"

#include "win/winLIBMgr.hpp"
#include "W32API_CmnDefs.hpp"
#include "W32API_Ntdll_Defs.hpp"

namespace nsW32API
{
    namespace nsNtDeclare
    {
        typedef struct tagLpcSectionInfo
        {
          DWORD Size;
          #ifdef _WIN64
            DWORD Dummy1;
          #endif
          HANDLE SectionHandle;
          DWORD Param1;
          #ifdef _WIN64
            DWORD Dummy2;
          #endif
          DWORD SectionSize;
          #ifdef _WIN64
            DWORD Dummy3;
          #endif
          void *ClientBaseAddress;
          void *ServerBaseAddress;
        } LPC_SECTION_INFO, *PLPC_SECTION_INFO;

        typedef struct tagLpcSectionMapInfo
        {
          DWORD Size;
          #ifdef _WIN64
            DWORD Dummy1;
          #endif
          DWORD SectionSize;
          #ifdef _WIN64
            DWORD Dummy2;
          #endif
          void *ServerBaseAddress;
        } LPC_SECTION_MAP_INFO, *PLPC_SECTION_MAP_INFO;

        #ifndef _WIN64
          typedef struct tagLpcSectionMapInfo64
          {
            DWORD Size;
            DWORD Dummy1;
            DWORD SectionSize;
            DWORD Dummy2;
            void *ServerBaseAddress;
            DWORD Dummy3;
          } LPC_SECTION_MAP_INFO64, *P_LPC_SECTION_MAP_INFO64;
        #endif

        typedef DWORD LPC_MESSAGE_DATA[60];

        typedef struct tagLpcMessagePrivate
        {
            DWORD ProcessId;
            DWORD MessageLength;
            DWORD Counter;
            DWORD Session;
            DWORD AnswerLength;
            LPC_MESSAGE_DATA Data;
        } LPC_MESSAGE_PRIVATE;

        typedef struct tagIpcAnswer
        {
            HANDLE Map;
            LPCVOID Buffer;
            DWORD Length;
            HANDLE Event1;
            HANDLE Event2;
        } IPC_ANSWER;

        typedef struct tagLpcMessage
        {
          struct tagLpcMessage *Next;
          char Name[MAX_PATH];
          PVOID Callback;   // TyFnIPCCallback
          LPVOID Context;
          IPC_ANSWER Answer;
          DWORD PrivateOffset;
          WORD ActualMessageLength;
          WORD TotalMessageLength;
          USHORT MessageType;
          USHORT DataInfoOffset;
          DWORD ClientProcessId;
          DWORD ClientThreadId;
          DWORD MessageId;
          DWORD SharedSectionSize;
        } LPC_MESSAGE, *PLPC_MESSAGE;

    } // nsNtDeclare

    DYNLOAD_BEGIN_CLASS( CW32APINtdllImpl, L"Ntdll" )
        DYNLOAD_FUNC_WITH_01( STATUS_NOT_IMPLEMENTED, NTSTATUS, NTAPI, RtlGetVersion, __inout PRTL_OSVERSIONINFOW, lpVersionInformation );

        /// LPC
        /*!
         * @brief
         * @param Porthandle
         * @param ObjectAttributes
         * @param MaxConnectInfoLength has to be less than 260.  0 is fine
         * @param MaxDataLengthhas to be less than 328.  0 is fine
         * @param MaxPoolUsage always specify 0
         */
        DYNLOAD_FUNC_WITH_05( STATUS_NOT_IMPLEMENTED, NTSTATUS, NTAPI, NtCreatePort, PHANDLE, PortHandle, nsNtDeclare::POBJECT_ATTRIBUTES, ObjectAttributes, ULONG_PTR, MaxConnectInfoLength, ULONG_PTR, MaxDataLength, ULONG, MaxPoolUsage );
        DYNLOAD_FUNC_WITH_08( STATUS_NOT_IMPLEMENTED, NTSTATUS, NTAPI, NtConnectPort, PHANDLE, PortHandle, nsNtDeclare::PUNICODE_STRING, PortName, PSECURITY_QUALITY_OF_SERVICE, SecurityQos, nsNtDeclare::PLPC_SECTION_INFO, ClientShared, nsNtDeclare::PLPC_SECTION_MAP_INFO, ServerShared, PULONG, MaximumMessageLength, PVOID, ConnectInfo, PULONG, ConnectInfoLength );
        DYNLOAD_FUNC_WITH_02( STATUS_NOT_IMPLEMENTED, NTSTATUS, NTAPI, NtListenPort, HANDLE, PortHandle, nsNtDeclare::PLPC_MESSAGE, ReplyMessage );
        DYNLOAD_FUNC_WITH_04( STATUS_NOT_IMPLEMENTED, NTSTATUS, NTAPI, NtReplyWaitReceivePort, HANDLE, PortHandle, LPVOID, PortContext, nsNtDeclare::PLPC_MESSAGE, ReplyMessage, nsNtDeclare::PLPC_MESSAGE, ReceiveMessage );
        DYNLOAD_FUNC_WITH_05( STATUS_NOT_IMPLEMENTED, NTSTATUS, NTAPI, NtReplyWaitReceivePortEx, HANDLE, PortHandle, LPVOID, PortContext, nsNtDeclare::PLPC_MESSAGE, ReplyMessage, nsNtDeclare::PLPC_MESSAGE, ReceiveMessage, PLARGE_INTEGER, TimeOut );
        DYNLOAD_FUNC_WITH_06( STATUS_NOT_IMPLEMENTED, NTSTATUS, NTAPI, NtAcceptConnectPort, PHANDLE, PortHandle, ULONG_PTR, Unknown, nsNtDeclare::LPC_MESSAGE, Message, BOOLEAN, Accept, nsNtDeclare::PLPC_SECTION_INFO, ServerView, nsNtDeclare::PLPC_SECTION_MAP_INFO, ClientView );
        DYNLOAD_FUNC_WITH_01( STATUS_NOT_IMPLEMENTED, NTSTATUS, NTAPI, NtCompleteConnectPort, HANDLE, PortHandle );

    DYNLOAD_END_CLASS();

} // nsW32API

#endif //__HDR_CMNLIB_W32API_NTDLL__
