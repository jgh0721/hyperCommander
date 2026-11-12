#include "stdafx.h"
#include "rstPathCompletion.hpp"

/// 모듈
#include "rstFileKinds.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <queue>
#include <utility>
#include <vector>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst::rstpath {

namespace {

QStringList delimitedExtensions()
{
    return { QStringLiteral( "csv" ), QStringLiteral( "txt" ), QStringLiteral( "tsv" ) };
}

QStringList graphExtensions()
{
    return { QStringLiteral( "dot" ), QStringLiteral( "gv" ) };
}

/// `.. name:: <인자>` 가 경로인 directive.
///
/// 근거 (docutils 0.21 / Sphinx 8):
///   image           images.py 가 directives.uri() 로 변환한다
///   figure          Figure 는 Image 를 상속하므로 같은 변환
///   include         sphinx/directives/other.py 가 env.relfn2path() 를 부른다
///   literalinclude  sphinx/directives/code.py 도 같은 호출
///   graphviz        sphinx/ext/graphviz.py, final_argument_whitespace=false
///
/// **raw 와 csv-table 은 여기 없다.** raw 의 인자는 포맷 이름(html/latex)이고
/// csv-table 의 인자는 표 제목이다. 둘 다 경로는 :file: 옵션에 있다. 예전
/// takesPathArgument() 는 이 둘을 경로로 오인해서 표 제목 자리에 파일 목록을 띄웠다.
const QHash< QString, Slot >& argumentSlots()
{
    static const QHash< QString, Slot > table{
        { QStringLiteral( "image" ),
         Slot{ Shape::FilePath, Spaces::Escape, filekinds::imageExtensions(), {} } },
        { QStringLiteral( "figure" ),
         Slot{ Shape::FilePath, Spaces::Escape, filekinds::imageExtensions(), {} } },
        { QStringLiteral( "include" ),
         Slot{ Shape::FilePath, Spaces::Preserve, {}, filekinds::documentExtensions() } },
        { QStringLiteral( "literalinclude" ),
         Slot{ Shape::FilePath, Spaces::Preserve, {}, filekinds::textLikeExtensions() } },
        { QStringLiteral( "graphviz" ),
         Slot{ Shape::FilePath, Spaces::Forbidden, graphExtensions(), {} } },
    };
    return table;
}

/// "<directive>|<option>" -> 슬롯.
const QHash< QString, Slot >& optionSlots()
{
    static const QHash< QString, Slot > table{
        // 원본 크기 이미지나 다른 문서로 거는 링크. URL 일 수도 있어서 거르지
        // 않고 순위만 올린다.
        { QStringLiteral( "image|target" ),
         Slot{ Shape::FilePath, Spaces::Escape, {},
              filekinds::imageExtensions() + filekinds::documentExtensions() } },
        { QStringLiteral( "figure|target" ),
         Slot{ Shape::FilePath, Spaces::Escape, {},
              filekinds::imageExtensions() + filekinds::documentExtensions() } },
        // docutils tables.py 가 directives.path 로 받는다.
        { QStringLiteral( "csv-table|file" ),
         Slot{ Shape::FilePath, Spaces::Preserve, delimitedExtensions(), {} } },
        // docutils misc.py 가 directives.path 로 받는다.
        { QStringLiteral( "raw|file" ),
         Slot{ Shape::FilePath, Spaces::Preserve, {}, filekinds::textLikeExtensions() } },
        // sphinx code.py 가 relfn2path( options["diff"] ) 를 부른다.
        { QStringLiteral( "literalinclude|diff" ),
         Slot{ Shape::FilePath, Spaces::Preserve, {}, filekinds::textLikeExtensions() } },
    };
    return table;
}

/// directive 본문의 각 줄이 경로인 경우.
const QHash< QString, Slot >& bodySlots()
{
    static const QHash< QString, Slot > table{
        // sphinx/directives/other.py 는 접미사를 뗀 뒤 docname_join 한다.
        { QStringLiteral( "toctree" ),
         Slot{ Shape::DocName, Spaces::Forbidden, filekinds::documentExtensions(), {} } },
    };
    return table;
}

const QHash< QString, Slot >& roleTargetSlots()
{
    static const QHash< QString, Slot > table{
        // sphinx 의 download 롤도 relfn2path 를 탄다. 무엇이든 내려받을 수 있다.
        { QStringLiteral( "download" ), Slot{ Shape::FilePath, Spaces::Preserve, {}, {} } },
        { QStringLiteral( "doc" ),
         Slot{ Shape::DocName, Spaces::Forbidden, filekinds::documentExtensions(), {} } },
    };
    return table;
}

const Slot* lookup( const QHash< QString, Slot >& table, const QString& key )
{
    const auto it = table.constFind( key );
    return it == table.constEnd() ? nullptr : &it.value();
}

}   // namespace

