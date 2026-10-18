#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/document/documentSettingsDialog.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "text/QTextView.h"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "text/ColumnRulerWidget.h"
#include <QClipboard>
#include <QSaveFile>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include "thirdparty/scintilla-qt/PlatQtTextRendering.hpp"
#include "core/ExternalChangeWatcher.h"
#include "core/AppSettings.h"
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
#include <Scintilla.h>
#include <QCheckBox>
#include <QComboBox>
#include <QFontComboBox>
#include <QKeySequenceEdit>
#include <QListWidget>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>
using namespace hc::settings;
using namespace hc::document;
class DocumentPreferencesTests : public QObject {
    Q_OBJECT
    static QByteArray Read(const QString& Path) { QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return {}; return File.readAll(); }
    static void Write(const QString& Path, const QByteArray& Bytes) { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); QCOMPARE(File.write(Bytes), Bytes.size()); }
private slots:
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
        { DocumentSettingsDialog Dialog(Path, Window.findChildren<QAction*>()); QCOMPARE(Dialog.findChild<QListWidget*>("settingsCategories")->count(), 3);
          Dialog.findChild<QSpinBox*>("documentViews/textView/tabWidth")->setValue(7); Dialog.reject(); }
        QCOMPARE(Read(Path), Before);
        DocumentSettingsDialog Dialog(Path, Window.findChildren<QAction*>());
        Dialog.findChild<QSpinBox*>("documentViews/textView/tabWidth")->setValue(7);
        Dialog.findChild<QFontComboBox*>("documentViews/textView/fontFamily")->setCurrentFont(QFont("Consolas"));
        Dialog.findChild<QSpinBox*>("documentViews/textView/fontSize")->setValue(15);
        Dialog.findChild<QComboBox*>("appearance/documentWorkspace/themeMode")->setCurrentIndex(2);
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
