#include "stdafx.h"
#include "rstOutlineService.hpp"

/// 모듈
#include "rstLineUtils.hpp"
#include "rstFileKinds.hpp"
#include "rstBackgroundWork.hpp"
#include "text/MarkdownStructure.h"
#include "text/RstStructure.h"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

/// LSP SymbolKind::Number. 파이썬 원본과 맞춘다 (섹션에 딱 맞는 kind 가 없다).
constexpr int kSectionKind = 12;

struct Heading
{
    QString                             name;
    int                                 line = 1;   ///< 1-based, 제목 글자 줄
    QString                             style;      ///< "over:=" / "under:-"
};

/// 제목 판정은 syntax::titleScanAt 이 한다 — 렉서·접기와 같은 규칙이다.
///
/// 예전에는 여기에 판정이 한 벌 더 있었고 렉서와 다섯 축(문자 집합·동질성·최소
/// 길이·들여쓰기·길이 비교 단위)에서 갈라져 있었다. 그래서 개요에는 나오는데
/// 화면에는 색도 접기 마커도 없는 섹션이 실제로 생겼다.
QVector< Heading > sectionHeadings( const QString& text )
{
    const QByteArray     utf8 = text.toUtf8();
    const syntax::LineIndex lines( std::string_view( utf8.constData(),
                                                  static_cast< std::size_t >( utf8.size() ) ) );

    QVector< Heading > headings;
    for( std::size_t index = 0; index < lines.size(); )
    {
        const syntax::TitleScan scan = syntax::titleScanAt( lines, index );
        if( scan.title )
        {
            headings.push_back(
                { QString::fromUtf8( scan.title->text.data(),
                                     static_cast< qsizetype >( scan.title->text.size() ) ),
                  static_cast< int >( scan.title->textLine ) + 1,
                  QStringLiteral( "%1:%2" )
                      .arg( scan.title->overlined ? QStringLiteral( "over" )
                                                  : QStringLiteral( "under" ) )
                      .arg( QLatin1Char( scan.title->adornChar ) ) } );
        }
        // 제목이 아니어도 docutils 가 통째로 소비하는 구간이면 그만큼 건너뛴다.
        index += ( scan.span > 0 ) ? scan.span : 1;
    }
    return headings;
}

}  // namespace

QVector< OutlineSymbol > parseRstOutline( const QString& text, const QString& path )
{
    const QVector< Heading > headings = sectionHeadings( text );
    if( headings.isEmpty() )
        return {};

    QVector< OutlineSymbol > roots;
    // 부모를 가리키는 포인터를 담으면 벡터가 재할당될 때 전부 무효가 된다.
    // 루트에서부터의 자식 인덱스 경로로 들고 있는다.
    struct StackEntry
    {
        int                             level;
        QVector< int >                  indexPath;
    };
    QVector< StackEntry > stack;
    QHash< QString, int > styleLevels;

    auto resolve = [ &roots ]( const QVector< int >& indexPath ) -> QVector< OutlineSymbol >& {
        QVector< OutlineSymbol >* siblings = &roots;
        for( const int index : indexPath )
            siblings = &( ( *siblings )[ index ].children );
        return *siblings;
    };

    for( const Heading& heading : headings )
    {
        // 계층은 밑줄 문자가 **처음 나온 순서** 로 정해진다. docutils 규칙이다.
        const int level = styleLevels.value( heading.style, styleLevels.size() + 1 );
        styleLevels.insert( heading.style, level );

        while( !stack.isEmpty() && stack.last().level >= level )
            stack.removeLast();

        OutlineSymbol symbol;
        symbol.name = heading.name;
        symbol.detail = QStringLiteral( "reST" );
        symbol.kind = kSectionKind;
        symbol.line = heading.line;
        symbol.path = path;

        QVector< int > parentPath = stack.isEmpty() ? QVector< int >{} : stack.last().indexPath;
        QVector< OutlineSymbol >& siblings = resolve( parentPath );
        siblings.push_back( symbol );

        parentPath.push_back( static_cast< int >( siblings.size() ) - 1 );
        stack.push_back( { level, parentPath } );
    }

    return roots;
}

