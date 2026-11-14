#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QString>
#include <QVector>

/// 자동완성 후보 고르기 규칙(reST 편집기 · SQLite 쿼리 편집기 공용). QtCore 만 쓴다 — 후보를 만드는 화면 없는 코드
/// (reST 경로 후보, SQLite 문맥 분류)도 같은 점수 규칙을 쓴다.
namespace hc::document::completion
{
    /// 부분 일치(subsequence) 검사와 점수 매기기(reST 의 규칙, rstcomplete::fuzzyMatchCompletion 이 이리로 옮겨 왔다).
    ///
    /// 연속 일치 · 단어 경계 · 접두 일치에 가산점을 주고 건너뛴 글자에 감점한다.
    /// MatchedPositions 는 강조 표시용 Candidate 기준 문자 인덱스.
    [[nodiscard]] bool FuzzyMatch( const QString& Pattern, const QString& Candidate, int* Score = nullptr,
                                   QVector< int >* MatchedPositions = nullptr );

    /// 접두어, 그다음 대문자 머리 글자(SQLite 설계 5.2: mi → Milliseconds · MediaTypeId 의 M…I).
    ///
    /// 머리 글자는 대문자(앞이 대문자가 아닐 때), 숫자 묶음의 첫 자리, _ · - · . · 공백 뒤 글자다. 글자를 머리 글자에
    /// 먼저 붙이고, 없으면 바로 앞에 붙인 글자의 다음 글자에 붙인다(mid → MediaTypeId 의 M · I · d). 흩어진 아무
    /// 글자에는 붙이지 않는다 — 이름이 많은 스키마에서 엉뚱한 후보가 끼지 않게.
    /// 점수: 접두어 > 머리 글자. 같은 규칙 안에서는 짧은 이름이 앞선다(팝업이 길이로 한 번 더 가른다).
    [[nodiscard]] bool InitialsMatch( const QString& Pattern, const QString& Candidate, int* Score = nullptr,
                                      QVector< int >* MatchedPositions = nullptr );

    /// LSP CompletionItemKind 밖의 우리 전용 종류. 표준은 1~26 을 쓴다.
    ///
    /// 이미지 파일만 따로 나누는 이유는 상세 패널에 프리뷰가 뜨는 행을 목록에서 바로 알아볼 수 있게 하기 위해서다.
    inline constexpr int KindImageFile = 1001;
    /// 치환 참조 후보(reST `|name|`). 목록에서 `|` 배지로 바로 알아볼 수 있게 따로 나눈다.
    inline constexpr int KindSubstitution = 1002;
}
