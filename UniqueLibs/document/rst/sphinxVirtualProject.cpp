#include "stdafx.h"
#include "sphinxVirtualProject.hpp"

/// 모듈
#include "UniqueLibs/scratchSession.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

/// 가상 프로젝트용 최소 conf.py.
///
/// %1 = 프로젝트 표시 이름, %2 = root_doc(확장자 없는 파일명),
/// %3 = include_patterns 에 넣을 파일명, %4 = html_theme, %5 = 원본 확장자
///
/// myst_parser 는 있을 때만 켠다. 없는데 extensions 에 넣으면 빌드가 통째로
/// 실패한다.
constexpr auto kConfTemplate = R"PY(# HyperEditor 가 자동 생성한 파일입니다.
# conf.py 가 없는 단독 문서를 미리보기/진단하기 위한 임시 설정입니다.
project = "%1"
root_doc = "%2"
master_doc = root_doc

extensions = ["sphinx.ext.mathjax"]
try:
    import myst_parser  # noqa: F401
    extensions.append("myst_parser")
except ImportError:
    pass

source_suffix = {".rst": "restructuredtext", ".rest": "restructuredtext"}
if "myst_parser" in extensions:
    source_suffix[".md"] = "markdown"
    # 켜지 않으면 표 말고는 거의 아무것도 동작하지 않는다 — 취소선, 작업 목록,
    # $수식$, 정의 목록, `:::{note}` 컨테이너가 전부 꺼진 채로 렌더된다.
    # 단독 .md 문서와 .rst 에서 include 하는 Markdown 에 함께 적용한다.
    #
    # linkify 는 넣지 않는다. linkify-it-py 를 따로 요구하므로 그 패키지가 없는
    # 환경에서 빌드가 통째로 실패한다.
    myst_enable_extensions = [
        "strikethrough",
        "tasklist",
        "dollarmath",
        "deflist",
        "colon_fence",
    ]
    # strikethrough 는 HTML 출력에서만 동작한다는 경고를 낸다. 우리는 HTML 만
    # 만들므로 그 경고는 진단 표에 잡음일 뿐이다.
    suppress_warnings = ["myst.strikethrough"]

# 확장자의 대소문자가 달라도 원본 파일명을 그대로 빌드한다.
if "%5".lower() in source_suffix:
    source_suffix["%5"] = source_suffix["%5".lower()]

# 이 파일 하나만 읽는다. 그러지 않으면 같은 폴더의 무관한 문서까지 빌드된다.
include_patterns = ["%3"]
exclude_patterns = ["**/_build/**", "**/.git/**", "**/.venv/**"]

html_theme = "%4"
)PY";

/// ⚠ **번역 금지.** 이 값은 위 kConfTemplate 의 `project = "%1"` 자리에 그대로
///   들어가 파이썬 소스가 된다. 번역문에 따옴표나 역슬래시가 하나라도 있으면
///   conf.py 가 문법 오류가 되고 Sphinx 빌드가 통째로 실패한다. 화면에 보이는
///   문자열이 아니라 생성 코드의 일부다.
QString sanitizedProjectName( const QString& baseName )
{
    return baseName.isEmpty() ? QStringLiteral( "문서" ) : baseName;
}

/// 같은 이유로 테마 이름도 걸러 낸다. 이 값은 설정 파일에서 오고, 사용자가
/// ini 를 직접 고칠 수 있다. `alabaster"; import os` 한 줄이면 우리가 만든
/// conf.py 가 임의 코드를 실행한다 — Sphinx 는 conf.py 를 exec 한다.
QString sanitizedThemeName( const QString& theme )
{
    static const QRegularExpression allowed( QStringLiteral( "^[A-Za-z0-9._-]+$" ) );
    const QString trimmed = theme.trimmed();
    return allowed.match( trimmed ).hasMatch() ? trimmed : QStringLiteral( "alabaster" );
}

/// 프로세스의 임시 폴더( 소유 잠금, hc::scratch ) 안에 둔다. 강제 종료로 남은 것은 다음 실행의
/// 시작 정리가 소유 잠금으로 판정해 지운다 — PID 로 판정하면 권한이 다른 인스턴스나 재사용된 PID 를
/// 잘못 볼 수 있다. 예전 위치( %TEMP%\mrst-virtual-<pid>-* )는 같은 시작 정리가 하루 뒤에 지운다.
QString virtualDirTemplate()
{
    return hc::scratch::Session::Current().Template( QStringLiteral( "mrst-virtual" ) );
}

}  // namespace