const Slot* slotForArgument( const QString& directiveName )
{
    return lookup( argumentSlots(), directiveName );
}

const Slot* slotForOption( const QString& directiveName, const QString& optionName )
{
    return lookup( optionSlots(), directiveName + QLatin1Char( '|' ) + optionName );
}

const Slot* slotForBody( const QString& directiveName )
{
    return lookup( bodySlots(), directiveName );
}

const Slot* slotForRoleTarget( const QString& roleName )
{
    return lookup( roleTargetSlots(), roleName );
}

const Slot* slotFor( const rstcomplete::Context& context )
{
    if( context.kind != rstcomplete::ContextKind::Path )
        return nullptr;

    switch( context.pathSite )
    {
        case rstcomplete::PathSlotSite::Argument:
            return slotForArgument( context.directiveName );
        case rstcomplete::PathSlotSite::Option:
            return slotForOption( context.directiveName, context.optionName );
        case rstcomplete::PathSlotSite::Body:
            return slotForBody( context.directiveName );
        case rstcomplete::PathSlotSite::RoleTarget:
            return slotForRoleTarget( context.directiveName );
        case rstcomplete::PathSlotSite::None:
            break;
    }
    return nullptr;
}

TypedPath splitTypedPath( const QString& typed )
{
    TypedPath result;
    result.fromSourceRoot = typed.startsWith( QLatin1Char( '/' ) )
                            || typed.startsWith( QLatin1Char( '\\' ) );

    QString decoded;
    decoded.reserve( typed.length() );
    int lastSeparator = -1;

    for( int index = 0; index < typed.length(); ++index )
    {
        const QChar ch = typed.at( index );

        // reST 에서 백슬래시+공백은 이스케이프된 공백이지 경로 구분자가 아니다.
        // 이걸 먼저 보지 않으면 image 인자의 "my\ photo.png" 가 두 조각으로 잘린다.
        if( ch == QLatin1Char( '\\' ) && index + 1 < typed.length()
            && typed.at( index + 1 ) == QLatin1Char( ' ' ) )
        {
            decoded.append( QLatin1Char( ' ' ) );
            ++index;
            continue;
        }

        if( ch == QLatin1Char( '/' ) || ch == QLatin1Char( '\\' ) )
        {
            decoded.append( QLatin1Char( '/' ) );
            lastSeparator = static_cast< int >( decoded.length() ) - 1;
            continue;
        }

        decoded.append( ch );
    }

    if( lastSeparator < 0 )
    {
        result.name = decoded;
        return result;
    }

    result.directory = decoded.left( lastSeparator );   // 끝의 '/' 는 뗀다
    result.name = decoded.mid( lastSeparator + 1 );
    return result;
}

