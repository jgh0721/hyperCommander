#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QList>
#include <QString>
#include <functional>

class QAction;
class QMenu;

namespace hc::document
{
    struct RecentDocument;

    /// 파일 메뉴 끝의 '최근 항목' 구획(계획 6.1). 머리 · 두 줄 행(번호 · 이름 · 설명) · 모두 보기 · 지우기.
    /// 행은 QWidgetAction 이다. 숫자 1–9 는 동작 글자의 단축 문자로, 위 · 아래 · Enter 는 QMenu 가 처리한다.
    struct RecentMenuHandlers
    {
        std::function< void( const RecentDocument& ) > open_;
        std::function< void() > showAll_;
        std::function< void() > clear_;
    };

    /// Menu 끝에 구획을 붙이고 붙인 동작을 돌려준다(다음 갱신 때 지운다). Count 가 0 이면 아무것도 붙이지 않는다.
    QList< QAction* > AppendRecentSection( QMenu* Menu, const QList< RecentDocument >& Entries, int Count, const RecentMenuHandlers& Handlers );
}
