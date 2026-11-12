#include "stdafx.h"
#include "rstSubstitutionIndex.hpp"

/// 모듈
#include "rstLineUtils.hpp"
#include "text/RstStructure.h"
#include "rstOutlineService.hpp"
#include "rstBackgroundWork.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {

namespace {

/// 치환을 뽑을 때 훑을 문서 수의 상한. 프로젝트 개요·용어집과 같은 규모로 잡는다.
constexpr int kMaxDocuments = 2000;

// 줄 나누기·들여쓰기·빈 줄 판정은 공용 도우미가 한다. 예전에는 이 셋이 이 파일과
// 다른 인덱서에 바이트 단위로 같은 사본으로 있었다.
using hc::rst::rstline::indentWidth;
using hc::rst::rstline::isBlank;

using hc::rst::rstline::splitLines;

/// 파이썬 문자열의 이스케이프를 푼다 (raw 문자열이 아닐 때만 부른다).
///
/// ⚠ `\n` 이 들어 있으면 줄 수가 늘어 이후 줄 번호가 원본과 어긋난다. 그 값은
///   상세 패널의 출처 줄에만 쓰이고, rst_prolog 를 한 줄 문자열에 이스케이프로
///   적는 conf.py 는 드물어서 그대로 둔다.
QString unescapePython( const QString& text )
{
    QString out;
    out.reserve( text.size() );
    for( qsizetype index = 0; index < text.size(); ++index )
    {
        const QChar ch = text.at( index );
        if( ch != QLatin1Char( '\\' ) || index + 1 >= text.size() )
        {
            out += ch;
            continue;
        }

        const QChar next = text.at( ++index );
        switch( next.unicode() )
        {
            case 'n':  out += QLatin1Char( '\n' ); break;
            case 't':  out += QLatin1Char( '\t' ); break;
            case 'r':  out += QLatin1Char( '\r' ); break;
            case '\\': out += QLatin1Char( '\\' ); break;
            case '\'': out += QLatin1Char( '\'' ); break;
            case '"':  out += QLatin1Char( '"' ); break;
            case '\n': break;   // 줄 이음
            default:   out += QLatin1Char( '\\' ); out += next; break;
        }
    }
    return out;
}

bool isStringPrefix( const QChar ch )
{
    static const QString prefixes = QStringLiteral( "rRuUbBfF" );
    return prefixes.contains( ch );
}

/// 후보 순서. conf.py 는 어디서나 쓸 수 있고 다른 문서의 정의는 include 하지
/// 않았다면 못 쓴다 — 그 확신의 차이를 그대로 순서로 옮긴다.
int originRank( const SubstitutionOrigin origin )
{
    switch( origin )
    {
        case SubstitutionOrigin::Document: return 0;
        case SubstitutionOrigin::Conf:     return 1;
        case SubstitutionOrigin::Builtin:  return 2;
        case SubstitutionOrigin::Project:  return 3;
    }
    return 3;
}

}  // namespace

QString substitutionSummary( const SubstitutionEntry& entry )
{
    if( entry.directive.isEmpty() )
        return entry.argument.simplified();

    QString       text = entry.directive + QStringLiteral( "::" );
    const QString tail = entry.argument.isEmpty() ? entry.body : entry.argument;
    if( !tail.isEmpty() )
        text += QLatin1Char( ' ' ) + tail.simplified();
    return text;
}

QString substitutionDetail( const SubstitutionEntry& entry )
{
    QStringList parts;
    if( entry.directive.isEmpty() )
    {
        if( !entry.argument.isEmpty() )
            parts << entry.argument;
    }
    else
    {
        QString head = entry.directive + QStringLiteral( "::" );
        if( !entry.argument.isEmpty() )
            head += QLatin1Char( ' ' ) + entry.argument;
        parts << head;
    }

    if( !entry.body.isEmpty() )
        parts << entry.body;
    return parts.join( QLatin1Char( '\n' ) );
}

