#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace hc::rst::rstcomplete {

/// 커서 위치에서 무엇을 완성해야 하는가.
enum class ContextKind
{
    None,           ///< 완성할 것이 없다
    Directive,      ///< ".. " 뒤 — directive 이름
    Role,           ///< ":" 뒤 — role 이름
    DirectiveOption,///< directive 블록 안의 ":옵션:"
    RoleTarget,     ///< :ref:`...` 안 — 참조 대상
    Path,           ///< image/include 등의 경로 인자
    Substitution,   ///< `|` 뒤 — 치환 참조(`|name|`) 이름
};

/// Path 컨텍스트에서 경로가 문법의 어느 자리에 있는가.
///
/// 슬롯 표(rstpath)를 다시 찾을 수 있도록 detectContext 가 남겨 둔다. 표 자체를
/// 여기 두지 않는 것은 이 모듈이 파일 시스템도 Qt GUI 도 모르는 순수 판정기로
/// 남아야 하기 때문이다.
enum class PathSlotSite
{
    None,
    Argument,     ///< ".. image:: <여기>"
    Option,       ///< "   :file: <여기>"
    Body,         ///< toctree 본문의 각 줄
    RoleTarget,   ///< :download:`<여기>`
};

struct Context
{
    ContextKind                         kind = ContextKind::None;
    /// 이미 입력된 부분. **경로면 경로 전체**다 (예: "../img/lo").
    QString                             prefix;
    /// DirectiveOption/Path 면 소유 directive, RoleTarget 이면 롤 이름.
    QString                             directiveName;
    /// 옵션 값 자리일 때 그 옵션 이름 (":file:" 이면 "file").
    QString                             optionName;
    /// 완성 항목을 넣을 때 지워야 할 글자 수. prefix 길이와 같지만
    /// 컨텍스트에 따라 달라질 수 있어 따로 둔다.
    int                                 replaceLength = 0;

    PathSlotSite                        pathSite = PathSlotSite::None;

    /// 팝업의 퍼지 필터에 넘길 접두.
    ///
    /// 경로가 아니면 prefix 와 같다. 경로면 **마지막 구분자 뒤 조각**이다.
    /// 경로 후보의 라벨은 파일 이름뿐이라 "../img/lo" 전체로 거르면
    /// 부분수열 검사가 전부 실패해 목록이 통째로 사라진다.
    QString                             filterPrefix;

    /// ".. image::" 처럼 인자 앞 공백이 아직 없다. 삽입 문자열 앞에 공백을 붙인다.
    bool                                argumentNeedsSpace = false;

    /// Directive 컨텍스트가 **치환 정의**의 자리인가 (".. |logo| ").
    ///
    /// docutils 는 치환 정의 안에서 아무 directive 나 받지 않는다. 인라인으로
    /// 펼칠 수 있는 것만 된다 — replace / image / unicode / date / raw. 전체 표를
    /// 그대로 띄우면 목록 대부분이 그 자리에서 오류가 되는 후보다.
    bool                                substitutionDefinition = false;
};

/// 부분 일치(subsequence) 검사와 점수 매기기.
///
/// 연속 일치·단어 경계·접두 일치에 가산점을 주고 건너뛴 글자에 감점한다.
/// matchedPositions 는 강조 표시용 candidate 기준 문자 인덱스.
///
/// 팝업 위젯에 있던 것을 여기로 옮겼다. 경로 후보를 프로젝트 전역에서
/// 고를 때 core 쪽에서도 같은 점수 규칙이 필요한데, core 가 editor 를
/// include 할 수는 없기 때문이다.
[[nodiscard]] bool fuzzyMatchCompletion( const QString& pattern, const QString& candidate,
                                         int* score = nullptr,
                                         QVector< int >* matchedPositions = nullptr );

/// LSP CompletionItemKind 밖의 우리 전용 종류. 표준은 1~26 을 쓴다.
///
/// 이미지 파일만 따로 나누는 이유는 상세 패널에 프리뷰가 뜨는 행을 목록에서
/// 바로 알아볼 수 있게 하기 위해서다. 팝업 위젯과 후보 생성기 양쪽이 쓰므로
/// 둘 다 이미 아는 이 헤더에 둔다 (core 가 editor 를 include 하면 안 된다).
inline constexpr int kKindImageFile = 1001;

