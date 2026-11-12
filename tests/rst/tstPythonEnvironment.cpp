#include "rstTestRunner.hpp"

#include "pythonEnvHealth.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace hc::rst;

class TestPythonEnvironment : public QObject
{
    Q_OBJECT

private slots:
    void missingBasePythonIsDamaged();
    void missingVenvLauncherIsDamaged();
    void existingBasePythonIsAccepted();
    void windowsLauncherFailureIsRecognised_data();
    void windowsLauncherFailureIsRecognised();
};

namespace
{
    bool writeVenvConfig( const QString& venvDir, const QString& home )
    {
        if( !QDir().mkpath( venvDir ) )
            return false;
        QFile file( QDir( venvDir ).filePath( QStringLiteral( "pyvenv.cfg" ) ) );
        if( !file.open( QIODevice::WriteOnly | QIODevice::Text ) )
            return false;
        file.write( QStringLiteral( "home = %1\nversion_info = 3.11.13\n" ).arg( home ).toUtf8() );
        return true;
    }
}

void TestPythonEnvironment::missingBasePythonIsDamaged()
{
    QTemporaryDir temporary;
    QVERIFY( temporary.isValid() );

    const QString venvDir = QDir( temporary.path() ).filePath( QStringLiteral( ".venv" ) );
    const QString missingHome = QDir( temporary.path() ).filePath( QStringLiteral( "missing-python" ) );
    QVERIFY( writeVenvConfig( venvDir, QStringLiteral( "\"%1\"" ).arg( missingHome ) ) );
#ifdef Q_OS_WIN
    const QString launcherPath = QDir( venvDir ).filePath( QStringLiteral( "Scripts/python.exe" ) );
#else
    const QString launcherPath = QDir( venvDir ).filePath( QStringLiteral( "bin/python" ) );
#endif
    QVERIFY( QDir().mkpath( QFileInfo( launcherPath ).absolutePath() ) );
    QFile launcher( launcherPath );
    QVERIFY( launcher.open( QIODevice::WriteOnly ) );
    launcher.close();

#ifdef Q_OS_WIN
    const QString reason = pythonVenvDamageReason( venvDir );
    QVERIFY2( !reason.isEmpty(), qPrintable( reason ) );
    QVERIFY( reason.contains( QDir::toNativeSeparators( missingHome ) ) );
#else
    QVERIFY( pythonVenvDamageReason( venvDir ).isEmpty() );
#endif
}

void TestPythonEnvironment::missingVenvLauncherIsDamaged()
{
    QTemporaryDir temporary;
    QVERIFY( temporary.isValid() );

    const QString baseDir = QDir( temporary.path() ).filePath( QStringLiteral( "base-python" ) );
    QVERIFY( QDir().mkpath( baseDir ) );
#ifdef Q_OS_WIN
    QFile basePython( QDir( baseDir ).filePath( QStringLiteral( "python.exe" ) ) );
    QVERIFY( basePython.open( QIODevice::WriteOnly ) );
    basePython.close();
#endif

    const QString venvDir = QDir( temporary.path() ).filePath( QStringLiteral( ".venv" ) );
    QVERIFY( writeVenvConfig( venvDir, baseDir ) );
    const QString reason = pythonVenvDamageReason( venvDir );
    QVERIFY2( !reason.isEmpty(), qPrintable( reason ) );
    QVERIFY( reason.contains( QStringLiteral( "python" ), Qt::CaseInsensitive ) );
}

void TestPythonEnvironment::existingBasePythonIsAccepted()
{
    QTemporaryDir temporary;
    QVERIFY( temporary.isValid() );

    const QString baseDir = QDir( temporary.path() ).filePath( QStringLiteral( "base-python" ) );
    QVERIFY( QDir().mkpath( baseDir ) );
#ifdef Q_OS_WIN
    QFile python( QDir( baseDir ).filePath( QStringLiteral( "python.exe" ) ) );
    QVERIFY( python.open( QIODevice::WriteOnly ) );
    python.close();
#endif

    const QString venvDir = QDir( temporary.path() ).filePath( QStringLiteral( ".venv" ) );
    QVERIFY( writeVenvConfig( venvDir, baseDir ) );
#ifdef Q_OS_WIN
    const QString launcherPath = QDir( venvDir ).filePath( QStringLiteral( "Scripts/python.exe" ) );
#else
    const QString launcherPath = QDir( venvDir ).filePath( QStringLiteral( "bin/python" ) );
#endif
    QVERIFY( QDir().mkpath( QFileInfo( launcherPath ).absolutePath() ) );
    QFile launcher( launcherPath );
    QVERIFY( launcher.open( QIODevice::WriteOnly ) );
    launcher.close();
    QVERIFY( pythonVenvDamageReason( venvDir ).isEmpty() );
}

void TestPythonEnvironment::windowsLauncherFailureIsRecognised_data()
{
    QTest::addColumn< int >( "exitCode" );
    QTest::addColumn< bool >( "crashed" );
    QTest::addColumn< QString >( "output" );
    QTest::addColumn< bool >( "broken" );

    QTest::newRow( "missing-base-message" )
        << 103 << false << QStringLiteral( "No Python at 'C:/missing/python.exe'" ) << true;
#ifdef Q_OS_WIN
    QTest::newRow( "windows-venvlauncher-code" )
        << 103 << false << QString{} << true;
#endif
    QTest::newRow( "missing-sphinx" )
        << 4 << false << QStringLiteral( "No module named sphinx" ) << false;
    QTest::newRow( "sphinx-build-error" )
        << 1 << false << QStringLiteral( "Sphinx error" ) << false;
}

void TestPythonEnvironment::windowsLauncherFailureIsRecognised()
{
    QFETCH( int, exitCode );
    QFETCH( bool, crashed );
    QFETCH( QString, output );
    QFETCH( bool, broken );

    QCOMPARE( pythonFailureIndicatesBrokenEnvironment( exitCode, crashed, output ), broken );
}

HC_RST_REGISTER_TEST( TestPythonEnvironment );

#include "tstPythonEnvironment.moc"
