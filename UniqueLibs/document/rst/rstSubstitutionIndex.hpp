#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace hc::rst {

/// 이 치환 정의를 지금 문서에서 쓸 수 있는가 — 를 가르는 출처.
///
/// docutils 의 치환은 **문서 단위**다. 다른 문서에서 정의한 `|x|` 는 그 문서를
/// `.. include::` 하지 않는 한 여기서 쓸 수 없다. 그래도 후보에서 빼지 않는 것은
/// 실사용 문서가 공용 조각 파일 하나를 모든 문서에 include 하는 형태를 흔히
/// 쓰기 때문이다. 대신 순서를 뒤로 미루고 상세 패널에 정의한 파일을 밝힌다.
enum class SubstitutionOrigin
{
    Document,   ///< 지금 편집 중인 문서. 확실히 쓸 수 있다
    Conf,       ///< conf.py 의 rst_prolog / rst_epilog. 모든 문서에 붙는다
    Builtin,    ///< Sphinx 가 언제나 넣어 주는 |version| |release| |today|
    Project,    ///< 프로젝트의 다른 문서. include 하지 않았다면 못 쓴다
};

/// `.. |name| replace:: ...` 한 벌.
struct SubstitutionEntry
{
    QString                             name;        ///< `|` 사이의 이름 (공백 정규화)
    QString                             directive;   ///< replace / image / unicode / date / raw
    QString                             argument;    ///< directive 인자. replace 면 치환될 글
    QString                             body;        ///< 들여쓴 본문. `raw:: html` 의 `<br/>`
    QString                             path;        ///< 정의가 있는 파일 절대경로
    int                                 line = 1;    ///< 1-based
    SubstitutionOrigin                  origin = SubstitutionOrigin::Project;
};

/// 자동완성 목록 한 행에 들어갈 한 줄 요약 ("raw:: html").
[[nodiscard]] QString substitutionSummary( const SubstitutionEntry& entry );
/// 상세 패널 본문. 인자와 들여쓴 본문을 모두 담는다.
[[nodiscard]] QString substitutionDetail( const SubstitutionEntry& entry );

/// 문서 하나의 텍스트에서 치환 정의를 뽑는다.
///
/// lineOffset 은 text 가 파일의 일부일 때 쓴다 (conf.py 의 rst_prolog 문자열).
/// entry.line 은 lineOffset + 문자열 안에서의 줄 번호가 된다.
[[nodiscard]] QVector< SubstitutionEntry > parseSubstitutions(
    const QString& text, const QString& path = {},
    SubstitutionOrigin origin = SubstitutionOrigin::Project, int lineOffset = 0 );

/// 파이썬 소스에서 `name = "..."` 의 **문자열 값**을 꺼낸다.
///
/// 삼중따옴표·raw 접두(`r"""`)를 다룬다. 표현식이나 이어 붙이기(`a + b`),
/// 다른 변수 대입은 다루지 않는다 — 값을 알려면 파이썬을 돌려야 하고,
/// 자동완성 후보 하나 때문에 그럴 수는 없다.
///
/// firstContentLine 은 문자열 **내용의 첫 줄**이 파일의 몇 번째 줄인지 (1-based).
/// 찾지 못하면 0 을 넣는다.
[[nodiscard]] QString pythonStringAssignment( const QString& source, const QString& variable,
                                              int* firstContentLine = nullptr );

/// conf.py 의 `rst_prolog` / `rst_epilog` 안에 있는 치환 정의.
///
/// Sphinx 는 이 두 문자열을 **모든 원본 문서의 앞뒤에 붙인다**. 그래서 여기서
/// 정의한 치환은 프로젝트 어느 문서에서나 쓸 수 있다 — 실사용 문서가 `|br|`
/// 같은 조각을 두는 가장 흔한 자리다.
[[nodiscard]] QVector< SubstitutionEntry > parseConfSubstitutions( const QString& confText,
                                                                   const QString& confPath );

/// Sphinx 가 모든 문서에 넣어 주는 세 개. conf.py 를 넘기면 값도 채운다.
[[nodiscard]] QVector< SubstitutionEntry > builtinSubstitutions( const QString& confText = {},
                                                                 const QString& confPath = {} );

/// 프로젝트 하나의 치환 목록. 문서와 conf.py 를 훑어 만든 결과를 담는다.
///
/// 수집은 디스크 I/O 라 작업 스레드에서 하고 결과만 GUI 스레드로 되돌린다
/// (용어집 인덱스와 같은 패턴 — solGlossaryIndex 참조).
class SubstitutionIndex final : public QObject
{
    Q_OBJECT

public:
    explicit SubstitutionIndex( QObject* parent = nullptr );

    /// 이 프로젝트의 conf.py 와 문서들을 훑어 치환 목록을 다시 만든다.
    /// force=false 면 같은 프로젝트에 대해 다시 돌지 않는다.
    void                                refresh( const QString& projectId, const QString& sourceRoot,
                                                 const QString& rootDoc, const QString& confPath,
                                                 bool force );
    void                                setActiveProjectId( const QString& projectId );
    [[nodiscard]] QString               activeProjectId() const { return activeProjectId_; }

    /// 모아 둔 정의 전부. Conf → Builtin → Project 순, 그 안에서는 이름순이다.
    [[nodiscard]] const QVector< SubstitutionEntry >& entries() const { return entries_; }
    /// 이름으로 찾는다. 없으면 nullptr. 대소문자를 구분하지 않는다.
    [[nodiscard]] const SubstitutionEntry* lookup( const QString& name ) const;
    [[nodiscard]] int                   count() const { return static_cast< int >( entries_.size() ); }

signals:
    /// 수집이 끝났다. 자동완성 팝업이 떠 있으면 후보를 다시 채우는 데 쓴다.
    void                                ready( const QString& projectId, int count );

private:
    void                                apply( const QString& projectId,
                                               QVector< SubstitutionEntry > entries,
                                               quint64 generation );

    QString                             activeProjectId_;
    QString                             indexedProjectId_;
    QVector< SubstitutionEntry >        entries_;
    /// 소문자 이름 -> entries_ 인덱스
    QHash< QString, int >               byLowerName_;
    /// 늦게 도착한 이전 프로젝트의 결과를 버리기 위한 세대 번호.
    quint64                             generation_ = 0;
};

}  // namespace hc::rst
