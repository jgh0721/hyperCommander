#pragma once

/// 모듈
#include "epubEditModel.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArray>
#include <QString>

namespace hc::document::epub
{
    /// 새 책 서식의 입력. 문자열은 앞뒤 공백을 지운 값으로 쓴다.
    struct NewBook
    {
        QString    title_;
        QString    creator_;
        QString    language_ = QStringLiteral( "ko" );
        QString    chapterTitle_;
        bool       stylesheet_ = true;
        QByteArray coverImage_;            // 비어 있으면 표지 없음
    };

    /// 새 책에서 처음 고를 장. 편집 모드 진입 뒤 이 리소스를 연다.
    inline constexpr auto NewBookFirstChapter = "OEBPS/Text/chapter-001.xhtml";

    /// BCP 47 형식 검사(책 정보 편집과 같은 식).
    bool ValidLanguageTag( const QString& Value );

    /// 책 정보 한 줄 값으로 쓸 수 있는가. 1024자 이하, 제어 문자 없음.
    bool ValidBookText( const QString& Value, bool Required );

    /// 최소 EPUB 3 패키지(container · OPF · 목차 · 첫 장 · 선택 스타일시트 · 선택 표지)를 만든다.
    /// mimetype 이 첫 항목인 무압축 ZIP 이며, EditModel 과 출판물 색인으로 다시 읽어 검사한 뒤 돌려준다.
    /// 실패하면 error_ 에 edit-new-* 코드를 담는다.
    EditOutput CreateBook( const NewBook& Book, const Cancellation& Cancel = {} );
}
