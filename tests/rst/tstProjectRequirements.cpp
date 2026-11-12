#include "rstTestRunner.hpp"

#include "pythonEnvResolver.hpp"
#include "rstLineUtils.hpp"
#include "sphinxProjectRequirements.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace hc::rst;

namespace
{

bool put( const QString& path, const QByteArray& bytes )
{
    QDir().mkpath( QFileInfo( path ).absolutePath() );
    QFile file( path );
    return file.open( QIODevice::WriteOnly ) && file.write( bytes ) == bytes.size();
}

/// 가상환경처럼 보이는 폴더: pyvenv.cfg 와 site-packages 의 dist-info.
bool makeVenv( const QString& venv, const QList< QPair< QString, QString > >& distributions )
{
    if( !put( venv + QStringLiteral( "/pyvenv.cfg" ), "home = C:\\Python\nversion = 3.12.7\n" ) )
        return false;
    for( const auto& [ stem, topLevel ] : distributions )
    {
        const QString info = venv + QStringLiteral( "/Lib/site-packages/" ) + stem + QStringLiteral( ".dist-info" );
        if( !put( info + QStringLiteral( "/METADATA" ), "Metadata-Version: 2.1\n" ) )
            return false;
        if( !topLevel.isEmpty() && !put( info + QStringLiteral( "/top_level.txt" ), topLevel.toUtf8() ) )
            return false;
    }
    return true;
}

}  // namespace

/// Sphinx 는 언제나 HyperEditor 환경에서 돈다. 프로젝트의 가상환경과 선언 파일은 테마 · 확장 요구를 읽는
/// 데만 쓰고, 그 내용이 설치 명령의 옵션이나 다른 출처로 흘러가면 안 된다.
class TestProjectRequirements : public QObject
{
    Q_OBJECT

private slots:
    void namesAreNormalised();
    void requirementsKeepNameExtrasAndVersions_data();
    void requirementsKeepNameExtrasAndVersions();
    void requirementsThatAreNotPlainPackagesAreDropped_data();
    void requirementsThatAreNotPlainPackagesAreDropped();
    void pyprojectArraysAreRead();
    void requirementsTextIsRead();
    void declaredSpecsWinOverInstalledVersions();
    void nearerDeclarationWins();
    void searchStopsAtRepositoryRoot();
    void modulesMapToDistributions();
    void missingPackagesFollowTheProject();
    void resolverAlwaysUsesTheProductEnvironment();
    void filesWithBomReadLikeFilesWithout();
};

void TestProjectRequirements::namesAreNormalised()
{
    QCOMPARE( normalizedDistributionName( QStringLiteral( "Sphinx_RTD.Theme" ) ), QStringLiteral( "sphinx-rtd-theme" ) );
    QCOMPARE( normalizedDistributionName( QStringLiteral( "  myst--parser " ) ), QStringLiteral( "myst-parser" ) );
    QCOMPARE( requirementName( QStringLiteral( "furo>=2024.1" ) ), QStringLiteral( "furo" ) );
    QCOMPARE( requirementName( QStringLiteral( "sphinx-tabs[extra]==3.4.5" ) ), QStringLiteral( "sphinx-tabs" ) );
}

void TestProjectRequirements::requirementsKeepNameExtrasAndVersions_data()
{
    QTest::addColumn< QString >( "line" );
    QTest::addColumn< QString >( "expected" );
    QTest::newRow( "이름만" ) << "furo" << "furo";
    QTest::newRow( "고정 버전" ) << "sphinx-tabs==3.4.5" << "sphinx-tabs==3.4.5";
    QTest::newRow( "공백" ) << "  breathe  >=  4.35 , < 5 " << "breathe>=4.35,<5";
    QTest::newRow( "extras" ) << "myst-parser[linkify] ~= 3.0" << "myst-parser[linkify]~=3.0";
    QTest::newRow( "환경 표식" ) << "sphinx-design>=0.5; python_version >= \"3.9\"" << "sphinx-design>=0.5";
    QTest::newRow( "주석" ) << "sphinx-copybutton==0.5.2  # 복사 단추" << "sphinx-copybutton==0.5.2";
    QTest::newRow( "와일드카드" ) << "sphinx==8.*" << "sphinx==8.*";
}

void TestProjectRequirements::requirementsKeepNameExtrasAndVersions()
{
    QFETCH( QString, line );
    QFETCH( QString, expected );
    QCOMPARE( sanitizedRequirement( line ), expected );
}

