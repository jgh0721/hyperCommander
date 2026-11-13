#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QString>

namespace hc::document::explorer
{
    /// 파일 · 폴더 이름으로 쓸 수 있는가. 쓸 수 없으면 Reason 에 사용자에게 보일 까닭(한국어)을 담는다.
    /// MRE MainWindow.cpp:4652-4670 의 isSafeEntryName 규칙(빈 이름, "." "..", 끝 공백 · 점, <>:"/\|?*,
    /// 제어 문자)에 Windows 예약 장치 이름(CON · NUL · COM1 …)을 더했다.
    [[nodiscard]] bool IsSafeEntryName( const QString& Name, QString* Reason = nullptr );

    /// Folder 에 Name 으로 새 항목을 만들거나(RenamingFrom 이 비었을 때) 이름을 바꿀 수 있는가.
    /// IsSafeEntryName 에 더해 같은 이름이 이미 있는지 본다. 이름 바꾸기에서 대소문자만 바꾸는 것은 허용한다.
    /// 문제가 없으면 빈 문자열.
    [[nodiscard]] QString EntryNameProblem( const QString& Folder, const QString& Name, const QString& RenamingFrom = {} );

    /// Folder 안에서 겹치지 않는 이름. Name 이 없으면 그대로, 있으면 "이름 (2).확장자" · "(3)" … 를 붙인다.
    [[nodiscard]] QString UniqueEntryName( const QString& Folder, const QString& Name );

    /// 새 파일의 기본 이름. Sphinx 프로젝트 안이면 "새 문서.rst", 밖이면 "새 문서.md"(설계 5.5). 겹치지 않게 만든다.
    [[nodiscard]] QString DefaultNewFileName( const QString& Folder, bool InsideSphinx );

    /// 새 폴더의 기본 이름. "새 폴더" 를 겹치지 않게 만든다.
    [[nodiscard]] QString DefaultNewFolderName( const QString& Folder );
}
