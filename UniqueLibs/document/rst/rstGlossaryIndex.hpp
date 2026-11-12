#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace hc::rst {

/// `.. glossary::` 블록에서 뽑아낸 용어 하나.
struct GlossaryEntry
{
    QString                             term;         ///< 표시용 용어 (정렬 키 제거 후)
    QString                             definition;   ///< 정의 본문. 들여쓰기를 벗긴 여러 줄
    QString                             path;         ///< 정의가 있는 파일 절대경로
    int                                 line = 1;     ///< 용어 줄 (1-based)
};

/// 문서 하나의 텍스트에서 용어집 항목을 뽑는다.
///
/// reStructuredText 용어집은 `.. glossary::` directive 안의 **정의 목록**이다.
///
///     .. glossary::
///
///        docutils
///           reStructuredText 를 처리하는 파이썬 라이브러리.
///
///        role
///        롤
///           인라인 마크업을 만드는 이름 있는 해석기.
///
/// 용어 줄이 연속으로 여러 개면 모두 같은 정의를 가리키는 동의어다.
/// `용어 : 정렬키` 형식의 정렬 키는 표시에서 뗀다.
[[nodiscard]] QVector< GlossaryEntry > parseGlossary( const QString& text, const QString& path = {} );

/// 프로젝트 하나의 용어집. 문서를 훑어 만든 결과를 담고 조회를 제공한다.
///
/// 수집은 디스크 I/O 라 작업 스레드에서 하고, 결과만 GUI 스레드로 되돌린다.
/// (프로젝트 개요 스캔과 같은 패턴 — solRestWorkspaceController 참조.)
class GlossaryIndex final : public QObject
{
    Q_OBJECT

public:
    explicit GlossaryIndex( QObject* parent = nullptr );

    /// 이 프로젝트의 문서들을 훑어 용어집을 다시 만든다.
    /// force=false 면 같은 프로젝트에 대해 다시 돌지 않는다.
    void                                refresh( const QString& projectId, const QString& sourceRoot,
                                                 const QString& rootDoc, bool force );
    /// 활성 프로젝트를 바꾼다. 조회는 이 프로젝트의 결과만 본다.
    void                                setActiveProjectId( const QString& projectId );
    [[nodiscard]] QString               activeProjectId() const { return activeProjectId_; }

    /// 용어를 찾는다. 없으면 nullptr. 대소문자를 구분하지 않는다.
    [[nodiscard]] const GlossaryEntry*  lookup( const QString& term ) const;
    /// 접두가 일치하는 용어들 (자동완성 후보용). 빈 접두면 전부.
    [[nodiscard]] QVector< GlossaryEntry > match( const QString& prefix, int limit = 200 ) const;
    [[nodiscard]] int                   count() const { return static_cast< int >( entries_.size() ); }

signals:
    /// 수집이 끝났다. 자동완성 팝업이 떠 있으면 후보를 다시 채우는 데 쓴다.
    void                                ready( const QString& projectId, int count );

private:
    void                                apply( const QString& projectId, QVector< GlossaryEntry > entries,
                                               quint64 generation );

    QString                             activeProjectId_;
    QString                             indexedProjectId_;
    QVector< GlossaryEntry >            entries_;
    /// 소문자 용어 -> entries_ 인덱스
    QHash< QString, int >               byLowerTerm_;
    /// 늦게 도착한 이전 프로젝트의 결과를 버리기 위한 세대 번호.
    quint64                             generation_ = 0;
};

}  // namespace hc::rst