QVector< OutlineSymbol > parseMarkdownOutline( const QString& text, const QString& path )
{
    // 접기와 같은 스캐너를 쓴다. 코드펜스와 front matter 판정을 두 벌 두면 개요와
    // 접기가 서로 다른 제목 목록을 갖게 되고, 그때 증상은 "접기가 이상하다" 로
    // 나타나면서 원인은 이쪽에 있게 된다.
    const QByteArray utf8 = text.toUtf8();
    const hc::rst::md::MdScan scan =
        hc::rst::md::scanMarkdown( std::string( utf8.constData(), static_cast< std::size_t >( utf8.size() ) ) );
    if( scan.headings.empty() )
        return {};

    QVector< OutlineSymbol > roots;
    // parseRstOutline 과 같은 이유로 부모를 포인터가 아니라 인덱스 경로로 들고
    // 있는다. 포인터를 담으면 벡터가 재할당될 때 전부 무효가 된다.
    struct StackEntry
    {
        int            level;
        QVector< int > indexPath;
    };
    QVector< StackEntry > stack;

    auto resolve = [ &roots ]( const QVector< int >& indexPath ) -> QVector< OutlineSymbol >& {
        QVector< OutlineSymbol >* siblings = &roots;
        for( const int index : indexPath )
            siblings = &( ( *siblings )[ index ].children );
        return *siblings;
    };

    for( const hc::rst::md::MdHeading& heading : scan.headings )
    {
        // 해시 개수가 곧 단계다. reST 의 styleLevels(처음 나온 순서) 는 필요 없다.
        const int level = heading.level;

        while( !stack.isEmpty() && stack.last().level >= level )
            stack.removeLast();

        OutlineSymbol symbol;
        symbol.name = QString::fromUtf8( heading.text.c_str(),
                                         static_cast< qsizetype >( heading.text.size() ) );
        // tr() 로 감싸지 않는다. 마크업 고유명사이고, LSP 가 채우는 detail(예:
        // "class Foo") 과 같은 슬롯이라 번역하면 두 출처가 섞인다.
        symbol.detail = QStringLiteral( "Markdown" );
        symbol.kind = kSectionKind;
        symbol.line = static_cast< int >( heading.line );
        symbol.path = path;

        QVector< int > parentPath = stack.isEmpty() ? QVector< int >{} : stack.last().indexPath;
        QVector< OutlineSymbol >& siblings = resolve( parentPath );
        siblings.push_back( symbol );

        parentPath.push_back( static_cast< int >( siblings.size() ) - 1 );
        stack.push_back( { level, parentPath } );
    }

    return roots;
}

QVector< OutlineSymbol > parseDocumentOutline( const QString& text, const QString& path )
{
    if( filekinds::hasExtension( path, filekinds::markdownExtensions() ) )
        return parseMarkdownOutline( text, path );
    return parseRstOutline( text, path );
}

QVector< OutlineSymbol > toOutlineSymbols( const QList< LspDocumentSymbol >& symbols,
                                           const QString& path )
{
    QVector< OutlineSymbol > result;
    result.reserve( symbols.size() );
    for( const LspDocumentSymbol& symbol : symbols )
    {
        OutlineSymbol converted;
        converted.name = symbol.name;
        converted.detail = symbol.detail;
        converted.kind = symbol.kind;
        converted.line = qMax( 1, symbol.line );
        converted.path = path;
        converted.children = toOutlineSymbols( symbol.children, path );
        result.push_back( converted );
    }
    return result;
}