void TestProjectRequirements::requirementsThatAreNotPlainPackagesAreDropped_data()
{
    QTest::addColumn< QString >( "line" );
    QTest::newRow( "빈 줄" ) << "";
    QTest::newRow( "주석 줄" ) << "# docs";
    QTest::newRow( "다른 파일" ) << "-r base.txt";
    QTest::newRow( "색인 주소" ) << "--index-url https://example.invalid/simple";
    QTest::newRow( "옵션처럼 보이는 이름" ) << "--pre";
    QTest::newRow( "편집 설치" ) << "-e .";
    QTest::newRow( "URL" ) << "https://example.invalid/pkg.whl";
    QTest::newRow( "git" ) << "git+https://example.invalid/repo.git";
    QTest::newRow( "이름 @ 주소" ) << "pkg @ https://example.invalid/pkg.whl";
    QTest::newRow( "경로" ) << "./local/package";
    QTest::newRow( "윈도 경로" ) << "C:\\packages\\pkg.whl";
    QTest::newRow( "공백으로 덧붙인 옵션" ) << "furo --index-url https://example.invalid";
    QTest::newRow( "셸 문자" ) << "furo; rm -rf x && echo";
}

void TestProjectRequirements::requirementsThatAreNotPlainPackagesAreDropped()
{
    QFETCH( QString, line );
    const QString out = sanitizedRequirement( line );
    // 마지막 줄( 환경 표식 자리의 셸 문자 )은 이름만 남는다. 그 밖에는 통째로 버려진다.
    if( line.startsWith( QLatin1String( "furo;" ) ) )
        QCOMPARE( out, QStringLiteral( "furo" ) );
    else
        QVERIFY2( out.isEmpty(), qPrintable( out ) );
    // 어떤 경우에도 옵션이나 공백이 설치 인자로 나가지 않는다.
    QVERIFY( !out.startsWith( QLatin1Char( '-' ) ) );
    QVERIFY( !out.contains( QLatin1Char( ' ' ) ) );
}

void TestProjectRequirements::pyprojectArraysAreRead()
{
    const QString text = QStringLiteral( R"TOML([build-system]
requires = ["hatchling"]

[project]
name = "demo"
dependencies = [
    "requests>=2",        # 실행 의존성
    'click',
]
keywords = ["not-a-dependency"]

[project.optional-dependencies]
docs = ["sphinx>=7", "furo==2024.8.6", "pkg @ https://example.invalid/pkg.whl"]
test = [
    "pytest",
]

[dependency-groups]
"doc" = ["sphinx-tabs[extra]>=3.4", "--index-url https://example.invalid"]

[tool.other]
dependencies = ["ignored"]
)TOML" );
    const QStringList found = requirementsFromPyproject( text );
    QCOMPARE( found, ( QStringList{ QStringLiteral( "requests>=2" ), QStringLiteral( "click" ), QStringLiteral( "sphinx>=7" ),
                                    QStringLiteral( "furo==2024.8.6" ), QStringLiteral( "pytest" ),
                                    QStringLiteral( "sphinx-tabs[extra]>=3.4" ) } ) );
}

void TestProjectRequirements::requirementsTextIsRead()
{
    const QStringList found = requirementsFromRequirementsText(
        QStringLiteral( "# 문서\r\nsphinx==8.2.3\r\n\r\n-r other.txt\r\nbreathe>=4.35\r\nhttps://example.invalid/x.whl\r\n" ) );
    QCOMPARE( found, ( QStringList{ QStringLiteral( "sphinx==8.2.3" ), QStringLiteral( "breathe>=4.35" ) } ) );
}

void TestProjectRequirements::declaredSpecsWinOverInstalledVersions()
{
    QTemporaryDir root;
    QVERIFY( root.isValid() );
    QVERIFY( put( root.filePath( "requirements.txt" ), "furo>=2024.1\n" ) );
    QVERIFY( makeVenv( root.filePath( ".venv" ), { { "furo-2023.9.10", "furo\n" }, { "sphinx_tabs-3.4.5", "sphinx_tabs\n" } } ) );

    const ProjectRequirements found = readProjectRequirements( root.path(), root.filePath( ".venv" ) );
    // 선언한 조건이 설치된 버전보다 앞선다. 선언이 없는 것은 설치된 버전으로 고정한다.
    QCOMPARE( found.specFor( QStringLiteral( "furo" ) ), QStringLiteral( "furo>=2024.1" ) );
    QCOMPARE( found.specFor( QStringLiteral( "sphinx-tabs" ) ), QStringLiteral( "sphinx_tabs==3.4.5" ) );
    QCOMPARE( found.sources.size(), 2 );
}

