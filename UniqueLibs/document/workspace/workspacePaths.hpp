#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QString>
#include <QStringList>

namespace hc::document::workspace
{
    /// 절대 경로 · cleanPath · '/' 구분 · 드라이브 문자 대문자 · 끝 '/' 없음(드라이브 루트는 "D:/", UNC 공유 루트는
    /// "//server/share"). 문자열만 다루며 디스크를 읽지 않는다(상대 경로는 현재 폴더 기준).
    [[nodiscard]] QString NormalizePath( const QString& Path );
    /// 워크스페이스 루트의 정규형. NormalizePath 와 같다(이름으로 뜻을 드러낸다).
    [[nodiscard]] QString NormalizeRoot( const QString& Path );
    /// 비교 · 해시 키. NormalizePath 를 Windows 에서 소문자로 바꾼 것.
    [[nodiscard]] QString PathKey( const QString& Path );
    [[nodiscard]] bool SamePath( const QString& Left, const QString& Right );

    /// Path 가 Root 이거나 그 아래인가. Windows 에서 대소문자를 가리지 않는다.
    /// 문자열로 먼저 보고, AllowDiskAccess 면 둘 다 있을 때 정규 경로(canonicalFilePath: 접합 · 링크 풀림)로 한 번 더
    /// 본다(MRE solWorkspaceSession.cpp:133-151 isPathInWorkspace). 네트워크 경로에서 디스크 확인은 느릴 수 있다.
    [[nodiscard]] bool IsInside( const QString& Path, const QString& Root, bool AllowDiskAccess = true );

    /// Root 기준 상대 경로('/' 구분). Path 가 Root 면 ".", 밖이면 빈 문자열. 디스크를 읽지 않는다.
    [[nodiscard]] QString RelativeTo( const QString& Root, const QString& Path );
    /// 상태 파일에 적을 형태. 루트 안이면 상대 경로, 밖이면 정규화한 절대 경로와 Outside = true(설계 6.4).
    [[nodiscard]] QString ToStored( const QString& Root, const QString& Path, bool* Outside = nullptr );
    /// 상태 파일에서 읽은 경로를 절대 경로로. 상대 경로는 **지금 루트** 기준이라 폴더를 옮겨도 따라간다.
    [[nodiscard]] QString FromStored( const QString& Root, const QString& Stored );

    /// Folder 부터 드라이브 루트(UNC 는 공유 루트)까지, 가까운 것부터. 정규화된 경로. 디스크를 읽지 않는다.
    [[nodiscard]] QStringList AncestorFolders( const QString& Folder );
}