QVector< SubstitutionEntry > parseSubstitutions( const QString& text, const QString& path,
                                                 const SubstitutionOrigin origin,
                                                 const int lineOffset )
{
    // `.. |이름| directive:: 인자`
    //
    // 판정은 공용 파서가 한다. 예전에는 여기에 정규식이 한 벌 더 있었고, 다섯 벌
    // 중 이것만 directive 이름의 `+` 를 허용하는 식으로 조금씩 갈라져 있었다.
    // 이름에는 `|` 만 못 들어간다 (공백도 한글도 된다).
    struct Definition
    {
        QString name;
        QString directive;
        QString argument;
    };
    const auto parseDefinition = []( const QString& line ) -> std::optional< Definition > {
        const QByteArray                          utf8 = line.toUtf8();
        const std::string_view                    view( utf8.constData(),
                                    static_cast< std::size_t >( utf8.size() ) );
        const std::optional< syntax::DirectiveParts > parts = syntax::parseDirective( view );
        if( !parts || !parts->hasSubstitution() )
            return std::nullopt;

        Definition definition;
        // `|이름|` 에서 막대를 뗀다.
        definition.name = QString::fromUtf8( utf8.constData() + parts->subStart + 1,
                                             static_cast< qsizetype >( parts->subEnd - parts->subStart ) - 2 );
        definition.directive =
            QString::fromUtf8( utf8.constData() + parts->nameStart,
                               static_cast< qsizetype >( parts->nameEnd - parts->nameStart ) );
        definition.argument =
            QString::fromUtf8( utf8.constData() + parts->colonsEnd,
                               utf8.size() - static_cast< qsizetype >( parts->colonsEnd ) );
        return definition;
    };

    // directive 옵션(`:format: html`)은 본문이 아니다. 판정은 공용 파서에 맡긴다 —
    // 그쪽은 닫는 콜론 뒤에 공백이나 줄끝을 요구하므로, 본문 첫머리에 놓인
    // `:ref:`대상`` 을 옵션으로 오인하지 않는다. 예전 정규식은 오인하였다.
    const auto isOptionLine = []( const QString& line ) {
        const QByteArray utf8 = line.toUtf8();
        return syntax::parseField(
                   std::string_view( utf8.constData(), static_cast< std::size_t >( utf8.size() ) ) )
            .has_value();
    };

    const QStringList lines = splitLines( text );

    QVector< SubstitutionEntry > entries;
    for( qsizetype index = 0; index < lines.size(); ++index )
    {
        // 값싼 사전 검사. 문서의 거의 모든 줄에는 `|` 가 없다.
        if( !lines.at( index ).contains( QLatin1Char( '|' ) ) )
            continue;

        const std::optional< Definition > definition = parseDefinition( lines.at( index ) );
        if( !definition )
            continue;

        SubstitutionEntry entry;
        // docutils 는 치환 이름의 공백을 정규화해서 대조한다.
        entry.name = definition->name.simplified();
        if( entry.name.isEmpty() )
            continue;

        entry.directive = definition->directive;
        entry.argument = definition->argument.trimmed();
        entry.path = path;
        entry.line = static_cast< int >( index ) + 1 + lineOffset;
        entry.origin = origin;

        // 본문: 뒤따르는 "더 깊게 들여쓴" 줄들. 빈 줄은 본문에 속한다.
        const int   directiveIndent = indentWidth( lines.at( index ) );
        QStringList bodyLines;
        int         bodyIndent = -1;
        bool        sawBlank = false;
        qsizetype   scan = index + 1;
        for( ; scan < lines.size(); ++scan )
        {
            const QString& next = lines.at( scan );
            if( isBlank( next ) )
            {
                sawBlank = true;
                if( !bodyLines.isEmpty() )
                    bodyLines << QString();
                continue;
            }

            if( indentWidth( next ) <= directiveIndent )
                break;
            // 옵션은 빈 줄 앞에만 온다. 그 뒤의 `:foo:` 는 본문 글이다.
            if( !sawBlank && isOptionLine( next ) )
                continue;

            if( bodyIndent < 0 )
                bodyIndent = indentWidth( next );
            bodyLines << next.mid( qMin< qsizetype >( bodyIndent, next.size() ) );
        }

        while( !bodyLines.isEmpty() && bodyLines.constLast().isEmpty() )
            bodyLines.removeLast();
        entry.body = bodyLines.join( QLatin1Char( '\n' ) );

        entries.push_back( entry );
        index = scan - 1;
    }

    return entries;
}