void TestProjectRequirements::nearerDeclarationWins()
{
    QTemporaryDir root;
    QVERIFY( root.isValid() );
    QVERIFY( put( root.filePath( "pyproject.toml" ), "[project]\ndependencies = [\"furo==1.0\", \"breathe==4.0\"]\n" ) );
    QVERIFY( put( root.filePath( "docs/requirements.txt" ), "furo==2.0\n" ) );
    QVERIFY( QDir().mkpath( root.filePath( "docs/source" ) ) );

    // 프로젝트( docs/source )에서 위로: docs/requirements.txt 가 저장소 뿌리의 pyproject.toml 보다 가깝다.
    const ProjectRequirements found = readProjectRequirements( root.filePath( "docs/source" ), {} );
    QCOMPARE( found.specFor( QStringLiteral( "furo" ) ), QStringLiteral( "furo==2.0" ) );
    QCOMPARE( found.specFor( QStringLiteral( "breathe" ) ), QStringLiteral( "breathe==4.0" ) );
}

void TestProjectRequirements::searchStopsAtRepositoryRoot()
{
    QTemporaryDir root;
    QVERIFY( root.isValid() );
    QVERIFY( put( root.filePath( "requirements.txt" ), "outside==9.9\n" ) );
    QVERIFY( put( root.filePath( "repo/.git/HEAD" ), "ref: refs/heads/main\n" ) );
    QVERIFY( put( root.filePath( "repo/requirements.txt" ), "inside==1.0\n" ) );
    QVERIFY( QDir().mkpath( root.filePath( "repo/docs" ) ) );

    const ProjectRequirements found = readProjectRequirements( root.filePath( "repo/docs" ), {} );
    QCOMPARE( found.specFor( QStringLiteral( "inside" ) ), QStringLiteral( "inside==1.0" ) );
    // 저장소 밖의 파일은 이 프로젝트의 것이 아니다.
    QVERIFY( found.specFor( QStringLiteral( "outside" ) ).isEmpty() );
}

void TestProjectRequirements::modulesMapToDistributions()
{
    QTemporaryDir root;
    QVERIFY( root.isValid() );
    QVERIFY( makeVenv( root.filePath( "venv" ), { { "sphinx_rtd_theme-3.0.2", "sphinx_rtd_theme\n" },
                                                    { "sphinxcontrib_mermaid-1.0.0", "sphinxcontrib\n" },
                                                    { "sphinxcontrib_bibtex-2.6.3", "sphinxcontrib\n" },
                                                    { "pillow-11.0.0", "PIL\n" },
                                                    { "myst_parser-4.0.0", QString() } } ) );
    const ProjectRequirements found = readProjectRequirements( root.path(), root.filePath( "venv" ) );
    // 모듈 이름( import 이름 )으로도 찾는다. 점이 있으면 첫 마디를 본다.
    QCOMPARE( found.specFor( QStringLiteral( "sphinx_rtd_theme" ) ), QStringLiteral( "sphinx_rtd_theme==3.0.2" ) );
    QCOMPARE( found.specFor( QStringLiteral( "sphinxcontrib.mermaid" ) ), QStringLiteral( "sphinxcontrib_mermaid==1.0.0" ) );
    // top_level.txt 가 없으면 배포명에서 모듈 이름을 추정한다.
    QCOMPARE( found.specFor( QStringLiteral( "myst_parser" ) ), QStringLiteral( "myst_parser==4.0.0" ) );
    // 배포명과 모듈 이름이 다르면 top_level.txt 로 찾는다.
    QCOMPARE( found.specFor( QStringLiteral( "PIL" ) ), QStringLiteral( "pillow==11.0.0" ) );
    // 여러 배포판이 함께 쓰는 네임스페이스로는 고르지 않는다: 이름이 맞는 것만 찾는다.
    QCOMPARE( found.specFor( QStringLiteral( "sphinxcontrib.bibtex" ) ), QStringLiteral( "sphinxcontrib_bibtex==2.6.3" ) );
    QVERIFY( found.specFor( QStringLiteral( "sphinxcontrib.other" ) ).isEmpty() );
    QVERIFY( found.specFor( QStringLiteral( "unknown_extension" ) ).isEmpty() );
}

