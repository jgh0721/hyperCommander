#pragma once

/// 고유 라이브러리
#include "sqliteCellDelegate.hpp"
#include "sqliteConnection.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFont>
#include <QObject>
#include <QString>

class QWidget;

namespace hc::sqlite
{
    class GridView;

    // 설정 › SQLite › 편집기 · 실행 · 기록(설계 4.5 · 5절). 문서 창이 설정에서 읽어 작업 창에 넘긴다.
    struct QueryPreferences
    {
        bool schemaColors_ = true;          // 스키마 이름(테이블 · 열 · 함수)을 따로 칠한다
        bool statementBar_ = true;          // Ctrl+Enter 로 실행할 현재 문을 거터에 표시
        bool selectionFirst_ = true;        // Ctrl+Enter: 고른 글이 있으면 그것을 실행
        bool confirmUnbounded_ = true;      // WHERE 없는 UPDATE · DELETE 확인
        int rowLimit_ = 1000;               // 새 쿼리 탭의 행 제한(0 = 모두)
        // 자동 완성(편집기 탭).
        bool autoComplete_ = true;          // 입력 중 자동으로 제안
        int completeDelayMs_ = 150;
        bool upperKeywords_ = true;         // 키워드 대소문자: 대문자
        bool initialsMatch_ = true;         // 이름 일치: 접두어, 그다음 머리 글자(끄면 접두어만)
        bool joinFirst_ = true;             // JOIN … ON 뒤 외래 키 조건을 첫 줄로
        bool callTips_ = true;              // 함수 매개변수 도움말(콜팁)
        // 기록 · 쿼리 탭(실행 · 기록 탭).
        bool historyEnabled_ = true;        // 실행한 문 기록
        int historyLimit_ = 500;            // 파일마다 남길 개수
        bool restoreTabs_ = true;           // 파일을 다시 열면 쿼리 탭 복원
        // 쓰기 · 표 편집(실행 · 기록 탭, 결과 · 편집 탭).
        bool manualCommit_ = false;         // 새 쿼리 탭의 커밋 모드(기본 자동)
        bool showSqlBeforeCommit_ = false;  // Ctrl+S 전에 변경 SQL 보기
        bool confirmDelete_ = true;         // 행 삭제 전에 확인
        bool backupBeforeRebuild_ = true;   // 테이블 수정(재작성) 전에 백업 사본
        CellDisplay cells_;                 // 셀 표시(NULL 글 · 긴 글 · 형식 표시 · 숫자 맞춤)
        // 열기(열기 탭). 연결을 열 때 쓴다(열린 연결은 그대로).
        int busyTimeoutMs_ = 5000;
        bool foreignKeys_ = true;           // PRAGMA foreign_keys = ON
        bool loadExtension_ = false;        // sqlite3_enable_load_extension
        int countRows_ = 0;                 // 0: 256 MiB 이하 파일만 · 1: 언제나 · 2: 세지 않음
        // 암호화(SEE) 탭 · 빠른 보기(설계 4.5 · 8).
        KeyFormat seeFormat_ = KeyFormat::Text;  // 키 대화의 키 형식 기본값
        bool backupBeforeRekey_ = true;     // 키를 바꾸기 전에 백업 사본
        bool saveKeyDefault_ = false;       // 키 대화의 ‘키를 이 PC 에 저장’ 을 미리 켬
        bool trySavedKeys_ = true;          // 열 때 저장한 키를 먼저 시도
        bool previewSavedKeys_ = true;      // 빠른 보기: 저장한 키가 있으면 암호화 파일도 보여 줌
    };

    // 작업 창의 탭 하나(테이블 · 뷰 탭, 쿼리 탭). 위 칸(Top)과 아래 결과 칸(Result)을 작업 창의 두 영역에 따로 넣는다.
    class WorkTab : public QObject
    {
        Q_OBJECT
    public:
        using QObject::QObject;
        virtual QWidget* Top() const = 0;
        virtual QWidget* Result() const = 0;
        virtual QString Title() const = 0;
        virtual QString ToolTip() const { return Title(); }
        // 탭 아이콘(sqliteGlyphs 이름).
        virtual QString GlyphName() const = 0;
        virtual bool IsQuery() const { return false; }
        // 저장하지 않은 쿼리(.sql 파일 탭) · 커밋하지 않은 편집 표시.
        virtual bool Modified() const { return false; }
        // 커밋을 기다리는 것(표의 보류 편집 수, 쿼리 탭의 열린 트랜잭션은 1).
        virtual int PendingCount() const { return 0; }
        // 상태 표시줄: 행 수 · 현재 위치. 복사: 그리드에서 고른 칸.
        virtual QString RowsText() const = 0;
        virtual QString PositionText() const = 0;
        virtual QString SelectionText() const = 0;
        virtual GridView* CurrentGrid() const = 0;
        // 탭을 열거나 고를 때 키보드를 받을 곳.
        virtual QWidget* FocusTarget() const = 0;
        virtual void Cancel() {}
        virtual void ApplyFonts(const QFont& Grid, const QFont& Mono, bool MonoValues) = 0;
    signals:
        void Changed();
        // 새 결과 그리드가 생겼다(작업 창이 글꼴 · 클립보드 중개를 맞춘다).
        void GridAdded(GridView* Grid);
    };
}