QString encodeForInsertion( const QString& relativePath, const Slot& slot )
{
    QString text = relativePath;
    text.replace( QLatin1Char( '\\' ), QLatin1Char( '/' ) );

    if( slot.spaces == Spaces::Escape )
    {
        // docutils 의 uri() 는 이스케이프하지 않은 공백을 통째로 지운다.
        // 백슬래시를 앞에 붙여야 실제 공백이 있는 파일을 가리킬 수 있다.
        // %20 은 답이 아니다 - relfn2path 는 퍼센트 디코딩을 하지 않는다.
        text.replace( QLatin1Char( ' ' ), QStringLiteral( "\\ " ) );
    }
    return text;
}

bool acceptsFileName( const QString& fileName, const Slot& slot )
{
    if( slot.spaces == Spaces::Forbidden && fileName.contains( QLatin1Char( ' ' ) ) )
        return false;
    if( slot.accepted.isEmpty() )
        return true;
    return filekinds::hasExtension( fileName, slot.accepted );
}

QString stripDocumentSuffix( const QString& fileName )
{
    for( const QString& suffix : filekinds::documentExtensions() )
    {
        const QString dotted = QLatin1Char( '.' ) + suffix;
        if( fileName.endsWith( dotted, Qt::CaseInsensitive ) )
            return fileName.left( fileName.length() - dotted.length() );
    }
    return fileName;
}

bool isWithin( const QString& root, const QString& child )
{
    if( root.isEmpty() )
        return true;

    const QString normalizedRoot = QDir::cleanPath( root );
    const QString normalizedChild = QDir::cleanPath( child );
    if( normalizedChild.compare( normalizedRoot, Qt::CaseInsensitive ) == 0 )
        return true;

    const QString prefix = normalizedRoot.endsWith( QLatin1Char( '/' ) )
                               ? normalizedRoot
                               : normalizedRoot + QLatin1Char( '/' );
    return normalizedChild.startsWith( prefix, Qt::CaseInsensitive );
}

// ── 후보 만들기 ───────────────────────────────────────────

DirectoryLister diskLister( const int maxEntries )
{
    return [ maxEntries ]( const QString& absoluteDirectory ) {
        QVector< DirEntry > entries;
        if( absoluteDirectory.isEmpty() )
            return entries;

        // entryInfoList 대신 반복자를 쓴다. 목록을 통째로 물질화하지 않고
        // 상한에서 그냥 멈출 수 있다.
        QDirIterator iterator( absoluteDirectory,
                              QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                              QDirIterator::NoIteratorFlags );
        while( iterator.hasNext() )
        {
            iterator.next();
            const QString name = iterator.fileName();
            // 숨김 항목과 빌드 산출물은 사용자가 문서에서 참조할 것이 아니다.
            if( name.startsWith( QLatin1Char( '.' ) ) )
                continue;
            const bool isDirectory = iterator.fileInfo().isDir();
            if( isDirectory && filekinds::isExcludedDirectoryName( name ) )
                continue;

            entries.push_back( { name, isDirectory } );
            if( entries.size() >= maxEntries )
                break;
        }
        return entries;
    };
}

