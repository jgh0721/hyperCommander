#include "stdafx.h"
#include "rstGlossaryIndex.hpp"

/// 모듈
#include "rstOutlineService.hpp"
#include "rstLineUtils.hpp"
#include "text/RstStructure.h"
#include "rstBackgroundWork.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {

namespace {

/// 용어집을 뽑을 때 훑을 문서 수의 상한. 프로젝트 개요와 같은 규모로 잡는다.
constexpr int kMaxGlossaryDocuments = 2000;

// 줄 나누기·들여쓰기·빈 줄 판정은 공용 도우미가 한다. 예전에는 이 셋이 이 파일과
// 다른 인덱서에 바이트 단위로 같은 사본으로 있었다.
using hc::rst::rstline::indentWidth;
using hc::rst::rstline::isBlank;

/// `용어 : 정렬키` 에서 표시용 용어만 남긴다.
/// 이스케이프한 `\:` 는 콜론 그대로 둔다.
QString stripSortKey( QString term )
{
    for( qsizetype index = 0; index + 1 < term.size(); ++index )
    {
        if( term.at( index ) != QLatin1Char( ' ' ) || term.at( index + 1 ) != QLatin1Char( ':' ) )
            continue;
        if( index > 0 && term.at( index - 1 ) == QLatin1Char( '\\' ) )
            continue;
        term = term.left( index );
        break;
    }
    return term.replace( QStringLiteral( "\\:" ), QStringLiteral( ":" ) ).trimmed();
}

}  // namespace

QVector< GlossaryEntry > parseGlossary( const QString& text, const QString& path )
{
    // `.. glossary::` 판정은 공용 파서가 한다. 예전에는 정규식이 한 벌 더 있었고,
    // 그것만 directive 이름의 `+` 를 막는 식으로 다섯 벌이 조금씩 갈라져 있었다.
    const auto isGlossaryDirective = []( const QString& line ) {
        const QByteArray utf8 = line.toUtf8();
        const std::optional< syntax::DirectiveParts > parts = syntax::parseDirective(
            std::string_view( utf8.constData(), static_cast< std::size_t >( utf8.size() ) ) );
        if( !parts || parts->hasSubstitution() )
            return false;
        if( QByteArray::fromRawData( utf8.constData() + parts->nameStart,
                                     static_cast< qsizetype >( parts->nameEnd - parts->nameStart ) )
            != QByteArrayLiteral( "glossary" ) )
            return false;
        // 인자를 받지 않는 directive 다. 뒤에 무언가 붙어 있으면 다른 것이다.
        return QByteArray::fromRawData( utf8.constData() + parts->colonsEnd,
                                        utf8.size() - static_cast< qsizetype >( parts->colonsEnd ) )
            .trimmed()
            .isEmpty();
    };

    const QStringList lines = hc::rst::rstline::splitLines( text );

    QVector< GlossaryEntry > entries;
    for( qsizetype index = 0; index < lines.size(); ++index )
    {
        if( !isGlossaryDirective( lines.at( index ) ) )
            continue;

        const int directiveIndent = indentWidth( lines.at( index ) );

        // directive 본문: 이후로 나오는 "더 깊게 들여쓴" 줄들. 빈 줄은 본문에 속한다.
        qsizetype bodyEnd = index + 1;
        int       bodyIndent = -1;
        for( qsizetype scan = index + 1; scan < lines.size(); ++scan )
        {
            const QString& line = lines.at( scan );
            if( isBlank( line ) )
                continue;
            const int width = indentWidth( line );
            if( width <= directiveIndent )
                break;
            if( bodyIndent < 0 || width < bodyIndent )
                bodyIndent = width;
            bodyEnd = scan + 1;
        }
        if( bodyIndent < 0 )
        {
            index = bodyEnd - 1;
            continue;
        }

        // 본문 안의 정의 목록: bodyIndent 인 줄이 용어, 더 깊은 줄이 정의.
        // `:sorted:` 같은 directive 옵션은 용어가 아니다.
        qsizetype cursor = index + 1;
        while( cursor < bodyEnd )
        {
            if( isBlank( lines.at( cursor ) ) || indentWidth( lines.at( cursor ) ) != bodyIndent )
            {
                ++cursor;
                continue;
            }

            const QString head = lines.at( cursor ).trimmed();
            if( head.startsWith( QLatin1Char( ':' ) ) )
            {
                ++cursor;
                continue;
            }

            // 연속된 용어 줄 = 동의어
            QStringList terms;
            QVector< int > termLines;
            while( cursor < bodyEnd && !isBlank( lines.at( cursor ) )
                   && indentWidth( lines.at( cursor ) ) == bodyIndent )
            {
                const QString term = stripSortKey( lines.at( cursor ).trimmed() );
                if( !term.isEmpty() )
                {
                    terms << term;
                    termLines << static_cast< int >( cursor ) + 1;
                }
                ++cursor;
            }
            if( terms.isEmpty() )
                continue;

            // 정의: 다음에 나오는 더 깊게 들여쓴 블록
            QStringList definitionLines;
            int         definitionIndent = -1;
            while( cursor < bodyEnd )
            {
                const QString& line = lines.at( cursor );
                if( isBlank( line ) )
                {
                    if( !definitionLines.isEmpty() )
                        definitionLines << QString();
                    ++cursor;
                    continue;
                }
                const int width = indentWidth( line );
                if( width <= bodyIndent )
                    break;
                if( definitionIndent < 0 )
                    definitionIndent = width;
                definitionLines << line.mid( qMin< qsizetype >( definitionIndent, line.size() ) );
                ++cursor;
            }

            while( !definitionLines.isEmpty() && definitionLines.constLast().isEmpty() )
                definitionLines.removeLast();

            const QString definition = definitionLines.join( QLatin1Char( '\n' ) );
            for( qsizetype termIndex = 0; termIndex < terms.size(); ++termIndex )
            {
                GlossaryEntry entry;
                entry.term = terms.at( termIndex );
                entry.definition = definition;
                entry.path = path;
                entry.line = termLines.at( termIndex );
                entries.push_back( entry );
            }
        }

        index = bodyEnd - 1;
    }

    return entries;
}

