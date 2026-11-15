#include "stdafx.h"
#include "environmentNotification.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <Windows.h>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcEnvironmentNotification, "hc.system.environmentNotification")
namespace hc::system {
quint32 NotifyEnvironmentChanged()
{
    // Microsoft WM_SETTINGCHANGE contract: broadcast Environment with wParam 0.
    // https://learn.microsoft.com/windows/win32/winmsg/wm-settingchange
    SetLastError(ERROR_SUCCESS); DWORD_PTR Result = 0;
    if (SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
        SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 250, &Result)) return ERROR_SUCCESS;
    const auto Error = GetLastError();
    return Error ? Error : ERROR_TIMEOUT;
}
}