namespace {

/// 문서(또는 소스 루트) 기준으로 문서에 넣을 문자열을 만든다.
QString insertTextFor( const Query& query, const Slot& slot, const QString& absolutePath,
                       const bool isDirectory )
{
    const TypedPath typed = splitTypedPath( query.context.prefix );

    QString relative;
    // 사용자가 고른 표기를 뒤엎지 않는다. '/' 로 시작해 쳤으면 그대로 둔다.
    // 단 소스 루트 밖 파일에까지 그러면 "/../.." 같은 것이 나오므로 그때는
    // 문서 기준 상대 경로로 돌아간다 (전역 후보에서 실제로 생긴다).
    if( typed.fromSourceRoot && !query.sourceRoot.isEmpty()
        && isWithin( query.sourceRoot, absolutePath ) )
    {
        relative = QLatin1Char( '/' )
                   + QDir( query.sourceRoot ).relativeFilePath( absolutePath );
    }
    else
    {
        relative = QDir( query.documentDirectory ).relativeFilePath( absolutePath );
    }

    if( !isDirectory && slot.shape == Shape::DocName )
        relative = stripDocumentSuffix( relative );

    QString text = encodeForInsertion( relative, slot );
    // QDir::relativeFilePath 는 상위로 올라가는 단계마다 "../" 를 **슬래시까지
    // 붙여** 내놓는다 (qdir.cpp). 부모 디렉터리를 물으면 ".." 이 아니라 "../" 다.
    // 그대로 한 번 더 붙이면 "..//" 이 된다.
    if( isDirectory && !text.endsWith( QLatin1Char( '/' ) ) )
        text += QLatin1Char( '/' );
    // ".. image::" 처럼 인자 앞 공백이 아직 없으면 여기서 붙인다.
    // replaceLength 가 0 이므로 그냥 앞에 놓으면 된다.
    if( query.context.argumentNeedsSpace )
        text.prepend( QLatin1Char( ' ' ) );
    return text;
}

int kindFor( const QString& name, const bool isDirectory )
{
    if( isDirectory )
        return 19;   // LSP CompletionItemKind::Folder
    return filekinds::isImageFile( name ) ? rstcomplete::kKindImageFile : 17;   // File
}

/// 이름 정렬. 로케일을 알고 숫자를 숫자로 보는 비교 (파일 이름에 번호가 흔하다).
QCollator& nameCollator()
{
    static QCollator collator = [] {
        QCollator value;
        value.setNumericMode( true );
        value.setCaseSensitivity( Qt::CaseInsensitive );
        return value;
    }();
    return collator;
}

}   // namespace

QString resolveTypedDirectory( const Query& query )
{
    const TypedPath typed = splitTypedPath( query.context.prefix );

    QString base = query.documentDirectory;
    QString directory = typed.directory;
    if( typed.fromSourceRoot )
    {
        // Sphinx 의 relfn2path 와 같은 규칙: 선행 구분자는 srcdir 기준이다.
        base = query.sourceRoot.isEmpty() ? query.documentDirectory : query.sourceRoot;
        directory = directory.mid( 1 );   // 앞의 '/' 를 뗀다
    }
    if( base.isEmpty() )
        return {};

    // 존재 확인은 하지 않는다. 없는 디렉터리면 lister 가 빈 목록을 돌려주므로
    // 결과가 같고, 이 함수가 순수 문자열 연산으로 남아 디스크 없이 검증된다.
    return directory.isEmpty()
               ? QDir::cleanPath( base )
               : QDir::cleanPath( base + QLatin1Char( '/' ) + directory );
}

