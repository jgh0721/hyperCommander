#include "UniqueLibs/settings/appSettings.hpp"
#include "HyperEditor/editorInstance.hpp"
#include "UniqueLibs/productTheme.hpp"
#include <future>
#include <QUuid>
#include "UniqueLibs/document/documentSettingsDialog.hpp"
#include "tools/epub/render/readingSettings.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "text/QTextView.h"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/epubSourceEditor.hpp"
#include "UniqueLibs/document/epubLineShifts.hpp"
#include "text/ColumnRulerWidget.h"
#include <QClipboard>
#include <QSaveFile>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolBar>
#include <QMenu>
#include <QMenuBar>
#include <QStyleOptionSlider>
#include <fmstyle/ThemeManager.h>
#include <fmstyle/PaperFluentStyle.h>
#include "thirdparty/scintilla-qt/PlatQtTextRendering.hpp"
#include "core/ExternalChangeWatcher.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include <QWindow>
#include <QScreen>
#include "text/ITextEditorBackend.h"
#include <QApplication>
#include "UniqueLibs/document/documentShortcutRegistry.hpp"
#include "text/FindReplaceWidget.h"
#include <QTabWidget>
#include <QInputMethodEvent>
#include <QMessageBox>
#include <QAction>
#include "text/ScintillaQtDirectBackend.h"
#include "text/TextLexerRegistry.h"
#include <Scintilla.h>
#include <SciLexer.h>
#include <ILexer.h>
#include <Lexilla.h>
#include <QJsonDocument>
#include <QTableWidget>
#include <QLineEdit>
#include <QSet>
#include <QCheckBox>
#include <QComboBox>
#include <QFontComboBox>
#include <QKeySequenceEdit>
#include <QListWidget>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QtTest>
using namespace hc::settings;
using namespace hc::document;
class DocumentPreferencesTests : public QObject {
    Q_OBJECT
    static QByteArray Read(const QString& Path) { QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return {}; return File.readAll(); }
    static void Write(const QString& Path, const QByteArray& Bytes) { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Bytes), Bytes.size()); }
