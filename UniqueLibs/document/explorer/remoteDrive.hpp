#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QString>

namespace hc::document::explorer
{
    /// Windows 드라이브 경로("Z:" · "Z:/..." )가 원격 매핑이고 지금 세션이 끊겨 있는가.
    /// 로컬 경로, 연결된 원격 드라이브, UNC 경로, 상태를 알 수 없는 공급자는 false 다.
    /// 파일 시스템 API 를 부르지 않으므로 끊긴 드라이브에서도 재연결을 기다리지 않는다.
    /// MRE src/uis/FileTreeFilterProxy.cpp:22-60 에서 옮겼다.
    [[nodiscard]] bool IsDisconnectedRemoteDrivePath( const QString& Path );

    /// "//server/share/..." 또는 "\\server\share\..." 인가( "\\?\UNC\..." 포함 ).
    [[nodiscard]] bool IsUncPath( const QString& Path );

    /// 경로의 드라이브 루트. "D:/" 또는 "//server/share/". 알 수 없으면 빈 문자열.
    /// 문자열만 보고 정한다(디스크를 읽지 않는다). 드라이브 문자는 대문자로 맞춘다.
    [[nodiscard]] QString DriveRootOf( const QString& Path );
}