QVector< Candidate > oneLevelCandidates( const Query& query, const DirectoryLister& lister,
                                         const int limit )
{
    QVector< Candidate > candidates;
    const Slot* slot = slotFor( query.context );
    if( slot == nullptr || !lister )
        return candidates;

    const QString directory = resolveTypedDirectory( query );
    if( directory.isEmpty() )
        return candidates;

    QVector< DirEntry > entries = lister( directory );
    std::stable_sort( entries.begin(), entries.end(),
                     []( const DirEntry& left, const DirEntry& right ) {
                         if( left.isDirectory != right.isDirectory )
                             return left.isDirectory;   // 디렉터리가 먼저
                         return nameCollator().compare( left.name, right.name ) < 0;
                     } );

    // 한 단계 위로 올라가는 길. 셸과 탐색기의 오랜 관례라 없으면 오히려 헤맨다.
    // docname 슬롯에서는 소스 루트 밖으로 나가면 반드시 빌드 경고가 되므로 뺀다.
    const QString parent = QDir::cleanPath( directory + QStringLiteral( "/.." ) );
    // 없는 디렉터리를 쳤을 때 ".." 만 덩그러니 뜨지 않도록 항목이 있을 때만 붙인다.
    const bool parentAllowed = !entries.isEmpty() && parent != directory
                               && ( slot->shape != Shape::DocName
                                    || isWithin( query.sourceRoot, parent ) );
    if( parentAllowed )
    {
        Candidate up;
        up.label = QStringLiteral( ".." );
        up.insertText = insertTextFor( query, *slot, parent, true );
        up.kind = 19;
        up.scoreBias = 40;
        up.absolutePath = parent;
        up.isDirectory = true;
        candidates.push_back( up );
    }

    for( const DirEntry& entry : std::as_const( entries ) )
    {
        const QString absolute = directory + QLatin1Char( '/' ) + entry.name;

        if( entry.isDirectory )
        {
            if( slot->shape == Shape::DocName && !isWithin( query.sourceRoot, absolute ) )
                continue;

            Candidate candidate;
            candidate.label = entry.name;
            candidate.insertText = insertTextFor( query, *slot, absolute, true );
            candidate.kind = 19;
            // 현재 디렉터리 +30, 디렉터리라서 +10. 접두가 있으면 팝업이 순수
            // 퍼지 점수로 정렬하므로, 가중치가 없으면 이름이 짧은 전역 후보가
            // 눈앞의 디렉터리를 이겨 버린다.
            candidate.scoreBias = 40;
            candidate.absolutePath = absolute;
            candidate.isDirectory = true;
            candidates.push_back( candidate );
            continue;
        }

        if( !acceptsFileName( entry.name, *slot ) )
            continue;
        if( slot->shape == Shape::DocName && !isWithin( query.sourceRoot, absolute ) )
            continue;

        Candidate candidate;
        candidate.label = slot->shape == Shape::DocName ? stripDocumentSuffix( entry.name )
                                                        : entry.name;
        candidate.insertText = insertTextFor( query, *slot, absolute, false );
        candidate.kind = kindFor( entry.name, false );
        candidate.scoreBias = 30;
        if( !slot->preferred.isEmpty() && filekinds::hasExtension( entry.name, slot->preferred ) )
            candidate.scoreBias += 8;
        candidate.absolutePath = absolute;
        candidates.push_back( candidate );

        if( candidates.size() >= limit )
            break;
    }

    return candidates;
}