/// 치환 참조 후보. 목록에서 `|` 배지로 바로 알아볼 수 있게 따로 나눈다.
inline constexpr int kKindSubstitution = 1002;

struct Item
{
    QString                             label;
    QString                             insertText;
    QString                             detail;
    int                                 kind = 0;        ///< LSP CompletionItemKind
};

/// 커서 앞 텍스트를 보고 완성 컨텍스트를 판정한다.
///
/// lineText 는 현재 줄 전체, column 은 1-based 캐럿 열.
/// previousLines 는 directive 블록 안인지 판단하는 데 쓴다 (역순: 바로 앞 줄이 [0]).
/// 캐럿을 감싸는 블록의 시작까지 거슬러 올라가며 앞줄을 모은다.
///
/// 예전에는 호출부가 무조건 12줄로 잘랐다. reST 문법상 옵션은 directive 머리에
/// 붙으므로 대개 충분했지만 `toctree` 본문은 그렇지 않다 — 항목이 12개를 넘으면
/// 머리가 창 밖으로 밀려나 경로 자동완성이 조용히 멈췄다. 실사용 문서에서
/// 발현하던 결함이다.
///
/// directive 본문은 반드시 연속 들여쓰기이므로, 캐럿보다 얕게 들여쓴 비어 있지
/// 않은 줄을 만나면 거기가 블록 경계다. **그 줄까지** 담아 돌려준다 — 그것이
/// directive 인지는 enclosingDirective 가 본다.
///
/// lineAt 은 0-based 줄 번호로 본문을 돌려주고, 범위를 벗어나면 nullopt 를 낸다.
/// 문서 전체가 한 블록인 병리 사례를 위해 절대 상한을 둔다.
[[nodiscard]] QStringList collectPreviousLines(
    int caretLine, int caretIndent,
    const std::function< std::optional< QString >( int ) >& lineAt, int absoluteLimit = 2000 );

[[nodiscard]] Context detectContext( const QString& lineText, int column,
                                     const QStringList& previousLines = {} );

/// Esbonio 가 아직 응답하지 못할 때 쓰는 기본 후보.
///
/// Esbonio 2.x 는 내부 Sphinx 빌드가 끝날 때까지 completion 에 빈 결과를
/// 돌려준다. 그 수십 초 동안 타이핑을 쓸모 있게 만드는 것이 이 표의 목적이다.
[[nodiscard]] QVector< Item > candidatesFor( const Context& context );

/// 표에 들어 있는 directive / role 이름 (테스트와 메타데이터 초기화용).
[[nodiscard]] QStringList knownDirectives();
[[nodiscard]] QStringList knownRoles();

/// 치환 정의(".. |name| ...") 안에 쓸 수 있는 directive 이름.
///
/// docutils 는 이 자리에 **인라인으로 펼쳐지는** directive 만 허용한다
/// (docutils/parsers/rst/states.py 의 substitution_def 처리).
[[nodiscard]] QStringList substitutionDirectives();

// ── LSP 응답 다듬기 ────────────────────────────────────────
// Esbonio 가 돌려주는 것을 그대로 넣으면 문서가 깨진다. 아래 셋은 순수 함수라
// 위젯 없이 검증한다.

/// Esbonio 는 ".." 직후에 발화될 것을 전제로 " image::" 처럼 **앞에 공백이 붙은**
/// insertText 를 돌려준다. 사용자가 ".. " 까지 친 뒤 완성을 부르면 "..  image::"
/// 처럼 공백이 둘이 된다. directive 컨텍스트에서만 앞 공백을 떼어낸다.
[[nodiscard]] QVector< Item > normalizeLspItems( QVector< Item > items, const QString& lineText,
                                                 int column );

/// 팝업에 올리기 전 정리: 제어 문자 제거, 빈/중복 insertText 제거, 개수 제한.
[[nodiscard]] QVector< Item > finalizeItems( QVector< Item > items, int limit = 200 );

/// primary 를 앞에 두고, additional 중 insertText 가 겹치지 않는 것만 뒤에 붙인다.
/// LSP 결과 위에 오프라인 후보를 보충할 때 쓴다.
[[nodiscard]] QVector< Item > mergeItems( QVector< Item > primary, const QVector< Item >& additional );

}  // namespace hc::rst::rstcomplete
