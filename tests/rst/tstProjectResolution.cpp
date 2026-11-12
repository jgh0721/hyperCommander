#include "rstTestRunner.hpp"

#include "sphinxScanner.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace hc::rst;

namespace {

/// conf.py 하나를 가진 프로젝트를 만든다.
void writeProject( const QString& directory, const QString& rootDoc )
{
    QDir().mkpath( directory );
    QFile conf( QDir( directory ).filePath( QStringLiteral( "conf.py" ) ) );
    QVERIFY2( conf.open( QIODevice::WriteOnly | QIODevice::Text ), "conf.py 를 쓸 수 없음" );
    conf.write( QStringLiteral( "project = 'x'\nroot_doc = '%1'\n" ).arg( rootDoc ).toUtf8() );
    conf.close();
}

void writeFile( const QString& path, const QByteArray& content )
{
    QDir().mkpath( QFileInfo( path ).absolutePath() );
    QFile file( path );
    QVERIFY2( file.open( QIODevice::WriteOnly ), "파일을 쓸 수 없음" );
    file.write( content );
    file.close();
}

}  // namespace

class TestProjectResolution : public QObject
{
    Q_OBJECT

private slots:
    void scansUnicodePaths_data();
    void scansUnicodePaths();
    void excludesUnicodeDirectoryNames();
    void resolvesFileInSameDirectory();
    /// Windows 에서 사용자가 대소문자가 다른 경로로 워크스페이스를 열 수 있다
    /// (예: Docs/Source vs docs/source). 그때도 같은 프로젝트로 해석돼야 한다.
    void resolvesDespiteCaseDifference();
    void prefersNearestProject();
    void returnsNullForUnrelatedFile();
    void detectsEmptyHtmlStyle();
    /// `.md` 를 Sphinx 로 보낼지 내장 렌더러로 보낼지 가르는 1차 판정이다.
    /// 거짓양성은 빌더 리포트가 정정하지만 거짓음성은 정정할 수단이 없다.
    void detectsMystMarkdown();
    /// 가상 프로젝트의 "다른 프로젝트와 동일" 테마가 이 판정 하나에 걸려 있다.
    void readsHtmlTheme();
};

void TestProjectResolution::scansUnicodePaths_data()
{
    QTest::addColumn< QString >( "name" );
    QTest::addColumn< QString >( "location" );
    QTest::addColumn< int >( "projectCount" );

    const QStringList names{
        QStringLiteral( "workspace" ), QStringLiteral( "한글 작업공간" ),
        QStringLiteral( "漢字工作區" ), QStringLiteral( "日本語ワークスペース" ),
        QStringLiteral( "한글_漢字_日本語_😀" ),
    };
    const QStringList locations{
        QStringLiteral( "root" ), QStringLiteral( "ancestor" ), QStringLiteral( "child" ),
    };
    for( qsizetype index = 0; index < names.size(); ++index )
    {
        for( const QString& location : locations )
        {
            for( int count : { 1, 2 } )
            {
                const QByteArray row = QStringLiteral( "%1-%2-%3" )
                    .arg( index ).arg( location ).arg( count ).toLatin1();
                QTest::newRow( row.constData() ) << names.at( index ) << location << count;
            }
        }
    }
}

