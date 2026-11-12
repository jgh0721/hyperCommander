#include "stdafx.h"
#include "rstFileKinds.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst::filekinds {

namespace {

/// 제외 목록의 단일 원본. 두 접근자가 이걸 각자의 자료형으로 옮겨 담는다.
///
/// 네 곳에 흩어져 있던 목록의 **합집합**이다. 좁히지 않는다 — 어느 한 곳에서만
/// 빼 두었다는 것은 그때 그 자리에서 필요했다는 뜻이고, 이 목록에 들어 있는
/// 디렉터리는 어느 것도 사용자가 문서에서 참조할 만한 것이 아니다.
constexpr const char* kExcludedDirectories[] = {
    ".git", ".hg", ".svn", ".idea", ".vs", "__pycache__",
    "_build", "build", ".multiroot", ".venv", "venv", "env",
    "node_modules", ".tox", ".mypy_cache", ".pytest_cache", ".ruff_cache",
};

QStringList toList( std::initializer_list< const char* > names )
{
    QStringList list;
    list.reserve( static_cast< qsizetype >( names.size() ) );
    for( const char* name : names )
        list << QString::fromLatin1( name );
    return list;
}

}   // namespace

const QStringList& imageExtensions()
{
    // MainWindow 의 목록에 apng/avif 를 더했다. 둘 다 브라우저가 그리므로
    // 프리뷰에서 실제로 보이고, Sphinx 는 이미지 경로를 그대로 복사한다.
    static const QStringList extensions = toList( { "png", "jpg", "jpeg", "gif", "bmp", "tiff",
                                                   "tif", "ico", "webp", "svg", "apng", "avif" } );
    return extensions;
}

const QStringList& markdownExtensions()
{
    static const QStringList extensions = toList( { "md", "markdown", "mdown" } );
    return extensions;
}

const QStringList& restructuredTextExtensions()
{
    static const QStringList extensions = toList( { "rst", "rest" } );
    return extensions;
}

const QStringList& documentExtensions()
{
    // 순서를 지킨다. textLikeExtensions() 가 이 목록을 물려받아 경로 자동완성의
    // 후보 순위로 쓴다.
    static const QStringList extensions = [] {
        QStringList list = restructuredTextExtensions();
        list += toList( { "txt" } );
        list += markdownExtensions();
        return list;
    }();
    return extensions;
}

const QStringList& textLikeExtensions()
{
    static const QStringList extensions = [] {
        QStringList list = documentExtensions();
        list += toList( { "py", "toml", "json", "yaml", "yml", "cfg", "ini",
                         "c", "cc", "cpp", "cxx", "h", "hpp", "hxx", "cs", "java",
                         "js", "ts", "jsx", "tsx", "css", "html", "htm", "xml",
                         "sh", "bat", "cmd", "ps1", "sql", "csv", "tsv",
                         "dot", "gv", "rs", "go", "rb", "php", "lua", "cmake" } );
        return list;
    }();
    return extensions;
}

const QSet< QString >& excludedScanDirectories()
{
    static const QSet< QString > names = [] {
        QSet< QString > set;
        for( const char* name : kExcludedDirectories )
            set.insert( QString::fromLatin1( name ) );
        return set;
    }();
    return names;
}

const std::set< std::string >& excludedScanDirectoriesNarrow()
{
    static const std::set< std::string > names = [] {
        std::set< std::string > set;
        for( const char* name : kExcludedDirectories )
            set.insert( name );
        return set;
    }();
    return names;
}

bool hasExtension( const QString& path, const QStringList& extensions )
{
    return extensions.contains( QFileInfo( path ).suffix().toCaseFolded() );
}

bool isImageFile( const QString& path )
{
    return hasExtension( path, imageExtensions() );
}

bool isExcludedDirectoryName( const QString& name )
{
    return excludedScanDirectories().contains( name.toCaseFolded() );
}

bool isUnderExcludedDirectory( const QString& relativePath )
{
    const QStringList parts = relativePath.split( QLatin1Char( '/' ), Qt::SkipEmptyParts );
    // 마지막 성분은 파일 이름이다. 디렉터리 성분만 본다.
    for( qsizetype index = 0; index + 1 < parts.size(); ++index )
    {
        if( isExcludedDirectoryName( parts.at( index ) ) )
            return true;
    }
    return false;
}

}   // namespace hc::rst::filekinds