QString pythonStringAssignment( const QString& source, const QString& variable,
                                int* firstContentLine )
{
    if( firstContentLine != nullptr )
        *firstContentLine = 0;
    if( variable.isEmpty() )
        return {};

    const QRegularExpression assignRe(
        QStringLiteral( R"(^[ \t]*%1[ \t]*=[ \t]*(.*)$)" )
            .arg( QRegularExpression::escape( variable ) ) );

    const QStringList lines = splitLines( source );
    for( qsizetype index = 0; index < lines.size(); ++index )
    {
        const QRegularExpressionMatch match = assignRe.match( lines.at( index ) );
        if( !match.hasMatch() )
            continue;

        QString rest = match.captured( 1 );
        bool    raw = false;
        while( !rest.isEmpty() && isStringPrefix( rest.at( 0 ) ) )
        {
            raw = raw || rest.at( 0 ) == QLatin1Char( 'r' ) || rest.at( 0 ) == QLatin1Char( 'R' );
            rest = rest.mid( 1 );
        }
        if( rest.isEmpty() )
            continue;

        const QChar quote = rest.at( 0 );
        if( quote != QLatin1Char( '"' ) && quote != QLatin1Char( '\'' ) )
            continue;   // 표현식이거나 다른 변수 대입이다. 우리가 값을 알 길이 없다

        // 내용의 첫 줄은 대입이 있는 줄이다 (여는 따옴표 바로 뒤부터가 내용).
        if( firstContentLine != nullptr )
            *firstContentLine = static_cast< int >( index ) + 1;

        const QString triple( 3, quote );
        if( rest.startsWith( triple ) )
        {
            const QString head = rest.mid( 3 );
            const int     close = head.indexOf( triple );
            if( close >= 0 )
                return raw ? head.left( close ) : unescapePython( head.left( close ) );

            QStringList collected{ head };
            for( qsizetype scan = index + 1; scan < lines.size(); ++scan )
            {
                const int end = lines.at( scan ).indexOf( triple );
                if( end < 0 )
                {
                    collected << lines.at( scan );
                    continue;
                }
                collected << lines.at( scan ).left( end );
                break;
            }
            const QString joined = collected.join( QLatin1Char( '\n' ) );
            return raw ? joined : unescapePython( joined );
        }

        // 한 줄 문자열.
        QString content;
        bool    closed = false;
        for( qsizetype pos = 1; pos < rest.size(); ++pos )
        {
            const QChar ch = rest.at( pos );
            if( !raw && ch == QLatin1Char( '\\' ) && pos + 1 < rest.size() )
            {
                content += ch;
                content += rest.at( ++pos );
                continue;
            }
            if( ch == quote )
            {
                closed = true;
                break;
            }
            content += ch;
        }
        if( !closed )
            continue;

        return raw ? content : unescapePython( content );
    }

    if( firstContentLine != nullptr )
        *firstContentLine = 0;
    return {};
}

QVector< SubstitutionEntry > parseConfSubstitutions( const QString& confText,
                                                     const QString& confPath )
{
    QVector< SubstitutionEntry > entries;
    for( const QLatin1String variable :
         { QLatin1String( "rst_prolog" ), QLatin1String( "rst_epilog" ) } )
    {
        int           firstLine = 0;
        const QString block = pythonStringAssignment( confText, variable, &firstLine );
        if( block.isEmpty() )
            continue;
        entries += parseSubstitutions( block, confPath, SubstitutionOrigin::Conf,
                                      qMax( 0, firstLine - 1 ) );
    }
    return entries;
}

QVector< SubstitutionEntry > builtinSubstitutions( const QString& confText, const QString& confPath )
{
    QVector< SubstitutionEntry > entries;

    const auto add = [ & ]( const QLatin1String name, const QLatin1String variable,
                            const QString& fallback )
    {
        SubstitutionEntry entry;
        entry.name = name;
        entry.origin = SubstitutionOrigin::Builtin;

        int           line = 0;
        const QString value =
            variable.size() == 0 ? QString{} : pythonStringAssignment( confText, variable, &line );
        if( value.isEmpty() )
        {
            entry.argument = fallback;
        }
        else
        {
            entry.argument = value;
            entry.path = confPath;
            entry.line = qMax( 1, line );
        }
        entries.push_back( entry );
    };

    add( QLatin1String( "version" ), QLatin1String( "version" ),
        QCoreApplication::translate( "RstSubstitutions", "conf.py 의 version" ) );
    add( QLatin1String( "release" ), QLatin1String( "release" ),
        QCoreApplication::translate( "RstSubstitutions", "conf.py 의 release" ) );
    add( QLatin1String( "today" ), QLatin1String( "" ),
        QCoreApplication::translate( "RstSubstitutions", "빌드한 날짜" ) );

    return entries;
}

