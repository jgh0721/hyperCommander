#ifndef CMNCONFIG_HPP
#define CMNCONFIG_HPP

/*!
    각 솔루션에서는 해당 파일을 프로젝트에 cmnConfig.hpp 라는 이름으로 복사하여 사용한다.
    공통 라이브러리 저장소에는 cmnConfig.hpp 라는 파일이 업로드 되지 않도록 주의한다.
*/

/*!
    공통 라이브러리에서 BOOST 지원을 활성화하려면 아래 주석을 해제합니다.
*/
#define USE_BOOST_SUPPORT

/*!
    공통 라이브러리에서 std::string 를 사용할 때 코드페이지를 UTF-8 로 가정합니다.
    윈도
        사용          std::string = UTF8
        사용 안 함    std::string = ANSI Text
    그외
        무조건 사용   std::string = UTF8
*/
#define STD_STRING_AS_UTF8

/*!
    QT 프레임워크 지원
*/
/// QtCore 모듈을 사용하는 Qt 지원을 활성화 합니다.
//#define USE_QT_SUPPORT
/// QtGui, QtWidgets 등 UI 모듈을 사용하는 Qt 지원을 활성화 합니다.
//#define USE_QT_UI_SUPPORT

/*!
    네트워크 관련
*/

/// 네트워크 지원 활성화 - 외부 라이브러리 ASIO 를 요구합니다.
#define USE_NETWORK_SUPPORT

/*!
    로그 시스템

    필수 지원 : 동시성(USE_CONCURRENT_SUPPORT)
*/

// 프로젝트에서 로깅 활성화
#define USE_LOGGING_SUPPORT

/*!
    편의 라이브러리 사용
*/

// NtDll 지원 활성화
#define USE_WIN32_NTDLL_SUPPORT
// NtDll 을 이용하여 NtQueryProcessInformation 을 사용합니다.
#define USE_WIN32_NTDLL_PROCESS_INFO_SUPPORT
#define USE_WIN32_SHELL_SUPPORT
#define USE_WIN32_OLE_SUPPORT
#define USE_OS_NATIVE_IMPLEMENTATION
#define USE_CONCURRENT_SUPPORT

#endif //CMNCONFIG_HPP
