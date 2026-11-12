#include "stdafx.h"
#include "sphinxProjectRequirements.hpp"

/// 모듈
#include "rstLineUtils.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::rst
{

namespace
{

/// 선언 파일은 작다. 이보다 크면 읽지 않는다( 잘못 놓인 큰 파일로 창이 멈추지 않게 ).
constexpr qint64 kMaxDeclarationBytes = 1024 * 1024;
/// 프로젝트 폴더에서 위로 몇 단계까지 선언 파일을 찾는가.
constexpr int    kMaxAncestorDepth = 6;
/// 가상환경에서 읽는 배포판 수의 상한.
constexpr int    kMaxInstalledDistributions = 2000;

QString readSmallText( const QString& path )
{
    QFile file( path );
    if( !file.open( QIODevice::ReadOnly ) || file.size() > kMaxDeclarationBytes )
        return {};
    return rstline::fromUtf8File( file.readAll() );
}

/// TOML 문자열 배열의 본문에서 문자열 리터럴만 뽑는다. 따옴표 안의 `]` 와 `#` 은 건너뛴다.
/// 끝( 짝이 맞는 `]` )을 만나면 true 를 돌려준다. 여러 줄에 걸친 배열은 줄마다 다시 부른다.
bool collectArrayStrings( const QString& text, QStringList& out )
{
    for( qsizetype index = 0; index < text.size(); ++index )
    {
        const QChar ch = text.at( index );
        if( ch == QLatin1Char( ']' ) )
            return true;
        if( ch == QLatin1Char( '#' ) )
            return false;   // 줄 끝까지 주석
        if( ch != QLatin1Char( '"' ) && ch != QLatin1Char( '\'' ) )
            continue;

        QString value;
        qsizetype end = index + 1;
        for( ; end < text.size() && text.at( end ) != ch; ++end )
        {
            // 기본 문자열( "..." )의 역슬래시 이스케이프. 요구 문자열에는 `\"` 정도만 나온다.
            if( ch == QLatin1Char( '"' ) && text.at( end ) == QLatin1Char( '\\' ) && end + 1 < text.size() )
                ++end;
            value += text.at( end );
        }
        if( end >= text.size() )
            return false;   // 닫히지 않은 문자열: 이 줄은 버린다.
        out << value;
        index = end;
    }
    return false;
}

}  // namespace

QString normalizedDistributionName( const QString& name )
{
    static const QRegularExpression separators( QStringLiteral( "[-_.]+" ) );
    return name.trimmed().toLower().replace( separators, QStringLiteral( "-" ) );
}

QString sanitizedRequirement( const QString& line )
{
    QString text = line;
    // 주석과 환경 표식을 뗀다. 표식은 HyperEditor 환경의 Python 에 맞는지 따지지 않는다.
    if( const qsizetype comment = text.indexOf( QStringLiteral( " #" ) ); comment >= 0 )
        text.truncate( comment );
    if( const qsizetype marker = text.indexOf( QLatin1Char( ';' ) ); marker >= 0 )
        text.truncate( marker );
    text = text.trimmed();
    if( text.isEmpty() || text.startsWith( QLatin1Char( '#' ) ) || text.startsWith( QLatin1Char( '-' ) ) )
        return {};

    // 이름[extras] 버전조건, 버전조건...  그 밖의 것( URL · 경로 · `이름 @ 주소` )은 받지 않는다.
    static const QRegularExpression shape( QStringLiteral(
        R"(^([A-Za-z0-9](?:[A-Za-z0-9._-]*[A-Za-z0-9])?)\s*(\[[A-Za-z0-9._-]+(?:\s*,\s*[A-Za-z0-9._-]+)*\])?\s*((?:(?:===|==|~=|!=|<=|>=|<|>)\s*[A-Za-z0-9][A-Za-z0-9.*+!_-]*)(?:\s*,\s*(?:===|==|~=|!=|<=|>=|<|>)\s*[A-Za-z0-9][A-Za-z0-9.*+!_-]*)*)?$)" ) );
    const QRegularExpressionMatch match = shape.match( text );
    if( !match.hasMatch() )
        return {};

    QString extras = match.captured( 2 );
    extras.remove( QLatin1Char( ' ' ) );
    QString versions = match.captured( 3 );
    versions.remove( QLatin1Char( ' ' ) );
    return match.captured( 1 ) + extras + versions;
}

QString requirementName( const QString& requirement )
{
    static const QRegularExpression name( QStringLiteral( R"(^[A-Za-z0-9](?:[A-Za-z0-9._-]*[A-Za-z0-9])?)" ) );
    return name.match( requirement.trimmed() ).captured( 0 );
}

QStringList requirementsFromPyproject( const QString& text )
{
    QStringList raw;
    QString table;
    bool inArray = false;

    static const QRegularExpression header( QStringLiteral( R"(^\s*\[\s*([A-Za-z0-9_.\-"' ]+?)\s*\]\s*(?:#.*)?$)" ) );
    static const QRegularExpression assignment( QStringLiteral( R"(^\s*([A-Za-z0-9_\-"']+)\s*=\s*\[(.*)$)" ) );

    const QStringList lines = text.split( QLatin1Char( '\n' ) );
    for( const QString& rawLine : lines )
    {
        const QString line = rawLine.endsWith( QLatin1Char( '\r' ) ) ? rawLine.chopped( 1 ) : rawLine;
        if( inArray )
        {
            inArray = !collectArrayStrings( line, raw );
            continue;
        }
        if( const auto match = header.match( line ); match.hasMatch() )
        {
            table = match.captured( 1 ).remove( QLatin1Char( '"' ) ).remove( QLatin1Char( '\'' ) ).remove( QLatin1Char( ' ' ) );
            continue;
        }

        const auto match = assignment.match( line );
        if( !match.hasMatch() )
            continue;
        QString key = match.captured( 1 );
        key.remove( QLatin1Char( '"' ) ).remove( QLatin1Char( '\'' ) );
        // [project] 에서는 dependencies 만, 선택 의존성 · 의존성 묶음 표에서는 모든 키( 묶음 이름 )를 읽는다.
        const bool wanted = ( table == QLatin1String( "project" ) && key == QLatin1String( "dependencies" ) )
                            || table == QLatin1String( "project.optional-dependencies" )
                            || table == QLatin1String( "dependency-groups" );
        if( !wanted )
            continue;
        inArray = !collectArrayStrings( match.captured( 2 ), raw );
    }

    QStringList out;
    for( const QString& item : std::as_const( raw ) )
        if( const QString requirement = sanitizedRequirement( item ); !requirement.isEmpty() )
            out << requirement;
    return out;
}

QStringList requirementsFromRequirementsText( const QString& text )
{
    QStringList out;
    for( const QString& line : text.split( QLatin1Char( '\n' ) ) )
        if( const QString requirement = sanitizedRequirement( line ); !requirement.isEmpty() )
            out << requirement;
    return out;
}

QString ProjectRequirements::specFor( const QString& distributionOrModule ) const
{
    const QString name = requirementName( distributionOrModule.trimmed() );
    if( name.isEmpty() )
        return {};
    // 이름 그대로( 정규화 )가 먼저다: sphinx_tabs -> sphinx-tabs, sphinxcontrib.mermaid -> sphinxcontrib-mermaid.
    if( const auto spec = specs.constFind( normalizedDistributionName( name ) ); spec != specs.constEnd() )
        return spec.value();
    // 배포명과 모듈 이름이 다른 것은 프로젝트 가상환경에서 읽은 모듈 표로 찾는다( PIL -> pillow ).
    const QString module = distributionOrModule.trimmed().section( QLatin1Char( '.' ), 0, 0 );
    if( const auto it = modules.constFind( module ); it != modules.constEnd() )
        if( const auto spec = specs.constFind( it.value() ); spec != specs.constEnd() )
            return spec.value();
    return {};
}

ProjectRequirements readProjectRequirements( const QString& projectRoot, const QString& venvDir )
{
    ProjectRequirements result;
    const auto add = [&result]( const QStringList& requirements ) {
        for( const QString& requirement : requirements )
        {
            const QString key = normalizedDistributionName( requirementName( requirement ) );
            // 먼저 읽은 것( 프로젝트에 가까운 선언 )이 남는다.
            if( !key.isEmpty() && !result.specs.contains( key ) )
                result.specs.insert( key, requirement );
        }
    };

    // 1) 선언 파일: 프로젝트 폴더에서 위로. 저장소 뿌리( .git )를 지나면 멈춘다.
    static const QStringList requirementFiles{
        QStringLiteral( "requirements.txt" ),       QStringLiteral( "requirements-docs.txt" ),
        QStringLiteral( "docs-requirements.txt" ),  QStringLiteral( "docs/requirements.txt" ),
        QStringLiteral( "doc/requirements.txt" ),   QStringLiteral( "requirements/docs.txt" ),
        QStringLiteral( "requirements/doc.txt" ) };
    QDir current( projectRoot );
    for( int depth = 0; depth < kMaxAncestorDepth && !projectRoot.isEmpty(); ++depth )
    {
        for( const QString& name : requirementFiles )
        {
            const QString path = current.filePath( name );
            if( !QFileInfo( path ).isFile() )
                continue;
            const QStringList found = requirementsFromRequirementsText( readSmallText( path ) );
            if( !found.isEmpty() )
                result.sources << QDir::toNativeSeparators( path );
            add( found );
        }
        if( const QString path = current.filePath( QStringLiteral( "pyproject.toml" ) ); QFileInfo( path ).isFile() )
        {
            const QStringList found = requirementsFromPyproject( readSmallText( path ) );
            if( !found.isEmpty() )
                result.sources << QDir::toNativeSeparators( path );
            add( found );
        }
        if( QFileInfo::exists( current.filePath( QStringLiteral( ".git" ) ) ) || !current.cdUp() )
            break;
    }

    // 2) 프로젝트 가상환경에 설치된 배포판: 이름과 버전, 그리고 모듈 이름. 실행하지 않고 폴더만 읽는다.
    if( !venvDir.isEmpty() )
    {
        const QDir packages( QDir( venvDir ).filePath( QStringLiteral( "Lib/site-packages" ) ) );
        const QStringList installed =
            packages.entryList( { QStringLiteral( "*.dist-info" ) }, QDir::Dirs | QDir::NoDotAndDotDot );
        if( !installed.isEmpty() )
            result.sources << QDir::toNativeSeparators( packages.absolutePath() );

        // 여러 배포판이 함께 쓰는 최상위 이름( sphinxcontrib 같은 네임스페이스 )은 어느 것인지 알 수 없다.
        QStringList shared;
        const auto addModule = [&result, &shared]( const QString& module, const QString& key ) {
            if( shared.contains( module ) )
                return;
            const auto it = result.modules.constFind( module );
            if( it == result.modules.constEnd() )
                result.modules.insert( module, key );
            else if( it.value() != key )
            {
                result.modules.remove( module );
                shared << module;
            }
        };

        int count = 0;
        for( const QString& entry : installed )
        {
            if( ++count > kMaxInstalledDistributions )
                break;
            // <이름>-<버전>.dist-info. 이름 안의 `-` 는 `_` 로 바뀌어 있으므로 마지막 `-` 가 경계다.
            const QString stem = entry.chopped( 10 );
            const qsizetype dash = stem.lastIndexOf( QLatin1Char( '-' ) );
            if( dash <= 0 )
                continue;
            const QString name = stem.left( dash );
            const QString requirement = sanitizedRequirement( name + QStringLiteral( "==" ) + stem.mid( dash + 1 ) );
            const QString key = normalizedDistributionName( name );
            if( requirement.isEmpty() || key.isEmpty() )
                continue;
            if( !result.specs.contains( key ) )
                result.specs.insert( key, requirement );

            // 모듈 이름: top_level.txt 가 있으면 그것, 없으면 배포명에서 추정한다( sphinx-tabs -> sphinx_tabs ).
            const QString topLevel = readSmallText( packages.filePath( entry + QStringLiteral( "/top_level.txt" ) ) );
            bool any = false;
            for( const QString& line : topLevel.split( QLatin1Char( '\n' ), Qt::SkipEmptyParts ) )
            {
                const QString module = line.trimmed();
                if( module.isEmpty() || module.startsWith( QLatin1Char( '_' ) ) )
                    continue;
                any = true;
                addModule( module, key );
            }
            if( !any )
            {
                QString module = key;
                module.replace( QLatin1Char( '-' ), QLatin1Char( '_' ) );
                addModule( module, key );
            }
        }
    }
    return result;
}

QStringList resolveMissingPackages( const ProjectRequirements& requirements, const QStringList& distributions,
                                    const QStringList& themes )
{
    QStringList out;
    QStringList seen;
    const auto add = [&]( const QString& name ) {
        QString requirement = requirements.specFor( name );
        if( requirement.isEmpty() )
        {
            // 프로젝트가 밝힌 것이 없다: 이름만으로 설치한다. 이름도 같은 검사를 거친다.
            requirement = sanitizedRequirement( normalizedDistributionName( name ) );
        }
        const QString key = normalizedDistributionName( requirementName( requirement ) );
        if( requirement.isEmpty() || key.isEmpty() || seen.contains( key ) )
            return;
        seen << key;
        out << requirement;
    };
    for( const QString& name : distributions )
        add( name );
    // 테마 이름은 모듈 이름과 같다( furo, sphinx_rtd_theme ). 내장 테마는 빌드가 없다고 알리지 않는다.
    for( const QString& name : themes )
        add( name );
    return out;
}

}  // namespace hc::rst