void TestProjectResolution::scansUnicodePaths()
{
    QFETCH( QString, name );
    QFETCH( QString, location );
    QFETCH( int, projectCount );

    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    const QString workspace = QDir( temp.path() ).filePath(
        location == QStringLiteral( "root" ) ? name
        : location == QStringLiteral( "ancestor" ) ? name + QStringLiteral( "/workspace" )
        : QStringLiteral( "workspace" ) );
    QStringList roots;
    QStringList files;
    // 두 프로젝트가 있어야 scan() 의 정렬 비교자가 실행된다.
    for( int index = 0; index < projectCount; ++index )
    {
        const QString directory = QStringLiteral( "project-%1" ).arg( projectCount - index );
        const QString root = QDir( workspace ).filePath(
            location == QStringLiteral( "child" ) ? name + QLatin1Char( '/' ) + directory : directory );
        writeProject( root, QStringLiteral( "index" ) );
        const QString file = QDir( root ).filePath( QStringLiteral( "page.rst" ) );
        writeFile( file, "T\n=\n" );
        roots.append( root );
        files.append( file );
    }
    // 기존 제외 디렉터리도 유니코드 경로 아래에서 계속 제외해야 한다.
    writeProject( QDir( workspace ).filePath( QStringLiteral( "_BUILD/ignored" ) ),
                  QStringLiteral( "index" ) );

    std::vector< SphinxProject > projects;
    try
    {
        projects = ProjectScanner( toPath( workspace ) ).scan();
    }
    catch( const std::exception& error )
    {
        QFAIL( error.what() );
    }
    QCOMPARE( projects.size(), static_cast< std::size_t >( projectCount ) );
    for( qsizetype index = 0; index < files.size(); ++index )
    {
        const SphinxProject* resolved = resolveProjectForFile( toPath( files.at( index ) ), projects );
        QVERIFY( resolved != nullptr );
        QCOMPARE( QDir::fromNativeSeparators( toQString( resolved->rootPath ) ), roots.at( index ) );
        QCOMPARE( QDir::fromNativeSeparators( toQString( resolved->confPath ) ),
                  QDir( roots.at( index ) ).filePath( QStringLiteral( "conf.py" ) ) );
        QCOMPARE( QString::fromStdWString( resolved->projectId ),
                  location == QStringLiteral( "child" )
                      ? name + QStringLiteral( ".project-%1" ).arg( projectCount - index )
                      : QStringLiteral( "project-%1" ).arg( projectCount - index ) );
    }
    // 생성 순서와 관계없이 project-1, project-2 순으로 반환해야 한다.
    QCOMPARE( QDir::fromNativeSeparators( toQString( projects.front().rootPath ) ), roots.constLast() );
    QCOMPARE( QDir::fromNativeSeparators( toQString( projects.back().rootPath ) ), roots.constFirst() );
}

void TestProjectResolution::excludesUnicodeDirectoryNames()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );
    const QString workspace = QDir( temp.path() ).filePath( QStringLiteral( "한글_漢字_日本語_😀" ) );
    writeProject( workspace, QStringLiteral( "index" ) );
    const QString excluded = QStringLiteral( "日本語_제외_Ä" );
    writeProject( QDir( workspace ).filePath( excluded.toLower() ), QStringLiteral( "index" ) );
    writeProject( QDir( workspace ).filePath( QStringLiteral( "문서" ) ), QStringLiteral( "index" ) );
    ScannerSettings settings;
    settings.excludedDirs.push_back( excluded.toStdString() );

    const auto projects = ProjectScanner( toPath( workspace ), settings ).scan();
    QCOMPARE( projects.size(), std::size_t( 2 ) );
    for( const auto& project : projects )
        QVERIFY( toQString( project.rootPath.filename() ) != excluded.toLower() );
    const auto* rootProject = resolveProjectForFile(
        toPath( QDir( workspace ).filePath( QStringLiteral( "conf.py" ) ) ), projects );
    QVERIFY( rootProject != nullptr );
    QCOMPARE( QString::fromStdWString( rootProject->projectId ), QFileInfo( workspace ).fileName() );
}

void TestProjectResolution::resolvesFileInSameDirectory()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString source = QDir( temp.path() ).filePath( QStringLiteral( "docs/source" ) );
    writeProject( source, QStringLiteral( "index" ) );
    const QString file = QDir( source ).filePath( QStringLiteral( "client.rst" ) );
    writeFile( file, "T\n=\n" );

    const std::vector< SphinxProject > projects = ProjectScanner( toPath( source ) ).scan();
    QCOMPARE( projects.size(), std::size_t( 1 ) );

    const SphinxProject* resolved = resolveProjectForFile( toPath( file ), projects );
    QVERIFY2( resolved != nullptr, "같은 디렉터리의 파일이 프로젝트로 해석되지 않음" );
}