// ── 인덱스 ────────────────────────────────────────────────

SubstitutionIndex::SubstitutionIndex( QObject* parent )
    : QObject( parent )
{
}

void SubstitutionIndex::setActiveProjectId( const QString& projectId )
{
    if( activeProjectId_ == projectId )
        return;

    activeProjectId_ = projectId;
    indexedProjectId_.clear();
    entries_.clear();
    byLowerName_.clear();
    ++generation_;   // 이전 워크스페이스/프로젝트의 늦은 스캔 결과를 버린다.
}

void SubstitutionIndex::refresh( const QString& projectId, const QString& sourceRoot,
                                 const QString& rootDoc, const QString& confPath, const bool force )
{
    if( projectId.isEmpty() || sourceRoot.isEmpty() )
        return;
    if( !force && indexedProjectId_ == projectId )
        return;

    indexedProjectId_ = projectId;
    const quint64                 generation = ++generation_;
    QPointer< SubstitutionIndex > guard( this );

    // 문서 수백 개를 읽는 일이라 GUI 스레드에서 하면 눈에 띄게 멈춘다.
    QThreadPool::globalInstance()->start(
        [ guard, sourceRoot, rootDoc, confPath, projectId, generation ]
        {
            QVector< SubstitutionEntry > entries;

            // conf.py 를 먼저 읽는다. rst_prolog 의 정의는 **모든 문서**에 붙으므로
            // 후보의 맨 앞자리이고, 내장 |version| 의 값도 여기서 나온다.
            QString confText;
            if( !confPath.isEmpty() )
            {
                QFile file( confPath );
                if( file.open( QIODevice::ReadOnly ) )
                    confText = rstline::fromUtf8File( file.readAll() );
            }
            entries += parseConfSubstitutions( confText, confPath );
            entries += builtinSubstitutions( confText, confPath );

            const QStringList paths = collectProjectDocuments( sourceRoot, rootDoc, kMaxDocuments );
            for( const QString& path : paths )
            {
                // 종료 중이면 멈춘다. 결과는 어차피 generation 검사에서 버려진다.
                if( isShuttingDown() )
                    return;

                QFile file( path );
                if( !file.open( QIODevice::ReadOnly | QIODevice::Text ) )
                    continue;
                entries += parseSubstitutions( rstline::fromUtf8File( file.readAll() ), path,
                                              SubstitutionOrigin::Project );
            }

            QMetaObject::invokeMethod(
                guard,
                [ guard, entries = std::move( entries ), projectId, generation ]() mutable
                {
                    if( guard )
                        guard->apply( projectId, std::move( entries ), generation );
                },
                Qt::QueuedConnection );
        } );
}

void SubstitutionIndex::apply( const QString& projectId, QVector< SubstitutionEntry > entries,
                               const quint64 generation )
{
    // 프로젝트가 바뀐 뒤 도착한 결과는 버린다.
    if( generation != generation_ )
        return;

    std::stable_sort( entries.begin(), entries.end(),
                     []( const SubstitutionEntry& lhs, const SubstitutionEntry& rhs )
                     {
                         const int left = originRank( lhs.origin );
                         const int right = originRank( rhs.origin );
                         if( left != right )
                             return left < right;
                         return lhs.name.toCaseFolded() < rhs.name.toCaseFolded();
                     } );

    entries_ = std::move( entries );
    byLowerName_.clear();
    byLowerName_.reserve( static_cast< int >( entries_.size() ) );
    for( qsizetype index = 0; index < entries_.size(); ++index )
    {
        // 같은 이름이 여러 곳에 있으면 **먼저 오는 출처**가 이긴다 (정렬이 끝난 뒤다).
        const QString key = entries_.at( index ).name.toCaseFolded();
        if( !byLowerName_.contains( key ) )
            byLowerName_.insert( key, static_cast< int >( index ) );
    }

    emit ready( projectId, static_cast< int >( entries_.size() ) );
}

const SubstitutionEntry* SubstitutionIndex::lookup( const QString& name ) const
{
    const auto it = byLowerName_.constFind( name.simplified().toCaseFolded() );
    if( it == byLowerName_.constEnd() )
        return nullptr;
    return &entries_.at( *it );
}

}  // namespace hc::rst