VirtualProjectManager::VirtualProjectManager( QObject* parent )
    : QObject( parent )
{
}

VirtualProjectManager::~VirtualProjectManager()
{
    cleanup();
}

bool VirtualProjectManager::isSupported( const QString& filePath )
{
    const QString suffix = QFileInfo( filePath ).suffix().toLower();
    return suffix == QStringLiteral( "rst" ) || suffix == QStringLiteral( "rest" )
        || suffix == QStringLiteral( "md" );
}

QString VirtualProjectManager::projectIdFor( const QString& filePath )
{
    const QString canonical = QFileInfo( filePath ).absoluteFilePath().toCaseFolded();
    const QByteArray digest = QCryptographicHash::hash( canonical.toUtf8(), QCryptographicHash::Sha1 );
    return QStringLiteral( "virtual-" ) + QString::fromLatin1( digest.toHex().left( 12 ) );
}

const SphinxProject* VirtualProjectManager::findById( const QString& projectId ) const
{
    const auto it = handles_.constFind( projectId );
    return it == handles_.constEnd() ? nullptr : &it.value()->project;
}

const SphinxProject* VirtualProjectManager::projectFor( const QString& filePath )
{
    if( filePath.isEmpty() || !isSupported( filePath ) )
        return nullptr;

    const QFileInfo info( filePath );
    if( !info.isFile() )
        return nullptr;

    const QString projectId = projectIdFor( filePath );
    if( const SphinxProject* existing = findById( projectId ) )
        return existing;

    auto directory = std::make_unique< QTemporaryDir >( virtualDirTemplate() );
    if( !directory->isValid() )
    {
        emit logMessage( tr( "가상 프로젝트용 임시 디렉터리를 만들 수 없습니다." ) );
        return nullptr;
    }
    // 정상 종료나 테마 변경으로 이 관리자가 놓을 때 지운다. 비정상 종료로 남은 것은 위의 임시 폴더 정리가 맡는다.
    directory->setAutoRemove( true );

    const QString confDir = directory->path();
    const QString docName = info.completeBaseName();

    QFile conf( QDir( confDir ).filePath( QStringLiteral( "conf.py" ) ) );
    if( !conf.open( QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate ) )
    {
        emit logMessage( tr( "가상 프로젝트 conf.py 를 쓸 수 없습니다." ) );
        return nullptr;
    }
    conf.write( QString::fromUtf8( kConfTemplate )
                    .arg( sanitizedProjectName( docName ), docName, info.fileName(),
                          sanitizedThemeName( htmlTheme_ ), QStringLiteral( "." ) + info.suffix() )
                    .toUtf8() );
    conf.close();

    auto handle = std::make_shared< Handle >();
    handle->project.projectId = projectId.toStdWString();
    handle->project.confPath = toPath( QDir( confDir ).filePath( QStringLiteral( "conf.py" ) ) );
    // conf 는 임시 디렉터리, 소스는 원본이 있는 실제 디렉터리.
    handle->project.rootPath = toPath( info.absolutePath() );
    handle->project.sourcePath = toPath( info.absolutePath() );
    handle->project.rootDoc = docName.toStdString();
    // 빌드 산출물은 사용자 폴더가 아니라 임시 디렉터리에 둔다.
    handle->project.buildPath = toPath( QDir( confDir ).filePath( QStringLiteral( "build" ) ) );
    // 합성 conf.py 는 myst_parser 가 설치되어 있으면 켠다.
    // 방금 쓴 파일을 그대로 읽어 판정한다 — 템플릿과 판정이 갈라질 여지를 두지 않는다.
    handle->project.mystMarkdown = confDeclaresMystMarkdown( handle->project.confPath );
    handle->directory = std::move( directory );

    handles_.insert( projectId, handle );
    emit logMessage( tr( "가상 프로젝트 생성: %1 (%2)" ).arg( info.fileName(), projectId ) );
    return &handle->project;
}

void VirtualProjectManager::setHtmlTheme( const QString& theme )
{
    if( htmlTheme_ == theme )
        return;

    htmlTheme_ = theme;
    // 이미 만든 conf.py 는 다시 읽히지 않는다. 버려서 다음 요청에 새로 쓰게 한다.
    cleanup();
}

void VirtualProjectManager::cleanup()
{
    // QTemporaryDir 소멸자가 디렉터리를 지운다.
    handles_.clear();
}

}  // namespace hc::rst
