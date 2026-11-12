#include "rstTestRunner.hpp"
#include "sphinxVirtualProject.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TestVirtualProject : public QObject
{
    Q_OBJECT
private slots:
    void standaloneDocuments_data();
    void standaloneDocuments();
};

void TestVirtualProject::standaloneDocuments_data()
{
    QTest::addColumn<QString>( "name" );
    QTest::addColumn<bool>( "supported" );
    QTest::newRow( "rst" ) << QString( "guide.rst" ) << true;
    QTest::newRow( "markdown" ) << QString( "guide.md" ) << true;
    QTest::newRow( "uppercase" ) << QString( "guide.MD" ) << true;
    QTest::newRow( "text" ) << QString( "notes.txt" ) << false;
    QTest::newRow( "other" ) << QString( "settings.ini" ) << false;
}

void TestVirtualProject::standaloneDocuments()
{
    QFETCH( QString, name );
    QFETCH( bool, supported );
    QTemporaryDir directory;
    QVERIFY( directory.isValid() );
    const QString path = directory.filePath( name );
    QFile document( path );
    QVERIFY( document.open( QIODevice::WriteOnly ) );
    document.write( "Document\n========\n" );
    document.close();
    hc::rst::VirtualProjectManager manager;
    QCOMPARE( manager.isSupported( path ), supported );
    const auto* project = manager.projectFor( path );
    QCOMPARE( project != nullptr, supported );
    if( !supported )
        return;
    QCOMPARE( manager.projectFor( path ), project );
    QCOMPARE( project->sourcePath, hc::rst::toPath( directory.path() ) );
    QVERIFY( project->mystMarkdown );
    const QString confPath = QString::fromStdWString( project->confPath.wstring() );
    QVERIFY( !confPath.startsWith( directory.path() + "/" ) );
    QFile conf( confPath );
    QVERIFY( conf.open( QIODevice::ReadOnly ) );
    const QByteArray contents = conf.readAll();
    QVERIFY( contents.contains( QByteArray( "include_patterns = [\"" ) + name.toUtf8() + "\"]" ) );
    QVERIFY( contents.contains( "root_doc = \"guide\"" ) );
    conf.close();
    manager.cleanup();
    QVERIFY( !QFile::exists( confPath ) );
    QVERIFY( QFile::exists( path ) );
}

HC_RST_REGISTER_TEST( TestVirtualProject );
#include "tstVirtualProject.moc"