private slots:
    void completeLexerCatalogAndDetection()
    {
        const auto& Registry = TextLexerRegistry::instance();
        QVERIFY(Registry.loadedFromJson()); QSet<QString> Keys;
        for (const auto& Name : Registry.displayNames()) Keys.insert(Registry.lexerKeyForDisplayName(Name));
        for (int Index = 0; Index < GetLexerCount(); ++Index) {
            char Name[128]{}; GetLexerName(Index, Name, sizeof(Name));
            QVERIFY2(Keys.contains(QString::fromLatin1(Name)) || QByteArray(Name) == "null", Name);
        }
        QWidget Host; ScintillaQtDirectBackend Backend(&Host);
        for (const auto& Name : Registry.displayNames()) QVERIFY2(Backend.applyLanguage(Name), qPrintable(Name));
        const QList<QPair<QString, QString>> Samples{
            {"build.CMD", "batch"}, {"build.bat", "batch"}, {"test.pyw", "python"}, {"test.rs", "rust"},
            {"app.go", "cpp"}, {"site.php", "phpscript"}, {"Gemfile", "ruby"}, {"settings.ini", "props"},
            {"settings.toml", "toml"}, {"test.md", "markdown"}, {"test.ps1", "powershell"}, {"test.nim", "nim"},
            {"CMakeLists.txt", "cmake"}, {"Makefile", "makefile"}, {".bashrc", "bash"}, {"test.vcxproj", "xml"},
            {"test.zig", "zig"}, {"test.unknown-extension", "none"}
        };
        for (const auto& [Path, Lexer] : Samples)
            QCOMPARE(Registry.lexerKeyForDisplayName(Registry.languageForFileName(Path)), Lexer);
        qInfo() << "Lexilla catalog:" << GetLexerCount() << "lexers," << Registry.displayNames().size() << "language entries";
    }
    void additionalLexerStyles_data()
    {
        QTest::addColumn<QString>("Language"); QTest::addColumn<QByteArray>("Source"); QTest::addColumn<QByteArray>("Keyword"); QTest::addColumn<int>("Style");
        QTest::newRow("yaml") << QString("YAML") << QByteArray("ready: true\n") << QByteArray("true") << SCE_YAML_KEYWORD;
        QTest::newRow("powershell") << QString("Powershell") << QByteArray("function Example { return 42 }\n") << QByteArray("function") << SCE_POWERSHELL_KEYWORD;
        QTest::newRow("rust") << QString("Rust") << QByteArray("fn main() { let x = 42; }\n") << QByteArray("fn") << SCE_RUST_WORD;
        QTest::newRow("php") << QString("PHP") << QByteArray("echo \"sample\";\n") << QByteArray("echo") << SCE_HPHP_WORD;
        QTest::newRow("lua") << QString("Lua") << QByteArray("local answer = 42\n") << QByteArray("local") << SCE_LUA_WORD;
    }
    void additionalLexerStyles()
    {
        QFETCH(QString, Language); QFETCH(QByteArray, Source); QFETCH(QByteArray, Keyword); QFETCH(int, Style);
        QWidget Host; ScintillaQtDirectBackend Backend(&Host); Backend.setText(QString::fromUtf8(Source));
        QVERIFY(Backend.applyLanguage(Language)); Backend.Send(SCI_COLOURISE, 0, -1);
        QCOMPARE(Backend.Send(SCI_GETSTYLEAT, Source.indexOf(Keyword)), qintptr(Style));
        QVERIFY(Backend.Send(SCI_STYLEGETFORE, Style) != Backend.Send(SCI_STYLEGETFORE, STYLE_DEFAULT));
    }
    void batchHighlightingAndLanguageSwitch()
    {
        QWidget Host; ScintillaQtDirectBackend Backend(&Host);
        const QByteArray Source("@echo off\r\nrem Build script\r\nSET Tools=%~dp0Tools\r\nif exist \"%Tools%\" echo Ready\r\n");
        Backend.setText(QString::fromUtf8(Source)); QVERIFY(Backend.applyLanguage("Batch")); Backend.Send(SCI_COLOURISE, 0, -1);
        QCOMPARE(Backend.Send(SCI_GETLEXER), qintptr(SCLEX_BATCH));
        QCOMPARE(Backend.Send(SCI_GETSTYLEAT, Source.indexOf("SET")), qintptr(SCE_BAT_WORD));
        QCOMPARE(Backend.Send(SCI_GETSTYLEAT, Source.indexOf("rem")), qintptr(SCE_BAT_COMMENT));
        QCOMPARE(Backend.Send(SCI_GETSTYLEAT, Source.indexOf("%~dp0")), qintptr(SCE_BAT_IDENTIFIER));
        const auto Fore = Backend.Send(SCI_STYLEGETFORE, STYLE_DEFAULT);
        QVERIFY(Backend.Send(SCI_STYLEGETFORE, SCE_BAT_WORD) != Fore);
        QVERIFY(Backend.Send(SCI_STYLEGETFORE, SCE_BAT_COMMENT) != Fore);
        QVERIFY(Backend.Send(SCI_STYLEGETFORE, SCE_BAT_IDENTIFIER) != Fore);
        QVERIFY(Backend.applyLanguage("C++")); QVERIFY(Backend.applyLanguage("Batch"));
        QVERIFY(Backend.Send(SCI_STYLEGETFORE, SCE_BAT_COMMENT) != Fore);
        QVERIFY(Backend.applyLanguage("None"));
        QCOMPARE(Backend.Send(SCI_STYLEGETFORE, SCE_BAT_WORD), Backend.Send(SCI_STYLEGETFORE, STYLE_DEFAULT));
        QCOMPARE(Backend.Send(SCI_GETSTYLEAT, Source.indexOf("SET")), qintptr(0));
        QTextView Text; auto* Toolbar = Text.createToolBar();
        Text.LoadContent(QString::fromUtf8(Source), "compile.CMD", "UTF-8");
        QCOMPARE(Text.currentLanguage(), QString("Batch"));
        QCOMPARE(Toolbar->findChild<QComboBox*>("textLanguage")->currentText(), QString("Batch"));
        Text.setLanguage("Python"); QCOMPARE(Toolbar->findChild<QComboBox*>("textLanguage")->currentText(), QString("Python"));
    }
    void textColorTabsAndPersistence()
    {
        QTemporaryDir Temp; const auto Previous = qApp->property("documentViewsConfigDir");
        qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([&] { qApp->setProperty("documentViewsConfigDir", Previous); ThemeManager::instance().ReloadColorOverrides(); });
        const auto Path = UnifiedConfig::FileName(Temp.path());
        Write(Path, QJsonDocument(QJsonObject{{"schemaVersion", 2}}).toJson()); const auto Before = Read(Path);
        DocumentSettingsDialog Dialog(Path, {});
        auto* Tabs = Dialog.findChild<QTabWidget*>("textViewerTabs"); QVERIFY(Tabs); QCOMPARE(Tabs->count(), 2);
        QCOMPARE(Tabs->tabText(0), QString::fromUtf8("공통")); QCOMPARE(Tabs->tabText(1), QString::fromUtf8("색상"));
        Dialog.findChild<QListWidget*>("settingsCategories")->setCurrentRow(1); Tabs->setCurrentIndex(1);
        auto* Mode = Dialog.findChild<QComboBox*>("textColorTheme"); auto* Language = Dialog.findChild<QComboBox*>("textColorLanguage");
        Mode->setCurrentIndex(Mode->findData("dark")); Language->setCurrentIndex(Language->findData("Batch"));
        auto* Keyword = Dialog.findChild<QLineEdit*>("textColor/text.lexer.batch.keyword"); QVERIFY(Keyword); Keyword->setText("#123456");
        Language->setCurrentIndex(0);
        auto* Background = Dialog.findChild<QLineEdit*>("textColor/text.background"); QVERIFY(Background); Background->setText("#101820");
        QCOMPARE(Read(Path), Before); QVERIFY(Dialog.Apply());
        const auto Config = QJsonDocument::fromJson(Read(Path)).object();
        QCOMPARE(UnifiedConfig::Value(Config, {"documentViews", "textView", "colors", "dark", "text.lexer.batch.keyword"}).toString(), QString("#123456"));
        QVERIFY(UnifiedConfig::Value(Config, {"documentViews", "textView", "colors", "light", "text.lexer.batch.keyword"}).isUndefined());
        ThemeManager::instance().ReloadColorOverrides(); const auto OldTheme = ThemeManager::instance().currentTheme();
        const auto ThemeRestore = qScopeGuard([&] { ThemeManager::instance().setTheme(OldTheme); });
        ThemeManager::instance().setTheme(ThemeManager::Dark);
        QCOMPARE(ThemeManager::instance().color("text.background"), QColor("#101820"));
        QWidget Host; ScintillaQtDirectBackend Backend(&Host); QVERIFY(Backend.applyLanguage("Batch"));
        QCOMPARE(Backend.Send(SCI_STYLEGETFORE, SCE_BAT_WORD), qintptr(0x563412));
        const auto Capture = qEnvironmentVariable("HC_TEXT_COLORS_CAPTURE");
        if (!Capture.isEmpty()) {
            Dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&Dialog)); QDir().mkpath(Capture);
            QVERIFY(Dialog.grab().save(Capture + "/text-colors-common.png"));
            Language->setCurrentIndex(Language->findData("Batch")); QCoreApplication::processEvents();
            QVERIFY(Dialog.grab().save(Capture + "/text-colors-batch.png"));
        }
        Mode->setCurrentIndex(Mode->findData("dark")); Language->setCurrentIndex(Language->findData("Batch"));
        Dialog.findChild<QLineEdit*>("textColor/text.lexer.batch.keyword")->clear(); QVERIFY(Dialog.Apply());
        ThemeManager::instance().ReloadColorOverrides(); QVERIFY(!ThemeManager::instance().hasColorOverride("text.lexer.batch.keyword"));
        Language->setCurrentIndex(0); Dialog.findChild<QLineEdit*>("textColor/text.background")->setText("#zzzzzz"); QVERIFY(!Dialog.Apply());
    }
    void textSelectionRemainsReadable_data()
    {
        QTest::addColumn<bool>("Hosted"); QTest::addColumn<QString>("Extension");
        for (bool Hosted : {true, false}) for (const auto& Extension : {QString("txt"), QString("cmd"), QString("cpp")})
            QTest::newRow(qPrintable(QString("%1-%2").arg(Hosted ? "viewer" : "editor", Extension))) << Hosted << Extension;
    }
    void textSelectionRemainsReadable()
    {
        QFETCH(bool, Hosted); QFETCH(QString, Extension);
        QTemporaryDir Temp; const auto Previous = qApp->property("documentViewsConfigDir");
        qApp->setProperty("documentViewsConfigDir", Temp.path());
        auto& Theme = fm::style::ThemeManager::instance(); const auto OldScheme = Theme.scheme();
        const auto OldMask = mrst::scintilla::textMaskState().enabled;
        const auto OldGamma = mrst::scintilla::textMaskState().gamma;
        const auto OldStrength = mrst::scintilla::textMaskState().strength;
        const auto Restore = qScopeGuard([&] {
            Theme.setScheme(OldScheme); qApp->setProperty("documentViewsConfigDir", Previous);
            mrst::scintilla::setTextMask(OldMask, OldGamma, OldStrength);
        });
        const QByteArray Content = QString::fromUtf8("ECHO Approve Subnet Route at Admin Console!!! 한글\n\n// Selected comment MMMM 한글\n").toUtf8();
        const auto Path = Temp.filePath("selection." + Extension); Write(Path, Content);
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"), Hosted);
        QVERIFY(!Window.Handle({{"action", "open"}, {"path", Path}}).contains("error")); QTRY_VERIFY(Window.IsIdle());
        auto* Text = Window.findChild<QTextView*>(); QVERIFY(Text);
        auto* Backend = static_cast<ScintillaQtDirectBackend*>(Text->Backend());
        auto* Surface = Backend->scrollArea()->viewport();
        Window.resize(1000, 550); Window.show(); QVERIFY(QTest::qWaitForWindowExposed(&Window));
        qInfo() << "Selection rendering DPR:" << Surface->devicePixelRatioF();
        Backend->Send(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE); Backend->Send(SCI_SETCARETPERIOD, 0);
        const int ThirdLine = int(Content.indexOf("//"));
        for (const auto& Mode : {QString("dark"), QString("light"), QString("dark")}) {
            QVERIFY(Window.ApplyChromeSettings({{"schemaVersion", 1}, {"themeMode", Mode}}));
            Backend->Send(SCI_SETSEL, 0, Content.indexOf('\n'));
            Backend->Send(SCI_ADDSELECTION, Content.size() - 1, ThirdLine);
            auto MatchColor = Theme.colors()[fm::style::Token::Warn]; MatchColor.setAlpha(110);
            Backend->setIndicatorStyle(21, INDIC_ROUNDBOX, MatchColor);
            Backend->applyIndicator(21, 0, Content.indexOf('\n'));
            for (bool Mask : {false, true}) for (bool Focus : {true, false}) {
                mrst::scintilla::setTextMask(Mask, 1.2, 1.1); Backend->Send(SCI_SETFOCUS, Focus);
                Surface->update(); QCoreApplication::processEvents();
                const auto Image = Surface->grab().toImage(); const auto Dpr = Image.devicePixelRatio();
                const auto Background = ThemeManager::instance().color("text.selection");
                for (int Start : {0, ThirdLine}) {
                    const int X = Backend->Send(SCI_POINTXFROMPOSITION, 0, Start);
                    const int Y = Backend->Send(SCI_POINTYFROMPOSITION, 0, Start);
                    const int End = Backend->Send(SCI_POINTXFROMPOSITION, 0, Content.indexOf('\n', Start));
                    const int Height = Backend->Send(SCI_TEXTHEIGHT, Backend->Send(SCI_LINEFROMPOSITION, Start));
                    const auto Rect = QRect(qCeil((X + 2) * Dpr), qCeil((Y + 2) * Dpr),
                        qFloor((End - X - 4) * Dpr), qFloor((Height - 4) * Dpr)).intersected(Image.rect());
                    int Ink = 0;
                    for (int Row = Rect.top(); Row <= Rect.bottom(); ++Row)
                        for (int Col = Rect.left(); Col <= Rect.right(); ++Col)
                            if (fm::style::contrastRatio(Image.pixelColor(Col, Row), Background) >= 4.5) ++Ink;
                    const auto Capture = qEnvironmentVariable("HC_DOCUMENT_UI_CAPTURE");
                    if (!Capture.isEmpty()) {
                        QDir().mkpath(Capture);
                        QVERIFY(Window.grab().save(Capture + QString("/selection-%1-%2-%3-mask%4-focus%5.png")
                            .arg(Hosted ? "viewer" : "editor", Extension, Mode).arg(Mask).arg(Focus)));
                    }
                    QVERIFY2(Ink > 100, qPrintable(QString("%1 line=%2 mask=%3 focus=%4 readable pixels=%5")
                        .arg(Mode).arg(Start).arg(Mask).arg(Focus).arg(Ink)));
                }
                QCOMPARE(Backend->Send(SCI_GETSELECTIONLAYER), qintptr(SC_LAYER_UNDER_TEXT));
                QVERIFY(Backend->Send(SCI_INDICGETUNDER, 21));
                QCOMPARE(Backend->Send(SCI_GETSELECTIONS), qintptr(2));
                QCOMPARE(Text->Content().toUtf8(), Content); QVERIFY(!Text->isModified());
            }
            Text->ReloadPreferences();
        }
    }
    void textThemeColorContrast()
    {
        fm::style::ThemeManager::instance().setDesign(hc::theme::ProductDesign);
        for (const auto Mode : {ThemeManager::Light, ThemeManager::Dark}) {
            const auto Colors = ThemeManager::defaultColors(Mode);
            for (const auto& Pair : {qMakePair("text.foreground", "text.background"),
                    qMakePair("text.foreground", "text.currentLine"), qMakePair("text.selectionForeground", "text.selection"),
                    qMakePair("text.marginForeground", "text.marginBackground"), qMakePair("text.caret", "text.background")}) {
                const auto Ratio = fm::style::contrastRatio(Colors[Pair.first], Colors[Pair.second]);
                QVERIFY2(Ratio >= 4.5, qPrintable(QString("%1 %2/%3=%4").arg(Mode).arg(Pair.first, Pair.second).arg(Ratio)));
            }
            for (auto It = Colors.cbegin(); It != Colors.cend(); ++It) {
                if (!It.key().startsWith("text.lexer.")) continue;
                for (const auto* Background : {"text.background", "text.currentLine"}) {
                    const auto Ratio = fm::style::contrastRatio(It.value(), Colors[Background]);
                    QVERIFY2(Ratio >= 4.5, qPrintable(QString("%1 %2/%3=%4")
                        .arg(Mode).arg(It.key(), Background).arg(Ratio)));
                }
            }
        }
    }
    void loadingImagePreviewPreservesDocumentAndClearsOnLoad() {
        DocumentViewPane Pane;TextInput Text;Text.path_="preview.txt";Text.text_="keep me";Text.bytes_="keep me";
        QVERIFY(Pane.Load(Text,false));Pane.SetReadOnly(true);const auto Before=Pane.Content();
        Pane.resize(400,300);Pane.show();QImage Image(100,120,QImage::Format_RGB32);Image.fill(Qt::red);
        Pane.ShowLoadingPreview(Image);auto* Preview=Pane.findChild<QWidget*>("documentLoadingPreview");
        QVERIFY(Preview);QVERIFY(!Preview->isHidden());QVERIFY(Pane.IsReadOnly());QCOMPARE(Pane.Content(),Before);
        Pane.resize(500,350);QCOMPARE(Preview->size(),Pane.size());
        Pane.Cancel();QVERIFY(Preview->isHidden());Pane.ShowLoadingPreview(Image);
        QVERIFY(Pane.Load(Text,false));QVERIFY(Preview->isHidden());QCOMPARE(Pane.Content(),Before);
    }

    void editorInstanceForwardsAndIsolatesSettings() {
        const auto Scope=QUuid::createUuid().toByteArray();EditorInstance Primary;QStringList Received;
        QCOMPARE(Primary.Start(Scope,{}),EditorInstance::Primary);
        Primary.SetOpenHandler([&](QStringList Files){Received=Files;});
        const auto Path=QDir::temp().absoluteFilePath("editor-instance.txt");
        auto Client=std::async(std::launch::async,[=]{EditorInstance Secondary;return Secondary.Start(Scope,{Path});});
        QTRY_VERIFY_WITH_TIMEOUT(Client.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,15000);
        QCOMPARE(Client.get(),EditorInstance::Forwarded);QTRY_COMPARE(Received,QStringList{Path});
        EditorInstance Other;QCOMPARE(Other.Start(Scope+"other-settings",{}),EditorInstance::Primary);
    }

    void deferredCppStylingPreservesMultilineState() {
        const QByteArray Unit("/* comment\nstill comment */\nint value = 3;\n");
        const auto Bytes=Unit.repeated(2048);QTextView View;View.resize(700,400);
        View.LoadContent(QString::fromUtf8(Bytes),"large.cpp","UTF-8",Bytes,"LF");View.show();
        auto* Backend=static_cast<ScintillaQtDirectBackend*>(View.Backend());QVERIFY(Backend);
        QTRY_COMPARE(Backend->Send(SCI_GETSTYLEAT,Bytes.indexOf("still")),qintptr(1));
        Backend->setCursorPosition(6143,0);Backend->scrollRangeToView(Bytes.size()-2,Bytes.size()-2);
        QTRY_COMPARE_WITH_TIMEOUT(Backend->Send(SCI_GETSTYLEAT,Bytes.lastIndexOf("int")),qintptr(5),10000);
        auto Settings=ScintillaEditorSettings::standard();Settings.tabWidth=7;Backend->applySettings(Settings);
        QCOMPARE(Backend->Send(SCI_GETSTYLEAT,Bytes.lastIndexOf("int")),qintptr(5));
        QCOMPARE(View.Content().toUtf8(),Bytes);QVERIFY(!View.isModified());
    }

    void epubGlobalSettingsApplyAndCancel() {
        QTemporaryDir Temp;const auto Path=Temp.filePath("HyperEditor.json");
        using hc::epub::poc::ReadingSettings;using hc::epub::poc::ReadingStore;
        {DocumentSettingsDialog Dialog(Path,{});auto* Panel=Dialog.findChild<ReadingSettings*>("epubGlobalSettings");QVERIFY(Panel);QTRY_VERIFY(!Panel->IsBusy());
            QVERIFY(Panel->Execute("preview",{{"line",2.4},{"layout","paginated"}},true));QVERIFY(Dialog.Apply());
            QVERIFY(Panel->Execute("preview",{{"line",2.8}},true));Dialog.reject();}
        const auto Stored=ReadingStore::Load(Temp.filePath("epub-reading.json"),QString(64,'a'));QVERIFY(Stored.error.isEmpty());QCOMPARE(Stored.global["line"].toDouble(),2.4);QVERIFY(Stored.book.isEmpty());
    }
    void epubSourceStartsWithTextFont() {
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        const auto Restore=qScopeGuard([]{qApp->setProperty("documentViewsConfigDir",QVariant());});
        {::AppSettings Settings;Settings.setValue("textView/fontFamily","Consolas");Settings.setValue("textView/fontSize",19);}
        // The editor opens before any settings Apply, so only its own construction can deliver the font.
        EpubSourceEditor Editor({},Temp.filePath("book.epub"));Editor.SetRecoveryEnabled(false);
        const auto Font=Editor.SourceView()->editorFont();QCOMPARE(Font.family(),QString("Consolas"));QCOMPARE(Font.pointSize(),19);
        auto* Backend=static_cast<ScintillaQtDirectBackend*>(Editor.SourceView()->Backend());QCOMPARE(Backend->Send(SCI_STYLEGETSIZE,STYLE_DEFAULT),qintptr(19));
    }
    void fractionalViewportRoundTrip_data() {
        QTest::addColumn<int>("Wrap");QTest::addColumn<int>("Extra");QTest::addColumn<bool>("Fold");
        for(int Wrap:{0,1})for(int Extra:{0,8})QTest::newRow(qPrintable(QString("wrap%1-extra%2").arg(Wrap).arg(Extra)))<<Wrap<<Extra<<false;
        QTest::newRow("folded")<<0<<0<<true;
    }
    void fractionalViewportRoundTrip() {
        QFETCH(int,Wrap);QFETCH(int,Extra);QFETCH(bool,Fold);
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        const auto Restore=qScopeGuard([]{qApp->setProperty("documentViewsConfigDir",QVariant());});
        // 30 groups of ten lines; every third paragraph wraps into several rows when wrapping is on.
        QString Text="<doc>\n";for(int G=0;G<30;++G){Text+=QString("<g n=\"%1\">\n").arg(G);for(int P=0;P<8;++P)Text+="<p>"+QString(P%3==0?400:40,QChar('a'+P))+"</p>\n";Text+="</g>\n";}Text+="</doc>\n";
        QTextView View;View.LoadContent(Text,Temp.filePath("rows.xml"),"UTF-8");View.resize(600,400);View.show();QVERIFY(QTest::qWaitForWindowExposed(&View));
        auto* Backend=static_cast<ScintillaQtDirectBackend*>(View.Backend());
        // Earlier tests can leave a tiny application font; a fixed editor font keeps every target reachable.
        QFont Font("Consolas");Font.setPointSize(12);Backend->setEditorFont(Font);
        View.SetWordWrapMode(Wrap);Backend->Send(SCI_SETEXTRAASCENT,Extra);Backend->Send(SCI_SETEXTRADESCENT,Extra);
        // XML tags fold only with fold.html; the product leaves it off, the fold geometry is what matters here.
        if(Fold){Backend->Send(SCI_SETPROPERTY,reinterpret_cast<quintptr>("fold.html"),reinterpret_cast<qintptr>("1"));Backend->Send(SCI_COLOURISE,0,-1);Backend->Send(SCI_FOLDLINE,121,SC_FOLDACTION_CONTRACT);QVERIFY(!Backend->Send(SCI_GETLINEVISIBLE,125));}
        // Styling and layout continue at idle after loading; measure the settled view.
        QTest::qWait(300);
        for(const double Line:Fold?QList<double>{126.3}:QList<double>{37.0,120.25,180.6})for(const double Ratio:{0.25,0.5}) {
            Backend->scrollFractionalLineToViewportRatio(Line,Ratio);QCoreApplication::processEvents();
            // SCI_LINESCROLL moves whole display rows, so a line's fraction lands within half a row.
            const int Target=Fold?122:int(Line);const auto Rows=qMax<qintptr>(1,Backend->Send(SCI_WRAPCOUNT,Target-1));
            const double Expected=Fold?122.0:Line,Read=Backend->fractionalLineAtViewportRatio(Ratio);
            QVERIFY2(qAbs(Read-Expected)<=0.5/Rows+0.02,qPrintable(QString("line %1 ratio %2 rows %3 read %4 viewport %5 row %6 first %7 onscreen %8 lines %9")
                .arg(Line).arg(Ratio).arg(Rows).arg(Read).arg(Backend->scrollArea()->viewport()->height()).arg(Backend->textHeight(Target-1)).arg(Backend->Send(SCI_GETFIRSTVISIBLELINE)).arg(Backend->Send(SCI_LINESONSCREEN)).arg(Backend->lineCount())));
        }
    }
    void lineShiftMapping() {
        // Three lines inserted after 0-based line 9 (editor lines 11-13 are new), then two removed after line 19.
        const QList<LineShift> Shifts{{9,3},{19,-2}};
        QCOMPARE(ToRevisionLine(5.5,Shifts),5.5);QCOMPARE(ToRevisionLine(12.0,Shifts),10.0);
        QCOMPARE(ToRevisionLine(15.25,Shifts),12.25);QCOMPARE(ToRevisionLine(25.5,Shifts),24.5);
        for(const double Line:{5.5,15.25,25.5,40.0})QCOMPARE(FromRevisionLine(ToRevisionLine(Line,Shifts),Shifts),Line);
        // A removed line lands on the line that absorbed it.
        QCOMPARE(FromRevisionLine(22.0,{{19,-2}}),20.0);QCOMPARE(ToRevisionLine(7.0,{}),7.0);
    }
    void viewportAndLineShiftSignals() {
        QTemporaryDir Temp;qApp->setProperty("documentViewsConfigDir",Temp.path());
        const auto Restore=qScopeGuard([]{qApp->setProperty("documentViewsConfigDir",QVariant());});
        QString Text;for(int I=1;I<=200;++I)Text+=QString("line %1\n").arg(I);
        QTextView View;View.LoadContent(Text,Temp.filePath("lines.txt"),"UTF-8");View.resize(600,400);View.show();QVERIFY(QTest::qWaitForWindowExposed(&View));
        auto* Backend=static_cast<ScintillaQtDirectBackend*>(View.Backend());
        QSignalSpy Scrolled(Backend,&ITextEditorBackend::viewportScrolled),Shifted(Backend,&ITextEditorBackend::linesShifted);
        Backend->Send(SCI_LINESCROLL,0,20);QTRY_VERIFY(Scrolled.count()>0);
        Scrolled.clear();View.resize(600,300);QTRY_VERIFY(Scrolled.count()>0);
        Scrolled.clear();View.SetWordWrapMode(View.WordWrapMode()?0:1);QTRY_VERIFY(Scrolled.count()>0);
        Scrolled.clear();Backend->FoldAll(true);QVERIFY(Scrolled.count()>0);
        Backend->Send(SCI_INSERTTEXT,Backend->Send(SCI_POSITIONFROMLINE,9),reinterpret_cast<qintptr>("a\nb\n"));
        QCOMPARE(Shifted.count(),1);QCOMPARE(Shifted.last().at(0).toInt(),9);QCOMPARE(Shifted.last().at(1).toInt(),2);
        Backend->Send(SCI_UNDO);QCOMPARE(Shifted.count(),2);QCOMPARE(Shifted.last().at(0).toInt(),9);QCOMPARE(Shifted.last().at(1).toInt(),-2);
    }
    void epubShortcutScope() {
        QCOMPARE(DocumentShortcutRegistry::NormalizeNavigationKeys(QKeySequence("Num+PgUp")),QKeySequence("PgUp"));
        QCOMPARE(DocumentShortcutRegistry::NormalizeNavigationKeys(QKeySequence("Num+PgDown")),QKeySequence("PgDown"));
        QVERIFY(DocumentShortcutRegistry::Validate({{"epub.previous","Num+PgUp"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"epub.next","Num+PgUp"}}).isEmpty());
        using hc::document::DocumentShortcutRegistry;
        QVERIFY(DocumentShortcutRegistry::Validate({}).isEmpty());
        QVERIFY(DocumentShortcutRegistry::Validate({{"epub.next","Ctrl+Alt+N"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"epub.next","PgUp"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"epub.next","Tab"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"epub.next","Ctrl+S"}}).isEmpty());
    }
    void workspaceScrollbarsAndIcons_data()
    {
        QTest::addColumn<bool>("hosted"); QTest::addColumn<QString>("mode");
        for (bool Hosted : {true, false}) for (const auto& Mode : {QString("light"), QString("dark")})
            QTest::newRow(qPrintable(QString("%1-%2").arg(Hosted ? "viewer" : "editor", Mode))) << Hosted << Mode;
    }
    void workspaceScrollbarsAndIcons()
    {
        QFETCH(bool, hosted); QFETCH(QString, mode);
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        auto& Theme = fm::style::ThemeManager::instance(); const auto OldScheme = Theme.scheme();
        const auto Restore = qScopeGuard([&] { Theme.setScheme(OldScheme); qApp->setProperty("documentViewsConfigDir", QVariant()); });
        const auto* ApplicationStyle = qApp->style(); const auto ApplicationSheet = qApp->styleSheet();
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"), hosted);
        QVERIFY(Window.ApplyChromeSettings({{"schemaVersion", 1}, {"themeMode", mode}}));
        if (hosted && !Theme.isInstalled()) { QCOMPARE(qApp->style(), ApplicationStyle); QCOMPARE(qApp->styleSheet(), ApplicationSheet); }
        auto* Toolbar = Window.findChild<QToolBar*>("mainToolbar"); QVERIFY(Toolbar);
        for (auto* Action : Toolbar->actions()) {
            QVERIFY2(!Action->icon().isNull(), qPrintable(Action->objectName()));
            QVERIFY(Action->isIconVisibleInMenu()); QVERIFY(!Action->text().isEmpty());
        }
        const QByteArray Content = (QByteArray(600, 'W') + "\r\n").repeated(120);
        const auto Path = Temp.filePath("scroll.txt"); Write(Path, Content);
        QVERIFY(!Window.Handle({{"action", "open"}, {"path", Path}}).contains("error")); QTRY_VERIFY(Window.IsIdle());
        auto* Text = Window.findChild<QTextView*>(); QVERIFY(Text); Text->SetWordWrapMode(0);
        auto* Backend = static_cast<ScintillaQtDirectBackend*>(Text->Backend());
        Backend->Send(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE); Backend->Send(SCI_SETCARETPERIOD, 0);
        auto* Area = Backend->scrollArea(); QVERIFY(Area);
        Window.resize(800, 550); Window.show(); QVERIFY(QTest::qWaitForWindowExposed(&Window));
        for (auto* Bar : {Area->horizontalScrollBar(), Area->verticalScrollBar()}) {
            QTRY_VERIFY(Bar->isVisible()); QVERIFY(Bar->maximum() > 0);
            QVERIFY(qobject_cast<fm::style::PaperFluentStyle*>(Bar->style()));
            QCOMPARE(Bar->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, Bar), 12);  // Paper Fluent 12 px 트랙
            QCOMPARE(Bar->style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, Bar), 0);
            Bar->setValue(Bar->maximum() / 2); QVERIFY(Bar->value() > 0);
            QStyleOptionSlider Option; Option.initFrom(Bar); Option.rect = Bar->rect(); Option.orientation = Bar->orientation();
            Option.minimum = Bar->minimum(); Option.maximum = Bar->maximum(); Option.pageStep = Bar->pageStep(); Option.sliderPosition = Bar->value();
            const auto Slider = Bar->style()->subControlRect(QStyle::CC_ScrollBar, &Option, QStyle::SC_ScrollBarSlider, Bar);
            QVERIFY(Bar->rect().contains(Slider)); QVERIFY(Slider.width() > 5 && Slider.height() > 5);
        }
        auto* MainMenu = Window.findChild<QMenuBar*>("mainMenu"); QVERIFY(MainMenu);
        for (auto* Menu : MainMenu->findChildren<QMenu*>()) {
            if (Menu->title().isEmpty()) continue; // QMenuBar's internal overflow popup contains top-level menu titles.
            for (auto* Action : Menu->actions()) {
                if (Action->isSeparator() || (Action->isCheckable() && Action->icon().isNull())) continue;
                QVERIFY2(!Action->icon().isNull(), qPrintable(Menu->title() + ": " + Action->text()));
                QVERIFY2(Action->isIconVisibleInMenu(), qPrintable(Action->text()));
            }
        }
        QVERIFY(!Area->testAttribute(Qt::WA_OpaquePaintEvent));
        QVERIFY(!Area->testAttribute(Qt::WA_NoSystemBackground));
        QVERIFY(Area->viewport()->testAttribute(Qt::WA_OpaquePaintEvent));
        QCOMPARE(Text->Content().toUtf8(), Content);
        QTest::qWait(100); const auto Scrolled = Area->grab().toImage();
        Area->viewport()->repaint(); QCOMPARE(Area->grab().toImage(), Scrolled);
        const auto Capture = qEnvironmentVariable("HC_DOCUMENT_UI_CAPTURE");
        if (!Capture.isEmpty()) {
            QDir().mkpath(Capture); const auto Prefix = (hosted ? QString("viewer-") : QString("editor-")) + mode;
            QVERIFY(Window.grab().save(Capture + "/scroll-" + Prefix + ".png"));
            int MenuIndex = 0;
            for (auto* Top : MainMenu->actions()) if (auto* Menu = Top->menu()) {
                Menu->ensurePolished(); Menu->adjustSize();
                QVERIFY(Menu->grab().save(Capture + "/menu-" + Prefix + '-' + QString::number(MenuIndex++) + ".png"));
            }
        }
    }
    void textThemeSwitchPreservesAllStyles_data()
    {
        QTest::addColumn<bool>("Hosted");
        QTest::newRow("viewer") << true;
        QTest::newRow("editor") << false;
    }
    void textThemeSwitchPreservesAllStyles()
    {
        QFETCH(bool, Hosted);
        QTemporaryDir Temp; const auto Previous = qApp->property("documentViewsConfigDir");
        qApp->setProperty("documentViewsConfigDir", Temp.path());
        auto& Theme = fm::style::ThemeManager::instance(); const auto OldScheme = Theme.scheme();
        const auto Restore = qScopeGuard([&] { Theme.setScheme(OldScheme); qApp->setProperty("documentViewsConfigDir", Previous); });
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"), Hosted);
        Window.resize(900, 600); Window.show(); QVERIFY(QTest::qWaitForWindowExposed(&Window));
        QList<QTextView*> Views;
        for (int Index = 0; Index < 2; ++Index) {
            const auto Path = Temp.filePath(QString("theme-%1.cpp").arg(Index));
            Write(Path, "// Theme transition return\nint main() {\n    return 42;\n}\n");
            QVERIFY(!Window.Handle({{"action", "open"}, {"path", Path}}).contains("error"));
            QTRY_VERIFY(Window.IsIdle());
        }
        Views = Window.findChildren<QTextView*>(); QCOMPARE(Views.size(), 2);
        for (auto* Text : Views) {
            Text->showFindBar(false);
            auto* Search = Text->findChild<FindReplaceWidget*>(); QVERIFY(Search);
            Search->SetSearchText("return"); Search->findNext(); Search->excludeRequested(); Search->findNext();
        }
        const auto SciColor = [](QColor Color) { return Color.red() | (Color.green() << 8) | (Color.blue() << 16); };
        for (const auto& Mode : {QString("light"), QString("dark"), QString("light"), QString("dark")}) {
            QVERIFY(Window.ApplyChromeSettings({{"schemaVersion", 1}, {"themeMode", Mode}}));
            for (auto* Text : Views) {
                const auto Original = Text->Content();
                const auto Check = [&] {
                    auto* Backend = static_cast<ScintillaQtDirectBackend*>(Text->Backend());
                    QCOMPARE(Text->currentTheme(), Mode == "dark" ? QBaseView::Dark : QBaseView::Light);
                    const auto& Colors = ThemeManager::instance();
                    const auto ExpectedPalette = Theme.colors().toPalette();
                    QCOMPARE(Text->palette().color(QPalette::Window), ExpectedPalette.color(QPalette::Window));
                    auto* Ruler = Text->findChild<QWidget*>("columnRuler"); QVERIFY(Ruler);
                    QCOMPARE(Ruler->palette().color(QPalette::Window), ExpectedPalette.color(QPalette::Window));
                    auto* Search = Text->findChild<FindReplaceWidget*>(); QVERIFY(Search);
                    QCOMPARE(Search->palette().color(QPalette::Text), ExpectedPalette.color(QPalette::Text));
                    int TextIcons = 0;
                    for (auto* Action : Search->findChildren<QAction*>()) {
                        if (!Action->isCheckable() || Action->icon().isNull()) continue;
                        const auto Image = Action->icon().pixmap(QSize(16, 16), QIcon::Normal, QIcon::Off).toImage();
                        QColor Ink(Qt::transparent);
                        for (int Y = 0; Y < Image.height(); ++Y) for (int X = 0; X < Image.width(); ++X)
                            if (Image.pixelColor(X, Y).alpha() > Ink.alpha()) Ink = Image.pixelColor(X, Y);
                        QVERIFY(Ink.alpha() > 0); QCOMPARE(Ink.rgb(), ExpectedPalette.color(QPalette::Text).rgb());
                        ++TextIcons;
                    }
                    QCOMPARE(TextIcons, 2);
                    QCOMPARE(Backend->Send(SCI_STYLEGETBACK, STYLE_DEFAULT), SciColor(Colors.color("text.background")));
                    QCOMPARE(Backend->Send(SCI_STYLEGETBACK, STYLE_LINENUMBER), SciColor(Colors.color("text.marginBackground")));
                    QCOMPARE(Backend->Send(SCI_STYLEGETFORE, STYLE_LINENUMBER), SciColor(Colors.color("text.marginForeground")));
                    QCOMPARE(Backend->Send(SCI_STYLEGETFORE, STYLE_INDENTGUIDE), SciColor(Colors.color("text.indentGuide")));
                    QCOMPARE(Backend->Send(SCI_STYLEGETFORE, 5), SciColor(Colors.color("text.lexer.keyword")));
                    QCOMPARE(Backend->Send(SCI_GETCARETFORE), SciColor(Colors.color("text.caret")));
                    QCOMPARE(Backend->Send(SCI_GETADDITIONALCARETFORE), SciColor(Colors.color("text.caret")));
                    for (const auto& Indicator : {qMakePair(20, fm::style::Token::DangerBg),
                            qMakePair(21, fm::style::Token::Warn), qMakePair(22, fm::style::Token::Fg3),
                            qMakePair(23, fm::style::Token::DangerLine)}) {
                        QCOMPARE(Backend->Send(SCI_INDICGETFORE, Indicator.first), SciColor(Theme.colors()[Indicator.second]));
                        QVERIFY(Backend->Send(SCI_INDICGETUNDER, Indicator.first));
                    }
                    QVERIFY(Backend->Send(SCI_INDICATORVALUEAT, 20, Original.indexOf("return")));
                    QCOMPARE(Text->Content(), Original); QVERIFY(!Text->isModified());
                };
                Check(); if (QTest::currentTestFailed()) return;
                // Preference reload and direct font changes both used to erase gutter/syntax colors.
                Text->ReloadPreferences(); Check(); if (QTest::currentTestFailed()) return;
                auto Font = Text->editorFont(); Font.setPointSize(13); Text->Backend()->setEditorFont(Font);
                Check(); if (QTest::currentTestFailed()) return;
            }
            const auto Capture = qEnvironmentVariable("HC_DOCUMENT_UI_CAPTURE");
            if (!Capture.isEmpty()) {
                QDir().mkpath(Capture); QTest::qWait(30);
                QVERIFY(Window.grab().save(Capture + "/text-switch-" + (Hosted ? "viewer-" : "editor-") + Mode + ".png"));
            }
        }
    }
    void renderingSettingsApplyWithoutContentChanges()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); mrst::scintilla::setTextMask(false, 1.2, 1.1); });
        QTextView View; View.LoadContent(QString::fromUtf8("한글 Á 😀\r\nline"), "render.txt", "UTF-8");
        View.ReplaceContent(QString::fromUtf8("edited 한글 😀")); const auto Before = View.Content();
        const auto Undo = static_cast<ScintillaQtDirectBackend*>(View.Backend())->Send(SCI_CANUNDO);
        DocumentSettingsDialog Dialog(UnifiedConfig::FileName(Temp.path()), {});
        Dialog.findChild<QCheckBox*>("documentViews/textView/mask/enabled")->setChecked(true);
        Dialog.findChild<QDoubleSpinBox*>("documentViews/textView/mask/gamma")->setValue(1.55);
        Dialog.findChild<QDoubleSpinBox*>("documentViews/textView/mask/strength")->setValue(1.35);
        QVERIFY(Dialog.Apply()); View.ReloadPreferences();
        const auto& State = mrst::scintilla::textMaskState();
        QVERIFY(State.enabled); QCOMPARE(State.gamma, 1.55); QCOMPARE(State.strength, 1.35);
        QCOMPARE(View.Content(), Before); QCOMPARE(static_cast<ScintillaQtDirectBackend*>(View.Backend())->Send(SCI_CANUNDO), Undo);
        { ::AppSettings Settings; Settings.setValue("textView/mask/gamma", "invalid"); Settings.setValue("textView/mask/strength", 100.0); }
        View.ReloadPreferences(); QCOMPARE(State.gamma, 1.2); QCOMPARE(State.strength, 1.1);
        QCOMPARE(View.Content(), Before);
    }

    void recoveryPreferenceKeepsHostReplacementSafe()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        { ::AppSettings Settings; Settings.setValue("textView/recoveryEnabled", false); }
        const auto Root = Temp.filePath("recovery");
        Write(Temp.filePath("one.txt"), "original"); Write(Temp.filePath("two.txt"), "next");
        DocumentWorkspaceWidget Window(Root, true);
        const auto Id = Window.Handle({{"action", "openHost"}, {"path", Temp.filePath("one.txt")}})["documentId"].toString(); QTRY_VERIFY(Window.IsIdle());
        Window.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "unsaved"}});
        Window.Checkpoint(); QTest::qWait(600); QVERIFY(!QFile::exists(Root + "/" + Id + ".json"));
        Window.Handle({{"action", "openHost"}, {"path", Temp.filePath("two.txt")}});
        QTRY_VERIFY(QFile::exists(Root + "/" + Id + ".json")); QTRY_VERIFY(Window.IsIdle());
        DocumentRecoveryStore Recovery(Root); const auto Record = Recovery.Read((Root + "/" + Id + ".json").toStdString());
        QVERIFY(Record.HasValue()); QCOMPARE(QByteArray::fromStdString(Record.Value().bytes_), QByteArray("unsaved"));
        QCOMPARE(Read(Temp.filePath("one.txt")), QByteArray("original"));
    }
    void externalWatcherStabilityAndSelfWrite()
    {
        QTemporaryDir Temp; const auto Path = Temp.filePath("watched.txt"); Write(Path, "one");
        mrst::ExternalChangeWatcher Watcher; Watcher.setDetection(mrst::ExternalChangeWatcher::Detection::Poll);
        Watcher.setSettleInterval(20); Watcher.setWatchedFiles({Path});
        QSignalSpy Changed(&Watcher, &mrst::ExternalChangeWatcher::sigFileChanged);
        QSignalSpy Vanished(&Watcher, &mrst::ExternalChangeWatcher::sigFileVanished);
        Write(Path, "changed length"); Watcher.recheckAll(); QTRY_COMPARE(Changed.count(), 1);
        Watcher.beginSelfWrite(Path); Write(Path, "own save"); Watcher.recheckAll(); QTest::qWait(80); QCOMPARE(Changed.count(), 1);
        Watcher.endSelfWrite(Path); Watcher.recheckAll(); QTest::qWait(80); QCOMPARE(Changed.count(), 1);
        QVERIFY(QFile::remove(Path)); Watcher.recheckAll(); QTRY_COMPARE(Vanished.count(), 1);
        Write(Path, "recreated"); Watcher.recheckAll(); QTRY_COMPARE(Changed.count(), 2);
        Watcher.setAction(mrst::ExternalChangeWatcher::Action::Ignore); Write(Path, "ignored changes"); Watcher.recheckAll(); QTest::qWait(80); QCOMPARE(Changed.count(), 2);
        Watcher.setAction(mrst::ExternalChangeWatcher::Action::Reload);
        Watcher.beginSelfWrite(Path); Write(Path, "conflicting other writer"); Watcher.endSelfWrite(Path, false);
        QTRY_COMPARE(Changed.count(), 3);
        Watcher.setDetection(mrst::ExternalChangeWatcher::Detection::Notify);
        QSaveFile Atomic(Path); QVERIFY(Atomic.open(QIODevice::WriteOnly)); Atomic.write("atomic external replacement"); QVERIFY(Atomic.commit());
        QTRY_COMPARE(Changed.count(), 4);
        Watcher.setWatchedFiles({}); QVERIFY(Watcher.watchedFiles().isEmpty());
    }
    void newDocumentPolicyAndExistingBytes()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        { ::AppSettings Settings; Settings.setValue("textView/saveEncoding", "UTF-16BE"); Settings.setValue("textView/saveBomMode", 1); Settings.setValue("textView/defaultLineEnding", "CR"); Settings.setValue("textView/recoveryEnabled", false); }
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));
        const auto Id = Window.Handle({{"action", "new"}})["documentId"].toString(); QTRY_VERIFY(Window.IsIdle());
        auto* Text = Window.findChild<QTextView*>(); QVERIFY(Text);
        QCOMPARE(static_cast<ScintillaQtDirectBackend*>(Text->Backend())->Send(SCI_GETEOLMODE), qintptr(SC_EOL_CR));
        Window.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "A\rB"}});
        Window.Handle({{"action", "saveAs"}, {"documentId", Id}, {"path", Temp.filePath("new.txt")}}); QTRY_VERIFY(Window.IsIdle());
        QCOMPARE(Read(Temp.filePath("new.txt")), QByteArray::fromHex("feff0041000d0042"));
        const QByteArray Original("existing\r\nline\n"); Write(Temp.filePath("old.txt"), Original);
        Window.Handle({{"action", "open"}, {"path", Temp.filePath("old.txt")}}); QTRY_VERIFY(Window.IsIdle());
        auto* Tabs = Window.findChild<QTabWidget*>("documentTabs"); auto* Pane = Tabs->currentWidget()->findChild<QTextView*>(); QVERIFY(Pane);
        QCOMPARE(Pane->Content().toUtf8(), Original);
        auto* Document = qobject_cast<DocumentViewPane*>(Tabs->currentWidget()); QVERIFY(Document);
        QCOMPARE(Document->Content(), Original);
    }
    void externalReloadProtectsDirtyDocument()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        const auto Path = Temp.filePath("source.txt"); Write(Path, "before");
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"));
        const auto Id = Window.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString(); QTRY_VERIFY(Window.IsIdle());
        auto* Watcher = Window.findChild<mrst::ExternalChangeWatcher*>(); QVERIFY(Watcher); Watcher->setSettleInterval(20);
        Write(Path, "external replacement"); Watcher->recheckAll();
        QTRY_COMPARE(Window.findChild<QTextView*>()->Content(), QString("external replacement"));
        Window.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "my edits"}});
        Write(Path, "another external write"); Watcher->recheckAll();
        QTRY_VERIFY(Window.findChild<QMessageBox*>("externalChangePrompt"));
        QCOMPARE(Window.findChild<QTextView*>()->Content(), QString("my edits"));
        auto* Prompt = Window.findChild<QMessageBox*>("externalChangePrompt");
        // An edit after the question invalidates its consent to discard.
        Window.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "newer edits"}});
        Prompt->done(QMessageBox::Yes); QTest::qWait(80);
        QCOMPARE(Window.findChild<QTextView*>()->Content(), QString("newer edits"));
    }

    void migrationAndTypePreservation()
    {
        QTemporaryDir Temp;
        const QByteArray Settings("{\"schemaVersion\":1,\"general\":{\"future\":42}}");
        const QByteArray Workspace("{\"schemaVersion\":1,\"workspace\":{\"version\":9,\"future\":true},\"unknown\":17}");
        Write(Temp.filePath("settings.json"), Settings); Write(Temp.filePath("workspace.json"), Workspace);
        { QSettings Ini(Temp.filePath("document-views.ini"), QSettings::IniFormat); Ini.setValue("textView/tabWidth", 8); Ini.setValue("theme/overrides", QByteArray("{\"x\":1}")); Ini.sync(); }
        const auto IniBytes = Read(Temp.filePath("document-views.ini")); QJsonObject Root; QString Error;
        QVERIFY2(UnifiedConfig::Load(UnifiedConfig::FileName(Temp.path()), Root, Error), qPrintable(Error));
        QCOMPARE(Root["schemaVersion"].toInt(), 2); QCOMPARE(Root["workspace"].toObject()["version"].toInt(), 9);
        QVERIFY(!Root["workspace"].toObject().contains("workspace")); QCOMPARE(Root["legacyWorkspace"].toObject()["unknown"].toInt(), 17);
        QCOMPARE(UnifiedConfig::Decode(UnifiedConfig::Value(Root, {"documentViews", "theme", "overrides"})).toByteArray(), QByteArray("{\"x\":1}"));
        QCOMPARE(Read(Temp.filePath("settings.json")), Settings); QCOMPARE(Read(Temp.filePath("workspace.json")), Workspace); QCOMPARE(Read(Temp.filePath("document-views.ini")), IniBytes);
        Write(Temp.filePath("settings.json"), "broken"); QJsonObject Again;
        QVERIFY(UnifiedConfig::Load(UnifiedConfig::FileName(Temp.path()), Again, Error)); QCOMPARE(Again, Root);
    }
    void independentWritesAndConflict()
    {
        QTemporaryDir Temp; const auto Path = UnifiedConfig::FileName(Temp.path()); QString Error;
        QJsonObject Base, Saved; QVERIFY(UnifiedConfig::Load(Path, Base, Error));
        auto A = Base; UnifiedConfig::Set(A, {"documentViews", "textView", "tabWidth"}, 8);
        auto B = Base; UnifiedConfig::Set(B, {"workspace", "future"}, 123);
        QVERIFY(UnifiedConfig::Save(Path, Base, A, Saved, Error)); QVERIFY(UnifiedConfig::Save(Path, Base, B, Saved, Error));
        QCOMPARE(UnifiedConfig::Value(Saved, {"documentViews", "textView", "tabWidth"}).toInt(), 8);
        QCOMPARE(Saved["workspace"].toObject()["future"].toInt(), 123);
        auto Conflicting = Base; UnifiedConfig::Set(Conflicting, {"documentViews", "textView", "tabWidth"}, 2);
        const auto Bytes = Read(Path); QVERIFY(!UnifiedConfig::Save(Path, Base, Conflicting, Saved, Error)); QVERIFY(Error.startsWith("conflict:")); QCOMPARE(Read(Path), Bytes);
        QVERIFY(UnifiedConfig::Save(Path, Base, Conflicting, Saved, Error, true)); QCOMPARE(Saved["workspace"].toObject()["future"].toInt(), 123);
        QLockFile Lock(Path + ".lock"); QVERIFY(Lock.tryLock()); QVERIFY(!UnifiedConfig::Save(Path, Saved, Saved, Base, Error)); QCOMPARE(Error, QString("file-locked"));
    }
    void commanderAndDocumentProfilesStaySeparate()
    {
        QTemporaryDir Temp;
        Repository Commander(Temp.path());
        QVERIFY(Commander.Put(Section::General, QJsonObject{{"sizeInHeader", "adaptive2"}}));
        const auto Original = Read(Commander.FileName());
        QJsonObject Base, Saved; QString Error;
        const auto Path = UnifiedConfig::FileName(Temp.path());
        QVERIFY(UnifiedConfig::Load(Path, Base, Error));
        auto Next = Base; UnifiedConfig::Set(Next, {"documentViews", "textView", "tabWidth"}, 7);
        QVERIFY(UnifiedConfig::Save(Path, Base, Next, Saved, Error));
        QCOMPARE(Read(Commander.FileName()), Original);
        Repository Restart(Temp.path()); QCOMPARE(Restart.Object()["schemaVersion"].toInt(), 1);
        QCOMPARE(Restart.Read(Section::General).toObject()["sizeInHeader"].toString(), QString("adaptive2"));
        QCOMPARE(ConfigPaths::Resolve({"app"}, "D:/bin", "D:/legacy").directory_, QString("D:/bin"));
        QCOMPARE(ConfigPaths::Resolve({"app", "--portable"}, "D:/bin", "D:/legacy").directory_, QString("D:/bin"));
    }
    void invalidAndFutureFilesArePreserved()
    {
        for (const auto& Bytes : {QByteArray("broken"), QByteArray("{\"schemaVersion\":99}")}) {
            QTemporaryDir Temp; const auto Path = UnifiedConfig::FileName(Temp.path()); Write(Path, Bytes);
            QJsonObject Root, Saved; QString Error; QVERIFY(!UnifiedConfig::Load(Path, Root, Error));
            QVERIFY(!UnifiedConfig::Save(Path, {}, {{"schemaVersion", 2}}, Saved, Error)); QCOMPARE(Read(Path), Bytes);
        }
    }
    void paths()
    {
        QCOMPARE(ConfigPaths::Resolve({"app"}, "D:/bin", "D:/user", true).directory_, QString("D:/bin"));
        QCOMPARE(ConfigPaths::Resolve({"app", "--portable"}, "D:/bin", {}, true).directory_, QString("D:/bin"));
        QCOMPARE(ConfigPaths::Resolve({"app", "--config-dir=D:/custom"}, "D:/bin").directory_, QString("D:/custom"));
        QCOMPARE(ConfigPaths::Resolve({"app", "--", "--config-dir=D:/other"}, "D:/bin", {}, true).directory_, QString("D:/bin"));
        QVERIFY(!ConfigPaths::Resolve({"app", "--config-dir="}, "D:/bin").error_.isEmpty());
        QVERIFY(!ConfigPaths::Resolve({"app", "--config-dir", "D:/a", "--config-dir=D:/b"}, "D:/bin").error_.isEmpty());
    }
    void epubEditingWorkspaceRoundTrip()
    {
        QFile Fixture(qEnvironmentVariable("HC_EPUB_READER_EPUB"));if(Fixture.fileName().isEmpty())QSKIP("EPUB fixture required");
        QVERIFY(Fixture.open(QIODevice::ReadOnly));const auto Bytes=Fixture.readAll();QTemporaryDir Temp;
        const auto OldWorkspace=qApp->property("epubWorkspaceDirectory"),OldConfig=qApp->property("documentViewsConfigDir");
        qApp->setProperty("epubWorkspaceDirectory",Temp.path());qApp->setProperty("documentViewsConfigDir",Temp.path());
        const auto Restore=qScopeGuard([&]{qApp->setProperty("epubWorkspaceDirectory",OldWorkspace);qApp->setProperty("documentViewsConfigDir",OldConfig);});
        const auto Path=Temp.filePath("book.epub"),Json=Temp.filePath("HyperEditor.epub-workspace.json");Write(Path,Bytes);
        QStringList Tabs;QString Current;int Position=0,Anchor=0,Scroll=0,FirstPosition=0,FirstAnchor=0;
        {
            EpubSourceEditor Editor(Bytes,Path);Editor.SetRecoveryEnabled(false);Editor.resize(850,650);Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);
            const auto Select=[&](const QString& Resource){return Editor.Execute({{"action","epub.edit.select"},{"resource",Resource}})["accepted"].toBool();};
            QVERIFY(Select("EPUB/one.xhtml"));QCoreApplication::processEvents();auto* Backend=static_cast<ScintillaQtDirectBackend*>(Editor.SourceView()->Backend());
            FirstAnchor=int(Backend->Send(SCI_POSITIONFROMLINE,20))+3;FirstPosition=FirstAnchor+5;Backend->Send(SCI_SETSEL,FirstAnchor,FirstPosition);Backend->Send(SCI_SETFIRSTVISIBLELINE,4);
            QVERIFY(Select("EPUB/two.xhtml"));QCoreApplication::processEvents();Anchor=int(Backend->Send(SCI_POSITIONFROMLINE,30))+3;Position=Anchor+5;Backend->Send(SCI_SETSEL,Anchor,Position);Backend->Send(SCI_SETFIRSTVISIBLELINE,8);
            Scroll=int(Backend->Send(SCI_GETFIRSTVISIBLELINE));
            for(const auto& Value:Editor.Snapshot()["tabs"].toArray())Tabs.append(Value.toString());Current=Editor.Snapshot()["current"].toString();
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(Json),5000);
        }
        const auto Saved=QJsonDocument::fromJson(Read(Json)).object();QCOMPARE(Saved["documents"].toObject().size(),1);
        {
            EpubSourceEditor Editor(Bytes,Path);Editor.SetRecoveryEnabled(false);Editor.resize(850,650);Editor.show();QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);
            QCOMPARE(Editor.Snapshot()["current"].toString(),Current);QCOMPARE(Editor.Snapshot()["tabs"].toArray(),QJsonArray::fromStringList(Tabs));
            auto* Backend=static_cast<ScintillaQtDirectBackend*>(Editor.SourceView()->Backend());
            QCOMPARE(Backend->Send(SCI_GETCURRENTPOS),qintptr(Position));QCOMPARE(Backend->Send(SCI_GETANCHOR),qintptr(Anchor));
            QTRY_COMPARE(Backend->Send(SCI_GETFIRSTVISIBLELINE),qintptr(Scroll));
            QVERIFY(Editor.Execute({{"action","epub.edit.select"},{"resource","EPUB/one.xhtml"}})["accepted"].toBool());
            QCOMPARE(Backend->Send(SCI_GETCURRENTPOS),qintptr(FirstPosition));QCOMPARE(Backend->Send(SCI_GETANCHOR),qintptr(FirstAnchor));
        }
        const auto Other=Temp.filePath("other.epub");Write(Other,Bytes);
        {EpubSourceEditor Editor(Bytes,Other);Editor.SetRecoveryEnabled(false);QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);}
        QCOMPARE(QJsonDocument::fromJson(Read(Json)).object()["documents"].toObject().size(),2);
        Write(Json,"{broken");
        {EpubSourceEditor Editor(Bytes,Path);Editor.SetRecoveryEnabled(false);QTRY_VERIFY_WITH_TIMEOUT(Editor.IsReady(),15000);QVERIFY(!Editor.Snapshot()["workspaceError"].toString().isEmpty());}
        QCOMPARE(Read(Json),QByteArray("{broken"));
    }
    void comicSettingsAndShortcutCatalog()
    {
        QTemporaryDir Temp; const auto Path=Temp.filePath("HyperEditor.json");
        const QString Key="documentViews/imageView/comic/coverAlone";
        {DocumentSettingsDialog Dialog(Path,{});
            auto* Tabs=Dialog.findChild<QTabWidget*>("imageViewerTabs");QVERIFY(Tabs);QCOMPARE(Tabs->tabText(0),QString::fromUtf8("코믹"));
            auto* Cover=Dialog.findChild<QCheckBox*>(Key);QVERIFY(Cover);QVERIFY(Cover->isChecked());
            Cover->setChecked(false);QVERIFY(Dialog.Apply());}
        const auto Saved=Read(Path);
        {DocumentSettingsDialog Dialog(Path,{});auto* Cover=Dialog.findChild<QCheckBox*>(Key);QVERIFY(!Cover->isChecked());Cover->setChecked(true);Dialog.reject();}
        QCOMPARE(Read(Path),Saved);
        DocumentSettingsDialog Dialog(Path,{});
        for(const auto* Id:{"image.volumePrevious","image.volumeNext","image.fullscreen","image.animation","image.previousSpace"})
            QVERIFY(Dialog.findChild<QKeySequenceEdit*>("documentViews/shortcuts/bindings/"+QString(Id)));
        QCOMPARE(DocumentShortcutRegistry::ScopeOf("image.volumeNext"),QString("image.comic"));
        QVERIFY(DocumentShortcutRegistry::CoActive("image","image.comic"));
        QVERIFY(!DocumentShortcutRegistry::Validate({{"image.volumeNext","Space"}}).isEmpty());
        QVERIFY(DocumentShortcutRegistry::Validate({{"image.volumeNext","Ctrl+Alt+N"}}).isEmpty());
    }
    void shortcutValidationAndAliases()
    {
        QVERIFY(DocumentShortcutRegistry::Validate({}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"text.find", "Ctrl+Space"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"text.find", "Ctrl+K"}, {"text.replace", "Ctrl+K, Ctrl+F"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"text.find", "Ctrl+Shift+Z"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"text.find", 7}}).isEmpty());
        const auto Values = DocumentShortcutRegistry::Bindings({{"file.open", "Ctrl+Alt+O"}, {"text.new", "Ctrl+Alt+N"}, {"future.command", "F10"}});
        QCOMPARE(Values["file.openWorkspace"].toString(), QString("Ctrl+Alt+O"));
        QCOMPARE(Values["file.new"].toString(), QString("Ctrl+Alt+N"));
        QCOMPARE(Values["future.command"].toString(), QString("F10"));
    }
    void shortcutScopes()
    {
        // Scopes that never share a focus position may reuse a key; those that do may not.
        QVERIFY(DocumentShortcutRegistry::CoActive("epub-edit", "text")); QVERIFY(!DocumentShortcutRegistry::CoActive("epub", "text"));
        QVERIFY(!DocumentShortcutRegistry::CoActive("epub-edit.tree", "epub-edit.source")); QVERIFY(!DocumentShortcutRegistry::CoActive("image", "pdf"));
        QVERIFY(DocumentShortcutRegistry::Validate({{"epubTree.rename", "Ctrl+Shift+R"}, {"epubSource.openLink", "Ctrl+Shift+R"}}).isEmpty());
        QVERIFY(DocumentShortcutRegistry::Validate({{"text.find", "Ctrl+K"}, {"epub.focus-search", "Ctrl+K"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"epubEdit.apply", "Ctrl+F"}}).isEmpty());
        const auto Issues = DocumentShortcutRegistry::Issues({{"epubEdit.filter", "Ctrl+P"}});
        QCOMPARE(Issues.size(), 1); QCOMPARE(Issues.first().id_, QString("epubEdit.filter")); QCOMPARE(Issues.first().other_, QString("file.print"));
        // Input scopes keep editing keys and an unmodified Enter; a modified Enter is free. Trees and viewers may use movement keys.
        QVERIFY(!DocumentShortcutRegistry::Validate({{"epubEdit.apply", "Delete"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"epubSource.quickFix", "Return"}}).isEmpty());
        QVERIFY(DocumentShortcutRegistry::Validate({{"epubSource.quickFix", "Ctrl+Alt+Return"}}).isEmpty());
        QVERIFY(!DocumentShortcutRegistry::Validate({{"epubTree.rename", "Return"}}).isEmpty());
        QVERIFY(DocumentShortcutRegistry::Validate({{"image.next", "N"}, {"pdf.pageNext", "N"}}).isEmpty());
        QCOMPARE(DocumentShortcutRegistry::NormalizeNavigationKeys(QKeySequence("Ctrl+Return")), QKeySequence("Ctrl+Enter"));
        for (const auto& Command : DocumentShortcutRegistry::Commands()) QVERIFY2(!DocumentShortcutRegistry::ScopeLabel(Command.scope_).isEmpty(), qPrintable(Command.id_));
    }
    void shortcutsRouteAcrossViewsAndSearchFields()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        DocumentWorkspaceWidget Window(Temp.filePath("recovery")); Window.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); }); Window.show();
        const auto First = Window.Handle({{"action", "new"}})["documentId"].toString(); QVERIFY(!First.isEmpty()); QTRY_VERIFY(Window.IsIdle());
        auto* Text = Window.findChild<QTextView*>(); QVERIFY(Text);
        auto* Input = Text->Backend()->widget(); Window.activateWindow(); Input->setFocus(); QTest::qWait(100);
        QInputMethodEvent Ime; Ime.setCommitString(QString::fromUtf8("한글")); QApplication::sendEvent(Input, &Ime);
        QTRY_COMPARE(Text->Content(), QString::fromUtf8("한글"));
        QJsonObject Base, Saved; QString Error; const auto Path = UnifiedConfig::FileName(Temp.path()); QVERIFY(UnifiedConfig::Load(Path, Base, Error));
        auto Next = Base;
        const QJsonObject Bindings{{"text.find", "Ctrl+K, Ctrl+F"}, {"text.findNext", "Ctrl+Alt+N"}, {"text.wordWrap", ""},
            {"tab.next", "Ctrl+Alt+J"}, {"edit.copy", "Ctrl+Alt+C"}, {"app.settings", "Ctrl+Alt+I"}};
        UnifiedConfig::Set(Next, {"documentViews", "shortcuts", "bindings"}, Bindings);
        QVERIFY2(UnifiedConfig::Save(Path, Base, Next, Saved, Error), qPrintable(Error));
        auto* FindAction = Window.findChild<QAction*>("text.find"); QVERIFY(FindAction);
        QTRY_COMPARE(FindAction->shortcut(), QKeySequence("Ctrl+K, Ctrl+F"));
        QSignalSpy Find(Text->findChild<QAction*>("text.find"), &QAction::triggered);
        QTest::keyClick(Input, Qt::Key_F, Qt::ControlModifier); QCOMPARE(Find.count(), 0);
        QTest::keyClick(Input, Qt::Key_K, Qt::ControlModifier); QTest::keyClick(Input, Qt::Key_F, Qt::ControlModifier); QTRY_COMPARE(Find.count(), 1);
        auto* Search = Text->findChild<FindReplaceWidget*>(); QVERIFY(Search && Search->isVisible());
        auto* SearchInput = qobject_cast<QLineEdit*>(QApplication::focusWidget()); QVERIFY(SearchInput);
        QSignalSpy NextFound(Search, &FindReplaceWidget::findNext);
        QTest::keyClick(SearchInput, Qt::Key_F3); QCOMPARE(NextFound.count(), 0);
        QSignalSpy LocalNext(Text->findChild<QAction*>("text.findNext"), &QAction::triggered);
        QTest::keyClick(SearchInput, Qt::Key_N, Qt::ControlModifier | Qt::AltModifier); QTRY_COMPARE(LocalNext.count(), 1);
        Text->hideFindBar(); Input->setFocus();
        const auto Content = Text->Content(); QTest::keyClick(Input, Qt::Key_I, Qt::ControlModifier); QCOMPARE(Text->Content(), Content);
        const auto Wrap = Text->isWordWrapEnabled(); QTest::keyClick(Input, Qt::Key_Z, Qt::AltModifier); QCOMPARE(Text->isWordWrapEnabled(), Wrap);
        Text->Backend()->setCursorPosition(0, 0);
        QTest::keyClick(Input, Qt::Key_Tab); QVERIFY(Text->Content().size() > Content.size()); // Ctrl+I retirement must not retire Tab.
        Window.Handle({{"action", "saveAs"}, {"documentId", First}, {"path", Temp.filePath("saved.txt")}}); QTRY_VERIFY(Window.IsIdle());
        QCOMPARE(Read(Temp.filePath("saved.txt")), QByteArray::fromHex("efbbbf") + Text->Content().toUtf8());
        const auto Second = Window.Handle({{"action", "new"}})["documentId"].toString(); QTRY_VERIFY(Window.IsIdle());
        auto* Tabs = Window.findChild<QTabWidget*>("documentTabs"); QCOMPARE(Tabs->count(), 2);
        auto* OtherText = Tabs->currentWidget()->findChild<QTextView*>(); QVERIFY(OtherText && OtherText != Text);
        auto* OtherInput = OtherText->Backend()->widget(); OtherInput->setFocus();
        QSignalSpy OtherFind(OtherText->findChild<QAction*>("text.find"), &QAction::triggered);
        QTest::keyClick(OtherInput, Qt::Key_K, Qt::ControlModifier); QTest::keyClick(OtherInput, Qt::Key_F, Qt::ControlModifier); QTRY_COMPARE(OtherFind.count(), 1); QCOMPARE(Find.count(), 1);
        OtherText->hideFindBar(); OtherInput->setFocus(); const int BeforeTab = Tabs->currentIndex();
        QTest::keyClick(OtherInput, Qt::Key_Tab, Qt::ControlModifier); QCOMPARE(Tabs->currentIndex(), BeforeTab);
        QTest::keyClick(OtherInput, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier); QTRY_VERIFY(Tabs->currentIndex() != BeforeTab);
        Input->setFocus(); Window.SetClipboardBroker(true); Text->Backend()->setSelectionByPos(0, 2);
        QTest::keyClick(Input, Qt::Key_C, Qt::ControlModifier); QVERIFY(Window.Snapshot()["clipboard"].toObject().isEmpty());
        QTest::keyClick(Input, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier); QTRY_VERIFY(!Window.Snapshot()["clipboard"].toObject().isEmpty());
        Text->setReadOnly(true); const auto ReadOnlyText = Text->Content(); QTest::keyClick(Input, Qt::Key_X, Qt::ControlModifier); QCOMPARE(Text->Content(), ReadOnlyText);
        QVERIFY(!Second.isEmpty());
    }
    void wrapModesLegacyAndLastMode()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        const auto Path = UnifiedConfig::FileName(Temp.path());
        QTextView Text; Text.LoadContent("one two three\n", "sample.txt", "UTF-8");
        QCOMPARE(Text.WordWrapMode(), 2);
        auto* Backend = static_cast<ScintillaQtDirectBackend*>(Text.Backend());
        Backend->setCursorPosition(0, 0); Backend->replace("X");
        const auto Content = Text.Content(); QSignalSpy Changed(Backend, &ITextEditorBackend::textChanged);
        for (int Mode = 0; Mode <= 3; ++Mode) {
            Text.SetWordWrapMode(Mode); QCOMPARE(Backend->Send(SCI_GETWRAPMODE), Mode);
            QCOMPARE(Text.Content(), Content); QVERIFY(Backend->Send(SCI_CANUNDO));
        }
        Text.setWordWrap(false); QCOMPARE(Text.WordWrapMode(), 0);
        Text.setWordWrap(true); QCOMPARE(Text.WordWrapMode(), 3); QCOMPARE(Changed.count(), 0);
        Backend->Send(SCI_UNDO); QCOMPARE(Text.Content(), QString("one two three\n"));
        QJsonObject Base, Saved; QString Error; QVERIFY(UnifiedConfig::Load(Path, Base, Error));
        auto Next = Base; UnifiedConfig::Set(Next, {"documentViews", "textView"}, QJsonObject{{"wordWrap", false}});
        QVERIFY(UnifiedConfig::Save(Path, Base, Next, Saved, Error)); Text.ReloadPreferences(); QCOMPARE(Text.WordWrapMode(), 0);
        Base = Saved; Next = Base; UnifiedConfig::Set(Next, {"documentViews", "textView", "wordWrap"}, true);
        QVERIFY(UnifiedConfig::Save(Path, Base, Next, Saved, Error)); Text.ReloadPreferences(); QCOMPARE(Text.WordWrapMode(), 1);
        DocumentSettingsDialog Dialog(Path, {});
        Dialog.findChild<QComboBox*>("documentViews/textView/wordWrapMode")->setCurrentIndex(3);
        Dialog.findChild<QComboBox*>("documentViews/textView/wrapVisualFlags")->setCurrentIndex(7);
        Dialog.findChild<QComboBox*>("documentViews/textView/wrapIndentMode")->setCurrentIndex(3);
        Dialog.findChild<QSpinBox*>("documentViews/textView/rulerFontSize")->setValue(14);
        QVERIFY(Dialog.Apply()); Text.ReloadPreferences();
        QCOMPARE(Backend->Send(SCI_GETWRAPMODE), 3); QCOMPARE(Backend->Send(SCI_GETWRAPVISUALFLAGS), 7);
        QCOMPARE(Backend->Send(SCI_GETWRAPINDENTMODE), 3);
        QCOMPARE(Text.findChild<QWidget*>("columnRuler")->font().pointSize(), 14);
        QCOMPARE(Text.Content(), QString("one two three\n"));
    }
    void unicodeMetricsSearchAndMultipleCarets()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        QTextView Text; const auto Original = QString::fromUtf8("한글 😀 é\r\nsecond\r\n");
        Text.LoadContent(Original, "sample.txt", "UTF-8"); Text.show();
        auto* Backend = static_cast<ScintillaQtDirectBackend*>(Text.Backend());
        QCOMPARE(Text.characterCount(), Original.size());
        Backend->setCursorPosition(0, 1); const auto Position = Backend->currentPos();
        QCOMPARE(Backend->WordAtCaret(), QString::fromUtf8("한글")); QCOMPARE(Backend->currentPos(), Position);
        Text.showFindBar(false); QCOMPARE(Text.findChild<FindReplaceWidget*>()->searchText(), QString::fromUtf8("한글")); Text.hideFindBar();
        Backend->setText("a\nb\n"); QCOMPARE(Text.characterCount(), 4);
        Backend->Send(SCI_SETSELECTION, 1, 1); Backend->Send(SCI_ADDSELECTION, 3, 3);
        QInputMethodEvent Ime; Ime.setCommitString(QString::fromUtf8("한😀")); QApplication::sendEvent(Backend->widget(), &Ime);
        QCOMPARE(Text.Content(), QString::fromUtf8("a한😀\nb한😀\n")); QCOMPARE(Text.characterCount(), Text.Content().size());
        QApplication::clipboard()->setText("!"); Backend->Send(SCI_PASTE);
        QCOMPARE(Text.Content(), QString::fromUtf8("a한😀!\nb한😀!\n")); QCOMPARE(Text.characterCount(), Text.Content().size());
        Backend->Send(SCI_UNDO); QCOMPARE(Text.Content(), QString::fromUtf8("a한😀\nb한😀\n")); QCOMPARE(Text.characterCount(), Text.Content().size());
        Backend->Send(SCI_REDO); QCOMPARE(Text.characterCount(), Text.Content().size());
        Backend->clear(); QCOMPARE(Text.characterCount(), 0);
        Backend->setText(Original); Backend->setSelectionRange(0, 0, 0, 5); QCOMPARE(Text.selectedCharacterCount(), 5);
        QCOMPARE(Text.characterCount(), Original.size());
    }
    void foldAndDisplayChangesAreNotEdits()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        QTextView Text; Text.LoadContent("int main() {\n    if (true) {\n        return 1;\n    }\n}\n", "sample.cpp", "UTF-8");
        auto* Backend = static_cast<ScintillaQtDirectBackend*>(Text.Backend()); QVERIFY(Backend->applyLanguage("C++"));
        QSignalSpy Changed(Backend, &ITextEditorBackend::textChanged); const auto Content = Text.Content();
        Backend->Send(SCI_MARKERADD, 0, 1); Backend->Send(SCI_SETFOLDLEVEL, 0, SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
        Text.FoldAll(true); QVERIFY(!Backend->Send(SCI_GETLINEVISIBLE, 1));
        Text.FoldAll(false); QVERIFY(Backend->Send(SCI_GETLINEVISIBLE, 1));
        QCOMPARE(Changed.count(), 0); QCOMPARE(Text.Content(), Content); QVERIFY(!Text.isModified());
        QVERIFY(DocumentShortcutRegistry::Validate({}).isEmpty());
    }
    void brokerMultipleSelectionsAndStaleReply()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        Write(Temp.filePath("sample.txt"), "one\ntwo\n");
        DocumentWorkspaceWidget Window(Temp.filePath("recovery")); Window.SetClipboardBroker(true);
        const auto Id = Window.Handle({{"action", "open"}, {"path", Temp.filePath("sample.txt")}})["documentId"].toString(); QTRY_VERIFY(Window.IsIdle());
        auto* Text = Window.findChild<QTextView*>(); auto* Backend = static_cast<ScintillaQtDirectBackend*>(Text->Backend());
        Backend->Send(SCI_SETSELECTION, 3, 0); Backend->Send(SCI_ADDSELECTION, 7, 4);
        Window.Handle({{"action", "clipboard"}, {"documentId", Id}, {"mode", "paste"}});
        QJsonValue Token = Window.Snapshot()["clipboard"].toObject()["token"];
        Window.Handle({{"action", "clipboardResult"}, {"token", Token}, {"accepted", true}, {"text", "X"}});
        QCOMPARE(Text->Content(), QString("X\nX\n")); Backend->Send(SCI_UNDO); QCOMPARE(Text->Content(), QString("one\ntwo\n"));
        Backend->Send(SCI_SETSELECTION, 3, 0); Backend->Send(SCI_ADDSELECTION, 7, 4);
        Window.Handle({{"action", "clipboard"}, {"documentId", Id}, {"mode", "cut"}});
        auto Clipboard = Window.Snapshot()["clipboard"].toObject(); QCOMPARE(Clipboard["text"].toString(), QString("one\r\ntwo"));
        Window.Handle({{"action", "clipboardResult"}, {"token", Clipboard["token"]}, {"accepted", true}});
        QCOMPARE(Text->Content(), QString("\n\n")); Backend->Send(SCI_UNDO);
        Window.Handle({{"action", "clipboard"}, {"documentId", Id}, {"mode", "paste"}}); Token = Window.Snapshot()["clipboard"].toObject()["token"];
        Backend->setCursorPosition(0, 0); Backend->replace("new"); const auto Before = Text->Content();
        Window.Handle({{"action", "clipboardResult"}, {"token", Token}, {"accepted", true}, {"text", "stale"}}); QCOMPARE(Text->Content(), Before);
    }
    void hostedSettingsAreVisibleAndNonBlocking()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        auto* Window = new DocumentWorkspaceWidget(Temp.filePath("recovery"), true); Window->show();
        auto* Action = Window->findChild<QAction*>("documentPreferences"); QVERIFY(Action);
        Action->trigger();
        QPointer<DocumentSettingsDialog> Dialog;
        for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Settings = qobject_cast<DocumentSettingsDialog*>(Widget)) Dialog = Settings;
        QVERIFY(Dialog); QVERIFY(Dialog->isVisible()); QVERIFY(!Dialog->parentWidget()); QCOMPARE(Dialog->windowModality(), Qt::NonModal);
        QVERIFY(!QApplication::activeModalWidget()); QVERIFY(Window->isEnabled());
        Action->trigger(); QVERIFY(Dialog->isVisible()); Dialog->reject(); QTRY_VERIFY(Dialog.isNull());
        Action->trigger(); for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Settings = qobject_cast<DocumentSettingsDialog*>(Widget)) Dialog = Settings;
        QVERIFY(Dialog); delete Window; QVERIFY(Dialog.isNull());
    }
    void dialogCancelApplyAndLiveDocuments()
    {
        QTemporaryDir Temp; qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Restore = qScopeGuard([] { qApp->setProperty("documentViewsConfigDir", QVariant()); });
        const auto Path = UnifiedConfig::FileName(Temp.path());
        Write(Temp.filePath("sample.txt"), "base\r\ntext\r\n");
        DocumentWorkspaceWidget Window(Temp.filePath("recovery")); Window.show();
        const auto Id = Window.Handle({{"action", "open"}, {"path", Temp.filePath("sample.txt")}})["documentId"].toString(); QVERIFY(!Id.isEmpty());
        QTRY_VERIFY(Window.IsIdle()); auto* Text = Window.findChild<QTextView*>(); QVERIFY(Text);
        Window.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "edited\r\ntext\r\n"}});
        const auto Content = Text->Content(); const auto Before = Read(Path);
        { DocumentSettingsDialog Dialog(Path, Window.findChildren<QAction*>()); QCOMPARE(Dialog.findChild<QListWidget*>("settingsCategories")->count(), 6);
          Dialog.findChild<QSpinBox*>("documentViews/textView/tabWidth")->setValue(7); Dialog.reject(); }
        QCOMPARE(Read(Path), Before);
        DocumentSettingsDialog Dialog(Path, Window.findChildren<QAction*>());
        Dialog.findChild<QSpinBox*>("documentViews/textView/tabWidth")->setValue(7);
        Dialog.findChild<QFontComboBox*>("documentViews/textView/fontFamily")->setCurrentFont(QFont("Consolas"));
        Dialog.findChild<QSpinBox*>("documentViews/textView/fontSize")->setValue(15);
        { auto* Mode = Dialog.findChild<QComboBox*>("appearance/documentWorkspace/themeMode"); Mode->setCurrentIndex(Mode->findData("dark")); }
        QVERIFY(Dialog.Apply()); QTRY_COMPARE(Text->tabWidth(), 7); QCOMPARE(Text->Content(), Content); QVERIFY(static_cast<ScintillaQtDirectBackend*>(Text->Backend())->Send(SCI_CANUNDO));
        QCOMPARE(Text->editorFont().pointSize(), 15);
        QTRY_COMPARE(Window.ChromeSettings()["themeMode"].toString(), QString("dark"));
        Window.ApplyChromeSettings({{"schemaVersion", 1}, {"themeMode", "dark"}}); QCOMPARE(Text->editorFont().pointSize(), 15);
        auto* Key = Dialog.findChild<QKeySequenceEdit*>("documentViews/shortcuts/bindings/file.save"); QVERIFY(Key);
        Key->setKeySequence(QKeySequence("Ctrl+O")); QVERIFY(!Dialog.Apply());
        Key->setKeySequence(QKeySequence()); QVERIFY(Dialog.Apply());
        QTRY_VERIFY(Window.findChild<QAction*>("saveDocument")->shortcut().isEmpty());
        Key->setKeySequence(QKeySequence("Ctrl+Alt+S")); QVERIFY(Dialog.Apply());
        auto* SaveAction = Window.findChild<QAction*>("saveDocument");
        QTRY_COMPARE(SaveAction->shortcut(), QKeySequence("Ctrl+Alt+S"));
        QSignalSpy Saved(SaveAction, &QAction::triggered);
        Window.activateWindow(); Text->Backend()->widget()->setFocus(); QTest::qWait(100);
        QTest::keyClick(Text->Backend()->widget(), Qt::Key_S, Qt::ControlModifier); QCOMPARE(Saved.count(), 0);
        QTest::keyClick(Text->Backend()->widget(), Qt::Key_S, Qt::ControlModifier | Qt::AltModifier); QTRY_COMPARE(Saved.count(), 1);
        QTRY_VERIFY(Window.IsIdle()); QCOMPARE(Read(Temp.filePath("sample.txt")), Content.toUtf8());
        QVERIFY(!QFileInfo::exists(Temp.filePath("document-views.ini")));
        const auto Screenshots = qEnvironmentVariable("HC_PREFERENCES_SCREENSHOT_DIR");
        if (!Screenshots.isEmpty()) {
            QDir().mkpath(Screenshots); Dialog.show();
            for (int Page = 0; Page < 3; ++Page) {
                Dialog.findChild<QListWidget*>("settingsCategories")->setCurrentRow(Page); QTest::qWait(100);
                QVERIFY(Dialog.grab().save(QDir(Screenshots).filePath(QString("page-%1.png").arg(Page))));
                if (Page == 1) for (auto* Scroll : Dialog.findChildren<QScrollArea*>()) if (Scroll->isVisible()) {
                    Scroll->verticalScrollBar()->setValue(Scroll->verticalScrollBar()->maximum()); QTest::qWait(50);
                    QVERIFY(Dialog.grab().save(QDir(Screenshots).filePath("page-1-bottom.png")));
                }
            }
        }
    }
};
QTEST_MAIN(DocumentPreferencesTests)
#include "tstDocumentPreferences.moc"
