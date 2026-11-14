#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리
#include "text/ScintillaQtDirectBackend.h"

/// 외부 라이브러리
#include <Scintilla.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QElapsedTimer>

namespace hc::document
{
    /// 편집기 ↔ 미리보기 스크롤 동기화에서 편집기 쪽 규칙. Markdown · reST · EPUB 이 함께 쓴다.
    ///
    /// 미리보기 쪽 규칙( 어떤 스크롤이 사용자의 것인가 )은 tools/epub/web/previewScrollIntent.js 에 있다.
    /// 여기는 그 보고를 받은 편집기가 언제 따르고, 따른 뒤 무엇을 되돌려 보내지 않는가다.
    ///
    ///  - 따른다: 미리보기가 사용자의 스크롤로 판정한 보고만. 단 편집기가 위치를 보낸 직후( GuardMs )의
    ///    보고는 그 요청보다 먼저 떠난 것일 수 있다. 따르면 편집기가 방금 떠난 자리로 되돌아간다.
    ///  - 되울리지 않는다: 보고를 따라 편집기를 옮긴 직후( GuardMs )에는 편집기 위치를 보내지 않는다.
    ///    버리지는 않는다 — 가드가 풀리면 한 번 더 시도해야 큰 이동의 끝이 반영된다. 그때 보낼지는
    ///    따른 뒤 편집기가 놓인 자리( 행 단위로 반올림된다 )와 비교해 정한다. 그 자리를 기억하지 않으면
    ///    반올림된 위치가 되돌아가 사용자가 놓은 미리보기를 조금 밀어낸다.
    ///  - 끝은 끝으로: 미리보기가 맨 위 · 맨 끝에 붙었으면 편집기도 그 끝으로 간다( PlaceAtPreviewEdge ).
    ///    기준 줄의 높이를 맞추면 첫 문단이 여러 행으로 접히는 문서에서 편집기가 맨 위에 닿지 않는다.
    class PreviewScrollSync
    {
    public:
        static constexpr int GuardMs = 250;

        /// 편집기가 미리보기에 위치를 보냈다.
        void                            EditorSent() { sent_.start(); }
        /// 미리보기의 보고를 따라( 또는 미리보기를 눌러 ) 편집기를 옮겼다.
        void                            EditorFollowed() { followed_.start(); }
        /// User: 미리보기가 사용자의 스크롤로 판정한 보고인가.
        [[nodiscard]] bool              Follows( const bool User ) const { return User && !within( sent_ ); }
        /// 편집기 위치를 보내기까지 기다릴 시간( ms ). 0 이면 지금 보낸다. 아니면 그만큼 뒤에 다시 시도한다.
        [[nodiscard]] int               SendDelayMs() const { return within( followed_ ) ? int( GuardMs - followed_.elapsed() ) : 0; }
        /// 새 페이지 · 새 문서: 이전 가드는 의미가 없다.
        void                            Reset() { sent_.invalidate(); followed_.invalidate(); }

        /// 미리보기가 맨 위( Top ) · 맨 끝( Bottom )에 붙어 있으면 편집기를 그 끝으로 옮기고 true.
        static bool                     PlaceAtPreviewEdge( ScintillaQtDirectBackend* Backend, const bool Top, const bool Bottom )
        {
            if( Backend == nullptr || ( !Top && !Bottom ) )
                return false;
            // 맨 끝: 마지막 줄의 표시 행. Scintilla 가 끝을 넘는 값을 마지막 화면으로 잘라 준다.
            Backend->Send( SCI_SETFIRSTVISIBLELINE,
                           Top ? 0 : quintptr( Backend->Send( SCI_VISIBLEFROMDOCLINE, quintptr( Backend->lineCount() ) ) ) );
            return true;
        }

    private:
        [[nodiscard]] static bool       within( const QElapsedTimer& Timer ) { return Timer.isValid() && Timer.elapsed() < GuardMs; }

        QElapsedTimer                   sent_;
        QElapsedTimer                   followed_;
    };
}