void TestProjectRequirements::missingPackagesFollowTheProject()
{
    ProjectRequirements requirements;
    requirements.specs.insert( QStringLiteral( "sphinx-tabs" ), QStringLiteral( "sphinx-tabs==3.4.5" ) );
    requirements.specs.insert( QStringLiteral( "furo" ), QStringLiteral( "furo>=2024.1" ) );
    requirements.modules.insert( QStringLiteral( "furo" ), QStringLiteral( "furo" ) );

    // 빌드가 알린 배포판 · 테마 이름 -> 프로젝트가 밝힌 요구. 모르는 것은 이름만, 같은 것은 한 번만.
    const QStringList out = resolveMissingPackages(
        requirements, { QStringLiteral( "sphinx-tabs" ), QStringLiteral( "breathe" ), QStringLiteral( "Sphinx_Tabs" ) },
        { QStringLiteral( "furo" ), QStringLiteral( "sphinx_rtd_theme" ) } );
    QCOMPARE( out, ( QStringList{ QStringLiteral( "sphinx-tabs==3.4.5" ), QStringLiteral( "breathe" ), QStringLiteral( "furo>=2024.1" ),
                                  QStringLiteral( "sphinx-rtd-theme" ) } ) );
    // 이름이 패키지 이름 모양이 아니면 설치 목록에 넣지 않는다.
    QVERIFY( resolveMissingPackages( {}, { QStringLiteral( "--index-url" ), QStringLiteral( "a b" ) }, {} ).isEmpty() );
}

void TestProjectRequirements::resolverAlwaysUsesTheProductEnvironment()
{
    QTemporaryDir root;
    QVERIFY( root.isValid() );
    QVERIFY( put( root.filePath( "docs/conf.py" ), "project = 'demo'\n" ) );
    QVERIFY( makeVenv( root.filePath( ".venv" ), { { "furo-2024.8.6", "furo\n" } } ) );
    // 실행할 수 있는 것처럼 보이는 인터프리터가 있어도 쓰지 않는다.
    QVERIFY( put( root.filePath( ".venv/Scripts/python.exe" ), "MZ" ) );

    SphinxProject project;
    project.projectId = L"demo-test";
    project.rootPath = std::filesystem::path( root.filePath( "docs" ).toStdWString() );
    project.confPath = project.rootPath / L"conf.py";
    project.sourcePath = project.rootPath;

    PythonEnvResolver resolver( nullptr );
    QStringList log;
    connect( &resolver, &PythonEnvResolver::logMessage, this, [&log]( const QString& text ) { log << text; } );
    const ResolvedPythonEnv resolved = resolver.resolve( project );
    QVERIFY( resolved.isBundled() );
    QVERIFY( !resolved.pythonExe.contains( QLatin1String( ".venv" ) ) );
    // 프로젝트 가상환경은 찾되, 요구를 읽는 데만 쓴다.
    QCOMPARE( QDir::cleanPath( resolver.projectVenvDir( project ) ), QDir::cleanPath( QFileInfo( root.filePath( ".venv" ) ).canonicalFilePath() ) );
    QCOMPARE( resolver.requirements( project ).specFor( QStringLiteral( "furo" ) ), QStringLiteral( "furo==2024.8.6" ) );
    QVERIFY( !log.isEmpty() );
}

void TestProjectRequirements::filesWithBomReadLikeFilesWithout()
{
    // conf.py · .rst · pyproject.toml · requirements 는 BOM 이 있든 없든 같게 읽힌다. BOM 이 첫 글자로 남으면
    // 첫 줄의 [project] 나 첫 요구가 보이지 않는다.
    QCOMPARE( hc::rst::rstline::fromUtf8File( QByteArray( "\xEF\xBB\xBF[project]" ) ), QStringLiteral( "[project]" ) );
    QCOMPARE( hc::rst::rstline::fromUtf8File( QByteArray( "[project]" ) ), QStringLiteral( "[project]" ) );
    QCOMPARE( hc::rst::rstline::fromUtf8File( QByteArray( "\xEF\xBB\xBF" ) ), QString() );
    QCOMPARE( hc::rst::rstline::fromUtf8File( QByteArray( "\xEF\xBB\xBF\xED\x95\x9C" ) ), QString::fromUtf8( "\xED\x95\x9C" ) );

    for( const bool Bom : { false, true } )
    {
        QTemporaryDir root;
        QVERIFY( root.isValid() );
        const QByteArray Prefix = Bom ? QByteArray( "\xEF\xBB\xBF" ) : QByteArray();
        QVERIFY( put( root.filePath( "pyproject.toml" ), Prefix + "[project]\ndependencies = [\"furo>=2024.1\"]\n" ) );
        QVERIFY( put( root.filePath( "requirements.txt" ), Prefix + "breathe>=4.36\n" ) );
        const ProjectRequirements found = readProjectRequirements( root.path(), {} );
        QCOMPARE( found.specFor( QStringLiteral( "furo" ) ), QStringLiteral( "furo>=2024.1" ) );
        QCOMPARE( found.specFor( QStringLiteral( "breathe" ) ), QStringLiteral( "breathe>=4.36" ) );
    }
}

HC_RST_REGISTER_TEST( TestProjectRequirements );

#include "tstProjectRequirements.moc"