void TestProjectResolution::resolvesDespiteCaseDifference()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString source = QDir( temp.path() ).filePath( QStringLiteral( "docs/source" ) );
    writeProject( source, QStringLiteral( "index" ) );
    writeFile( QDir( source ).filePath( QStringLiteral( "client.rst" ) ), "T\n=\n" );

    const std::vector< SphinxProject > projects = ProjectScanner( toPath( source ) ).scan();
    QCOMPARE( projects.size(), std::size_t( 1 ) );

    // 사용자가 대문자로 입력/선택한 경로. Windows 에서는 같은 파일을 가리킨다.
    const QString upperCased = QDir( temp.path() ).filePath( QStringLiteral( "Docs/Source/client.rst" ) );
    const SphinxProject* resolved = resolveProjectForFile( toPath( upperCased ), projects );

#ifdef Q_OS_WIN
    QVERIFY2( resolved != nullptr,
             "대소문자만 다른 경로가 같은 프로젝트로 해석되지 않음 "
             "(Windows 에서는 같은 파일이다)" );
#else
    Q_UNUSED( resolved );
#endif
}

void TestProjectResolution::prefersNearestProject()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString outer = QDir( temp.path() ).filePath( QStringLiteral( "outer" ) );
    const QString inner = QDir( outer ).filePath( QStringLiteral( "nested" ) );
    writeProject( outer, QStringLiteral( "index" ) );
    writeProject( inner, QStringLiteral( "index" ) );

    const QString file = QDir( inner ).filePath( QStringLiteral( "page.rst" ) );
    writeFile( file, "T\n=\n" );

    const std::vector< SphinxProject > projects = ProjectScanner( toPath( outer ) ).scan();
    QCOMPARE( projects.size(), std::size_t( 2 ) );

    const SphinxProject* resolved = resolveProjectForFile( toPath( file ), projects );
    QVERIFY( resolved != nullptr );
    // toQString 은 네이티브 구분자(\), QFileInfo 는 /. 비교 전에 맞춘다.
    QCOMPARE( QDir::fromNativeSeparators( toQString( resolved->rootPath ) ),
             QFileInfo( inner ).absoluteFilePath() );
}

void TestProjectResolution::returnsNullForUnrelatedFile()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString source = QDir( temp.path() ).filePath( QStringLiteral( "docs" ) );
    writeProject( source, QStringLiteral( "index" ) );

    const QString outside = QDir( temp.path() ).filePath( QStringLiteral( "elsewhere/orphan.rst" ) );
    writeFile( outside, "T\n=\n" );

    const std::vector< SphinxProject > projects = ProjectScanner( toPath( source ) ).scan();
    QVERIFY( resolveProjectForFile( toPath( outside ), projects ) == nullptr );
}

void TestProjectResolution::detectsEmptyHtmlStyle()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString path = QDir( temp.path() ).filePath( QStringLiteral( "conf.py" ) );
    writeFile( path, "project = 'x'\nhtml_style = ''\n" );
    QVERIFY( confDeclaresEmptyHtmlStyle( toPath( path ) ) );

    writeFile( path, "project = 'x'\nhtml_style = 'custom.css'\n" );
    QVERIFY( !confDeclaresEmptyHtmlStyle( toPath( path ) ) );

    // 마지막 대입이 이긴다.
    writeFile( path, "html_style = ''\nhtml_style = 'later.css'\n" );
    QVERIFY( !confDeclaresEmptyHtmlStyle( toPath( path ) ) );

    // BOM 으로 시작하는 conf.py( Sphinx 는 utf-8-sig 로 읽는다 ): 첫 줄 대입도 보인다.
    writeFile( path, "\xEF\xBB\xBFhtml_style = ''\nproject = 'x'\n" );
    QVERIFY( confDeclaresEmptyHtmlStyle( toPath( path ) ) );
}