QStringList collectProjectDocuments( const QString& sourceRoot, const QString& rootDoc,
                                     const int limit, int* totalFound )
{
    const QDir root( QFileInfo( sourceRoot ).absoluteFilePath() );
    if( !root.exists() )
    {
        if( totalFound != nullptr )
            *totalFound = 0;
        return {};
    }

    // 확장자 목록의 단일 출처는 filekinds 다. 여기에 사본을 두었더니 .rest 와
    // .markdown / .mdown 이 빠져 있었다 — 그 확장자로 쓴 문서는 프로젝트 개요에
    // 아예 올라오지 않는다.
    //
    // 이 목록은 용어집 인덱스(solGlossaryIndex) 와 공유한다. 넓히면 그쪽 스캔
    // 범위도 함께 넓어진다. parseGlossary() 는 `.. glossary::` 를 찾으므로 md 에서
    // 오탐하지 않고 비용은 파일 I/O 뿐이며 상한(kMaxGlossaryDocuments)도 이미 있다.
    const QStringList& suffixes = filekinds::documentExtensions();

    QStringList documents;
    QStringList pendingDirectories{root.absolutePath()};
    while (!pendingDirectories.isEmpty()) {
        if (isShuttingDown())
            return {};
        const QString directory = pendingDirectories.takeLast();
        // Single-level iteration lets us prune excluded trees before any descent.
        QDirIterator entries(directory, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        while (entries.hasNext()) {
            if (isShuttingDown())
                return {};
            entries.next();
            const QFileInfo info = entries.fileInfo();
            if (info.isDir()) {
                if (!info.isSymLink() && !info.isJunction()
                    && !filekinds::isExcludedDirectoryName(info.fileName()))
                    pendingDirectories.append(info.absoluteFilePath());
            } else if (suffixes.contains(info.suffix().toCaseFolded())) {
                documents.append(QDir::cleanPath(info.absoluteFilePath()));
            }
        }
    }

    // root_doc 이 맨 앞. 나머지는 상대경로순.
    const QString rootStem = QFileInfo( rootDoc ).path() == QStringLiteral( "." )
                                 ? QFileInfo( rootDoc ).completeBaseName().toCaseFolded()
                                 : ( QFileInfo( rootDoc ).path() + QLatin1Char( '/' )
                                    + QFileInfo( rootDoc ).completeBaseName() )
                                       .toCaseFolded();

    auto sortKey = [ &root ]( const QString& path ) {
        const QString relative = root.relativeFilePath( path );
        const qsizetype dot = relative.lastIndexOf( QLatin1Char( '.' ) );
        return QPair< QString, QString >{ dot > 0 ? relative.left( dot ).toCaseFolded()
                                                  : relative.toCaseFolded(),
                                         relative.toCaseFolded() };
    };

    std::sort( documents.begin(), documents.end(),
              [ & ]( const QString& left, const QString& right ) {
                  const auto leftKey = sortKey( left );
                  const auto rightKey = sortKey( right );
                  const bool leftIsRoot = leftKey.first == rootStem;
                  const bool rightIsRoot = rightKey.first == rootStem;
                  if( leftIsRoot != rightIsRoot )
                      return leftIsRoot;
                  return leftKey.second < rightKey.second;
              } );

    if( totalFound != nullptr )
        *totalFound = static_cast< int >( documents.size() );

    if( limit > 0 && documents.size() > limit )
        documents = documents.mid( 0, limit );
    return documents;
}

QVector< OutlineDocumentEntry > buildProjectOutline( const QString& sourceRoot,
                                                     const QStringList& documents )
{
    const QDir root( QFileInfo( sourceRoot ).absoluteFilePath() );

    QVector< OutlineDocumentEntry > entries;
    entries.reserve( documents.size() );
    for( const QString& path : documents )
    {
        // 문서마다 파일을 열고 전부 읽는다(상한 500). 종료 중이면 멈춘다.
        if( isShuttingDown() )
            return {};

        OutlineDocumentEntry entry;
        entry.path = path;
        entry.label = root.relativeFilePath( path );
        if( entry.label.startsWith( QStringLiteral( ".." ) ) )
            entry.label = QFileInfo( path ).fileName();

        QFile file( path );
        if( file.open( QIODevice::ReadOnly | QIODevice::Text ) )
        {
            // 개요만 뽑으므로 인코딩 추정까지 하지 않는다. UTF-8 이 아니면
            // 제목이 깨져 보일 수는 있어도 줄 번호는 맞다.
            entry.symbols = parseDocumentOutline( rstline::fromUtf8File( file.readAll() ), path );
        }
        entries.push_back( entry );
    }
    return entries;
}

}  // namespace hc::rst
