#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QRawFont>
#include <QSvgRenderer>
#include <oclero/qlementine/resources/ResourceInitialization.hpp>
#include <oclero/qlementine/style/QlementineStyle.hpp>

class QlementineTests final : public QObject
{
    Q_OBJECT
private slots:
    void staticResources()
    {
        oclero::qlementine::resources::initializeResources();
        for( const auto* Name : { "light", "dark" } )
        {
            const auto Path = QString( ":/qlementine/resources/themes/%1.json" ).arg( Name );
            QVERIFY( oclero::qlementine::Theme::fromJsonPath( Path ).has_value() );
        }
        QSvgRenderer Renderer( QStringLiteral( ":/qlementine/resources/icons/messagebox_information_fg.svg" ) );
        QVERIFY( Renderer.isValid() );
        const QStringList Fonts{
            "inter/Inter-Regular.ttf", "inter/InterDisplay-Regular.ttf", "roboto-mono/RobotoMono-Regular.ttf" };
        for( const auto& Font : Fonts )
        {
            QFile File( ":/qlementine/resources/fonts/" + Font );
            QVERIFY2( File.open( QIODevice::ReadOnly ), qPrintable( File.fileName() ) );
            const auto Bytes = File.readAll();
            QRawFont RawFont( Bytes, 14 );
            QVERIFY( RawFont.isValid() );
            QVERIFY( !RawFont.familyName().isEmpty() );
        }
    }

    void staticStyle()
    {
        // Exercise symbols, generated export header, moc, and rcc from the
        // product library; the application chooses its final theme in P3.
        oclero::qlementine::QlementineStyle Style;
        Style.setAnimationsEnabled( false );
        Style.setTheme( oclero::qlementine::Theme::makeDark() );
        QCOMPARE( Style.theme().meta.name, oclero::qlementine::Theme::makeDark().meta.name );
        Style.setTheme( oclero::qlementine::Theme::makeLight() );
        QVERIFY( Style.metaObject()->className() );
    }
};

QTEST_MAIN( QlementineTests )
#include "tstQlementine.moc"