GlossaryIndex::GlossaryIndex( QObject* parent )
    : QObject( parent )
{
}

void GlossaryIndex::setActiveProjectId( const QString& projectId )
{
    if( activeProjectId_ == projectId )
        return;

    activeProjectId_ = projectId;
    indexedProjectId_.clear();
    entries_.clear();
    byLowerTerm_.clear();
    ++generation_;   // 이전 워크스페이스/프로젝트의 늦은 스캔 결과를 버린다.
}

void GlossaryIndex::refresh( const QString& projectId, const QString& sourceRoot,
                             const QString& rootDoc, const bool force )
{
    if( projectId.isEmpty() || sourceRoot.isEmpty() )
        return;
    if( !force && indexedProjectId_ == projectId )
        return;

    indexedProjectId_ = projectId;
    const quint64 generation = ++generation_;
    QPointer< GlossaryIndex > guard( this );

    // 문서 수백 개를 읽는 일이라 GUI 스레드에서 하면 눈에 띄게 멈춘다.
    QThreadPool::globalInstance()->start( [guard, sourceRoot, rootDoc, projectId, generation] {
        const QStringList paths = collectProjectDocuments( sourceRoot, rootDoc, kMaxGlossaryDocuments );

        QVector< GlossaryEntry > entries;
        for( const QString& path : paths )
        {
            // 문서마다 전부 읽는다(상한 2000). 종료 중이면 멈춘다 — 끝까지 도는
            // 동안 프로세스가 살아 있는 것이 실제 문제였다. 결과는 어차피
            // generation 검사에서 버려진다.
            if( isShuttingDown() )
                return;

            QFile file( path );
            if( !file.open( QIODevice::ReadOnly | QIODevice::Text ) )
                continue;
            const QString text = rstline::fromUtf8File( file.readAll() );
            entries += parseGlossary( text, path );
        }

        QMetaObject::invokeMethod(
            guard,
            [guard, entries = std::move( entries ), projectId, generation]() mutable {
                if( guard )
                    guard->apply( projectId, std::move( entries ), generation );
            },
            Qt::QueuedConnection );
    } );
}

void GlossaryIndex::apply( const QString& projectId, QVector< GlossaryEntry > entries,
                           const quint64 generation )
{
    // 프로젝트가 바뀐 뒤 도착한 결과는 버린다.
    if( generation != generation_ )
        return;

    entries_ = std::move( entries );
    byLowerTerm_.clear();
    byLowerTerm_.reserve( static_cast< int >( entries_.size() ) );
    for( qsizetype index = 0; index < entries_.size(); ++index )
    {
        // 같은 용어가 여러 문서에 있으면 먼저 만난 것을 쓴다.
        byLowerTerm_.insert( entries_.at( index ).term.toCaseFolded(), static_cast< int >( index ) );
    }

    emit ready( projectId, static_cast< int >( entries_.size() ) );
}

const GlossaryEntry* GlossaryIndex::lookup( const QString& term ) const
{
    const auto it = byLowerTerm_.constFind( term.trimmed().toCaseFolded() );
    if( it == byLowerTerm_.constEnd() )
        return nullptr;
    return &entries_.at( *it );
}

QVector< GlossaryEntry > GlossaryIndex::match( const QString& prefix, const int limit ) const
{
    const QString folded = prefix.trimmed().toCaseFolded();

    QVector< GlossaryEntry > matched;
    for( const GlossaryEntry& entry : entries_ )
    {
        if( !folded.isEmpty() && !entry.term.toCaseFolded().startsWith( folded ) )
            continue;
        matched.push_back( entry );
        if( matched.size() >= limit )
            break;
    }
    return matched;
}

}  // namespace hc::rst
