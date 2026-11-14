#pragma once

/// 고유 라이브러리
#include "sqliteSchema.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <utility>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace hc::sqlite
{
    // 쿼리 편집기 자동 완성(설계 5.2, 시안 ‘부품 · 자동 완성’). 화면 없이 현재 문의 글과 커서 위치만 보고 문맥을 가르고
    // 후보를 만든다. 팝업 · 키 처리는 QueryTab, 고르기 · 밑줄은 공용 팝업(UniqueLibs/document/completion)이 맡는다.
    enum class CompletionKind { Keyword, Table, View, Cte, Column, PrimaryKey, ForeignKey, Function, Pragma, Collation, JoinCondition };

    enum class CompletionContext
    {
        None,           // 문자열 · 주석 · 숫자 안, 별칭을 쓰는 자리
        Statement,      // 문 처음(SELECT · INSERT · …)
        Tables,         // FROM · JOIN · INTO · UPDATE · TABLE 뒤
        Columns,        // 별칭. · 테이블. 뒤, INSERT INTO t ( 안
        Expression,     // SELECT 목록 · WHERE · SET · … (범위의 열 + 함수 + 키워드)
        JoinCondition,  // JOIN … ON 뒤(외래 키 조건을 첫 줄에)
        Clause,         // FROM 의 테이블 이름 뒤(AS · JOIN · WHERE …)
        Pragma,         // PRAGMA 뒤
        Collation,      // COLLATE 뒤
    };

    struct CompletionCandidate
    {
        QString label_, insertText_;
        QString filterText_;        // 비면 label_ 로 거른다(첨부 데이터베이스의 db.table 은 table 로)
        QString detail_;            // 오른쪽 형식 열(INTEGER · 347행 · CTE · 이 문)
        QString description_;       // 아래 설명 줄(tracks.Milliseconds · INTEGER NOT NULL)
        CompletionKind kind_ = CompletionKind::Keyword;
        int bias_ = 0;              // 같은 일치 점수에서 앞세울 정도
    };

    // 후보를 만들 자료. 문서 창이 스키마를 다시 읽을 때 채운다.
    struct CompletionSource
    {
        SchemaInfo schema_;
        QHash<QString, qint64> rowCounts_;  // "database.table"(소문자) → 행 수
        QStringList functions_, pragmas_, collations_;
        bool upperKeywords_ = true;         // 키워드를 대문자로 넣는다
        bool joinFirst_ = true;             // JOIN … ON 뒤 외래 키 조건을 첫 줄에
    };

    struct CompletionResult
    {
        CompletionContext context_ = CompletionContext::None;
        QString prefix_;                    // 커서 앞에 이미 친 이름 조각(바꿀 범위, UTF-16 글자 수 = prefix_.size())
        QList<CompletionCandidate> candidates_;
    };

    // Statement 는 현재 문 하나(커서가 든 문), Caret 은 그 안의 UTF-16 위치. Explicit 은 Ctrl+Space.
    CompletionResult Complete(const QString& Statement, int Caret, const CompletionSource& Source, bool Explicit);

    // 콜팁(설계 5.2 ‘함수 매개변수 도움말’). 커서가 든 함수 호출과 몇 번째 인자인지.
    struct CallInfo
    {
        QString name_;
        int argument_ = 0;
        int open_ = -1;                     // ( 의 위치(UTF-16). 없으면 -1
    };
    CallInfo EnclosingCall(const QString& Statement, int Caret);
    // "substr(X, Y [, Z])\n…설명" — 모르는 함수는 빈 문자열.
    QString FunctionSignature(const QString& Name);
    // 서명 첫 줄에서 Argument 번째 인자의 [시작, 끝). 강조용. 없으면 {-1, -1}.
    std::pair<int, int> SignatureArgument(const QString& Signature, int Argument);

    // 오류 빠른 수정(설계 5.4, Alt+Enter): ‘no such column / table’ 의 이름과 편집 거리가 가까운 이름.
    struct MissingName
    {
        enum class Kind { None, Column, Table };
        Kind kind_ = Kind::None;
        QString name_;              // 메시지의 이름(한정어를 뗀 것)
    };
    MissingName ParseMissingName(const QString& Error);
    // 편집 거리(대소문자 무시)가 가까운 순. 거리는 이름 길이의 1/3(적어도 2)까지.
    QStringList Suggestions(const QString& Wrong, const QStringList& Names, int Count = 6);
    int EditDistance(const QString& Left, const QString& Right);
    // 오류 자리에서 쓸 이름 목록: 열이면 그 문의 테이블 열(없으면 모든 열), 테이블이면 테이블 · 뷰 · CTE.
    QStringList NamesFor(const MissingName& Missing, const QString& Statement, const CompletionSource& Source);

    // SQL 서식(Alt+Shift+F): 큰 절은 새 줄, WHERE 의 AND · OR 와 긴 SELECT 목록은 들여 쓴 새 줄, 키워드 대소문자.
    // 문자열 · 주석 · 따옴표 이름은 그대로 둔다.
    QString FormatSql(const QString& Text, bool UpperKeywords);
}
