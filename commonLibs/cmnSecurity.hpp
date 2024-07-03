#pragma once

namespace nsCmn
{
    namespace nsSecurity
    {
        const SID_IDENTIFIER_AUTHORITY CEveryoneSia                 = {0, 0, 0, 0, 0,  1};
        const SID_IDENTIFIER_AUTHORITY CSecurityAppPackageAuthority = {0, 0, 0, 0, 0, 15};

        bool                            IsUserElevatedAdmin();

    }
}