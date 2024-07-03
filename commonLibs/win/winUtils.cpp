#include "stdafx.h"
#include "winUtils.hpp"

#include <windows.h>

DECLARE_CMNLIBSV2_NAMESPACE

namespace nsCmn
{
    namespace nsWin
    {
        ///////////////////////////////////////////////////////////////////////
        /// Security

        bool SetPrivilege( LPCTSTR lpszPrivilege, BOOL bEnablePrivilege )
        {
            TOKEN_PRIVILEGES tp;
            LUID luid;
            HANDLE hToken = NULL;
            bool isSuccess = false;

            do
            {
                if( OpenProcessToken( GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken ) == FALSE )
                    break;

                // lookup privilege on local system
                // privilege to lookup
                // receives LUID of privilege
                if( LookupPrivilegeValueW( NULL, lpszPrivilege, &luid ) == FALSE )
                    break;

                tp.PrivilegeCount = 1;
                tp.Privileges[ 0 ].Luid = luid;
                if( bEnablePrivilege )
                    tp.Privileges[ 0 ].Attributes = SE_PRIVILEGE_ENABLED;
                else
                    tp.Privileges[ 0 ].Attributes = 0;

                // Enable the privilege or disable all privileges.
                if( AdjustTokenPrivileges( hToken, FALSE, &tp, sizeof( TOKEN_PRIVILEGES ), ( PTOKEN_PRIVILEGES )NULL, ( PDWORD )NULL ) == FALSE )
                    break;

                if( GetLastError() == ERROR_NOT_ALL_ASSIGNED )
                    break;

                isSuccess = true;

            } while( false );

            if( hToken != NULL )
                CloseHandle( hToken );

            return isSuccess;
        }

        bool CheckPrivilege( LPCTSTR lpszPrivilege, BOOL* bIsAvailable )
        {
            if( bIsAvailable == nullptr )
                return false;

            HANDLE ProcessToken = NULL;
            PRIVILEGE_SET PrivilegeSet;
            if( LookupPrivilegeValueW( 0, lpszPrivilege, &PrivilegeSet.Privilege[ 0 ].Luid ) &&
                OpenProcessToken( GetCurrentProcess(), TOKEN_QUERY, &ProcessToken ) )
            {
                PrivilegeSet.PrivilegeCount = 1;
                PrivilegeSet.Control        = PRIVILEGE_SET_ALL_NECESSARY;
                PrivilegeSet.Privilege[ 0 ].Attributes = 0;
                const BOOL Ret = PrivilegeCheck( ProcessToken, &PrivilegeSet, bIsAvailable );
                CloseHandle( ProcessToken );
                return Ret != FALSE;
            }

            return false;
        }

    } // nsWin
} // nsCmn