QVector< Candidate > fuzzyCandidates( const Query& query, const QString& indexRoot,
                                      const QStringList& indexedPaths, const int limit,
                                      const qsizetype scanLimit )
{
    QVector< Candidate > candidates;
    const Slot* slot = slotFor( query.context );
    if( slot == nullptr || indexRoot.isEmpty() || indexedPaths.isEmpty() || limit <= 0 )
        return candidates;

    const TypedPath typed = splitTypedPath( query.context.prefix );
    // 접두가 너무 짧으면 전역 목록이 사실상 무작위다. 한 단계 후보만 남긴다.
    if( typed.name.length() < 2 )
        return candidates;

    struct Scored
    {
        Candidate                       candidate;
        int                             score = 0;
        int                             depth = 0;
    };

    const auto better = []( const Scored& left, const Scored& right ) {
        if( left.score != right.score )
            return left.score > right.score;
        if( left.depth != right.depth )
            return left.depth < right.depth;   // 얕은 쪽이 먼저
        return left.candidate.insertText < right.candidate.insertText;
    };

    // 전체 인덱스는 무제한이지만 자동완성에는 상위 limit개만 필요하다. 모든
    // 일치 항목을 모아 정렬하면 큰 워크스페이스에서 GUI 스레드를 오래 점유하고
    // 메모리도 일치 개수만큼 늘어난다. 최악 후보가 위에 있는 제한 힙으로
    // 메모리를 O(limit), 후보 유지 비용을 O(log limit)으로 제한한다.
    using HeapStorage = std::vector< Scored >;
    std::priority_queue< Scored, HeapStorage, decltype( better ) > best( better );
    const QDir root( indexRoot );

    const qsizetype inspectCount = scanLimit > 0
                                       ? (std::min)( scanLimit, indexedPaths.size() )
                                       : indexedPaths.size();
    for( qsizetype pathIndex = 0; pathIndex < inspectCount; ++pathIndex )
    {
        const QString& relative = indexedPaths.at( pathIndex );
        const qsizetype separator = relative.lastIndexOf( QLatin1Char( '/' ) );
        const QString fileName = separator < 0 ? relative : relative.mid( separator + 1 );

        int score = 0;
        if( !rstcomplete::fuzzyMatchCompletion( typed.name, fileName, &score ) )
            continue;
        if( !acceptsFileName( fileName, *slot ) )
            continue;

        const QString absolute = root.absoluteFilePath( relative );
        if( slot->shape == Shape::DocName && !isWithin( query.sourceRoot, absolute ) )
            continue;

        Scored entry;
        entry.candidate.label =
            slot->shape == Shape::DocName ? stripDocumentSuffix( fileName ) : fileName;
        entry.candidate.insertText = insertTextFor( query, *slot, absolute, false );
        // 오른쪽 흐린 글씨에 상대 디렉터리를 둔다. 같은 이름이 여럿일 때 이것만이
        // 구분 수단이고, "여기 말고 다른 데" 라는 신호도 된다.
        entry.candidate.detail = separator < 0 ? QString{} : relative.left( separator );
        entry.candidate.kind = kindFor( fileName, false );
        entry.candidate.absolutePath = absolute;
        entry.score = score;
        entry.depth = static_cast< int >( relative.count( QLatin1Char( '/' ) ) );
        if( static_cast< int >( best.size() ) < limit )
            best.push( std::move( entry ) );
        else if( better( entry, best.top() ) )
        {
            best.pop();
            best.push( std::move( entry ) );
        }
    }

    QVector< Scored > scored;
    scored.reserve( static_cast< qsizetype >( best.size() ) );
    while( !best.empty() )
    {
        scored.push_back( best.top() );
        best.pop();
    }
    std::sort( scored.begin(), scored.end(), better );

    candidates.reserve( scored.size() );
    for( const Scored& entry : std::as_const( scored ) )
        candidates.push_back( entry.candidate );
    return candidates;
}

QVector< Candidate > mergeCandidates( QVector< Candidate > oneLevel,
                                      const QVector< Candidate >& global )
{
    QSet< QString > seen;
    seen.reserve( static_cast< qsizetype >( oneLevel.size() + global.size() ) );
    for( const Candidate& candidate : std::as_const( oneLevel ) )
        seen.insert( candidate.insertText );

    for( const Candidate& candidate : global )
    {
        if( seen.contains( candidate.insertText ) )
            continue;   // 눈앞의 디렉터리에 이미 있는 파일. 한 단계 후보가 이긴다
        seen.insert( candidate.insertText );
        oneLevel.push_back( candidate );
    }
    return oneLevel;
}

QVector< rstcomplete::Item > rebaseLspPathItems( QVector< rstcomplete::Item > items,
                                                 const Query& query )
{
    const Slot* slot = slotFor( query.context );
    if( slot == nullptr )
        return items;

    const QString directory = resolveTypedDirectory( query );
    if( directory.isEmpty() )
        return items;

    for( rstcomplete::Item& item : items )
    {
        // 이미 경로 형태로 온 항목(구분자가 있다)은 건드리지 않는다.
        if( item.label.contains( QLatin1Char( '/' ) ) || item.label.contains( QLatin1Char( '\\' ) ) )
            continue;

        const bool isDirectory = item.kind == 19;   // LSP CompletionItemKind::Folder
        const QString absolute = directory + QLatin1Char( '/' ) + item.label;
        item.insertText = insertTextFor( query, *slot, absolute, isDirectory );
        if( !isDirectory && filekinds::isImageFile( item.label ) )
            item.kind = rstcomplete::kKindImageFile;
    }
    return items;
}

}   // namespace hc::rst::rstpath
