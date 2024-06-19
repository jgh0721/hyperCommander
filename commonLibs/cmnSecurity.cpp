#include "stdafx.h"
#include "cmnSecurity.hpp"

namespace nsCmn
{
    namespace nsSecurity
    {
        // http://www.rohitab.com/discuss/topic/40992-cc-uac-bypass/

        bool IsUserElevatedAdmin()
        {
            SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
            PSID SecurityIdentifier;
            if( !AllocateAndInitializeSid( &NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &SecurityIdentifier ) )
                return false;

            BOOL IsAdminMember;
            if( !CheckTokenMembership( nullptr, SecurityIdentifier, &IsAdminMember ) )
                IsAdminMember = FALSE;

            FreeSid( SecurityIdentifier );

            return IsAdminMember != FALSE;
        }
    }
}