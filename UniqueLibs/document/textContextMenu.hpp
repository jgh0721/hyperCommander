#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QPoint>
#include <QString>
#include <functional>

class QMenu;
class QTextView;

namespace hc::document
{
    /// 텍스트 본문 우클릭 메뉴(시안 MODES.text/mdedit/epubedit ctx). 텍스트 · Markdown 소스 · EPUB 소스 편집기가 함께 쓴다.
    struct TextMenuOptions
    {
        // 맨 위 항목(예: '미리보기에서 이 위치 보기')과 그 뒤 구분선. 글자가 비면 넣지 않는다.
        QString                 leadingText_;
        std::function< void() > leading_;
        // 맨 위에 여러 항목을 직접 넣는다( reST 소스: 참조로 이동 · 진단 · 자동 완성 ). 구분선도 넣는 쪽이 붙인다.
        std::function< void( QMenu& ) > prepend_;
        bool                    deleteItem_ = true;          // '삭제'
        bool                    findInSelection_ = true;     // '선택 영역에서 찾기…'
        bool                    fold_ = true;                // '접기 ▸'
        bool                    readOnly_ = false;           // 편집 명령을 끈다(EPUB 소스의 읽기 전용 리소스 등)
        QString                 selectAllText_;              // 비면 '전체 선택'
        // 비면 QTextView 의 실행 취소 · 다시 실행을 쓴다(EPUB 편집기는 문서 단위 기록을 넘긴다).
        std::function< bool() > canUndo_, canRedo_;
        std::function< void() > undo_, redo_;
    };

    /// 누른 곳이 선택 밖이면 캐럿을 그곳으로 옮긴다(메뉴가 누른 대상을 설명하도록).
    void MoveCaretForContextMenu( QTextView* Text, const QPoint& Position );
    /// Menu 에 항목을 채운다. 단축키 글자는 현재 단축키 설정에서 읽는다.
    void FillTextContextMenu( QMenu& Menu, QTextView* Text, const TextMenuOptions& Options = {} );
}
