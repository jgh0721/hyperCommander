#ifndef HDR_CMN_WIN_UTILS
#define HDR_CMN_WIN_UTILS

namespace nsCmn
{
    namespace nsWin
    {
        ///////////////////////////////////////////////////////////////////////
        /// Security

        // 프로세스의 특권 설정, 이전 EnablePrivilege 확장
        bool                                SetPrivilege( LPCTSTR lpszPrivilege, BOOL bEnablePrivilege );
        // 프로세스의 특권 확인,
        // 함수의 반환값은 함수가 값을 가져오는데 성공했는지 여부
        bool                                CheckPrivilege( LPCTSTR lpszPrivilege, BOOL* bIsAvailable );

    } // nsWin
} // nsCmn

#endif // HDR_CMN_WIN_UTILS