void TestProjectResolution::detectsMystMarkdown()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString path = QDir( temp.path() ).filePath( QStringLiteral( "conf.py" ) );
    const auto declares = [ &path ]( const char* body ) {
        writeFile( path, body );
        return confDeclaresMystMarkdown( toPath( path ) );
    };

    // extensions 에 실린 myst 계열.
    QVERIFY( declares( "extensions = ['myst_parser']\n" ) );
    QVERIFY( declares( "extensions = [\n    'sphinx.ext.autodoc',\n    'myst_parser',\n]\n" ) );
    QVERIFY( declares( "extensions = ['myst_nb']\n" ) );

    // source_suffix 로만 등록한 경우(recommonmark 나 커스텀 파서). extensions 에
    // myst 가 없어도 .md 는 원본이다.
    QVERIFY( declares( "source_suffix = {'.rst': 'restructuredtext', '.md': 'markdown'}\n" ) );
    QVERIFY( declares( "source_suffix = ['.rst', '.markdown']\n" ) );

    // 켜져 있지 않은 경우.
    QVERIFY( !declares( "extensions = ['sphinx.ext.autodoc']\n" ) );
    QVERIFY( !declares( "source_suffix = '.rst'\n" ) );
    QVERIFY( !declares( "project = 'x'\n" ) );

    // 주석은 세지 않는다. conf.py 에는 껐다 켠 흔적이 자주 남는다.
    QVERIFY( !declares( "# extensions = ['myst_parser']\n" ) );
    QVERIFY( !declares( "extensions = []  # 'myst_parser' 는 나중에\n" ) );

    // exclude_patterns 의 글롭은 걸리지 않아야 한다 — 따옴표로 정확히 감싼
    // '.md' 만 본다.
    QVERIFY( !declares( "exclude_patterns = ['**/*.md', '_build']\n" ) );

    // 파일이 없으면 거짓이다(스캔 중 지워진 경우).
    QVERIFY( !confDeclaresMystMarkdown( toPath( QDir( temp.path() ).filePath(
        QStringLiteral( "없는파일.py" ) ) ) ) );
}

void TestProjectResolution::readsHtmlTheme()
{
    QTemporaryDir temp;
    QVERIFY( temp.isValid() );

    const QString path = QDir( temp.path() ).filePath( QStringLiteral( "conf.py" ) );
    const auto theme = [ &path ]( const char* body ) {
        writeFile( path, body );
        return QString::fromStdString( readHtmlTheme( toPath( path ) ) );
    };

    QCOMPARE( theme( "html_theme = 'furo'\n" ), QStringLiteral( "furo" ) );
    QCOMPARE( theme( "project = 'x'\nhtml_theme = \"sphinx_rtd_theme\"\n" ),
             QStringLiteral( "sphinx_rtd_theme" ) );
    // 타입 힌트가 붙어 있어도 잡는다 (html_style 판정과 같은 문법을 받는다).
    QCOMPARE( theme( "html_theme: str = 'alabaster'\n" ), QStringLiteral( "alabaster" ) );

    // 마지막 대입이 이긴다.
    QCOMPARE( theme( "html_theme = 'alabaster'\nhtml_theme = 'furo'\n" ), QStringLiteral( "furo" ) );

    // 주석은 세지 않는다. 테마를 바꿔 보다 주석으로 남긴 줄이 흔하다.
    QVERIFY( theme( "# html_theme = 'furo'\n" ).isEmpty() );

    // 선언이 없으면 빈 문자열. 호출 측이 alabaster 로 물러선다.
    QVERIFY( theme( "project = 'x'\n" ).isEmpty() );

    // 변수 대입은 잡지 못한다 — 정규식 폴백의 알려진 한계다.
    QVERIFY( theme( "THEME = 'furo'\nhtml_theme = THEME\n" ).isEmpty() );

    // 파일이 없으면 빈 문자열(스캔 중 지워진 경우).
    QVERIFY( readHtmlTheme( toPath( QDir( temp.path() ).filePath(
        QStringLiteral( "없는파일.py" ) ) ) ).empty() );
}

HC_RST_REGISTER_TEST( TestProjectResolution );

#include "tstProjectResolution.moc"
