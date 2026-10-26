#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <QtTest>
#include "text/QTextView.h"
#include "commonLibs/layout/productLayout.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QTemporaryDir>
#include <QMenuBar>
#include <QMenu>
#include <QWidgetAction>
#include <QInputMethodEvent>
#include <QCryptographicHash>
#include <QMouseEvent>
#include "commonLibs/document/documentHostProtocol.hpp"
#include <QToolBar>
#include <QTabWidget>
#include <QMessageBox>
#include <QLabel>
#include <QLockFile>
#include <QScopeGuard>
#include <QScreen>
#include <iostream>
#include <Windows.h>
#include "commonLibs/document/documentWorkspace.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/documentViewPane.hpp"
#include "UniqueLibs/document/documentRecoveryStore.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include <fmstyle/ThemeManager.h>

using namespace hc::document;
using namespace hc::storage;
namespace
{
    bool Put(const QString& Path, const QByteArray& Bytes)
    { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    QByteArray Bytes(const QString& Path)
    { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    QJsonObject Document(const DocumentWorkspaceWidget& Widget, const QString& Id)
    { for (auto Item : Widget.Snapshot()["documents"].toArray()) if (Item.toObject()["documentId"].toString() == Id) return Item.toObject(); return {}; }
    bool Ready(const DocumentWorkspaceWidget& Widget, const QString& Id)
    { return Document(Widget, Id)["lifecycle"].toString() == "ready"; }
    class Lease final : public IMaterializedFileLease
    {
    public:
        std::string LocalPath() const override { return "lease.txt"; }
        EntrySnapshot Describe() const override { return {}; }
        std::size_t Depth() const override { return 0; }
    };
    QJsonObject Native(HWND Viewer, HMODULE Module, const QJsonObject& Request)
    {
        using Call = BOOL (*)(HWND, const char*, UINT, char*, UINT*);
        auto Invoke = reinterpret_cast<Call>(GetProcAddress(Module, "HC_DocumentCommand"));
        auto Input = QJsonDocument(Request).toJson(QJsonDocument::Compact); QByteArray Output(768 * 1024, Qt::Uninitialized); UINT Size = Output.size();
        if (!Invoke || !Invoke(Viewer, Input.constData(), Input.size(), Output.data(), &Size)) return {{"error", "not-ready"}};
        Output.resize(Size); return QJsonDocument::fromJson(Output).object();
    }
}
class DocumentP1Tests final : public QObject
{
    Q_OBJECT
private slots:
    void sessionRevisionCancellationAndLifetime()
    {
        SourceIdentity Source; Source.localPath_ = "source.txt"; Source.originalWritable_ = true;
        Source.lease_ = std::make_shared<Lease>(); std::weak_ptr<IMaterializedFileLease> Weak = Source.lease_;
        DocumentSession Session("stable", Source);
        QVERIFY(Session.CompleteOpen(1, OpenedDocument{Source, "initial"}));
        QVERIFY(Session.Execute("document.setText", "revision one").HasValue());
        auto Started = Session.Save({SaveDisposition::Original, "source.txt"}); QVERIFY(Started.HasValue());
        auto Ticket = Started.Value(); Source.lease_.reset();
        QVERIFY(Session.Execute("document.setText", "revision two").HasValue());
        Session.Cancel(); QVERIFY(Session.Snapshot().cancelRequested_);
        QVERIFY(!Session.Close(CloseDisposition::Discard).HasValue());
        QVERIFY(Session.CompleteSave(Ticket, {true, {}, {}, {{ErrorCategory::Unavailable, "cleanup"}}}));
        QVERIFY(Session.Snapshot().dirty_); QCOMPARE(Session.Snapshot().savedRevision_, Ticket.revision_);
        QCOMPARE(Session.Snapshot().cleanupErrors_.size(), std::size_t(1)); QVERIFY(!Session.Snapshot().error_);
        QVERIFY(!Session.CompleteSave(Ticket, {true}));
        QVERIFY(Session.Execute("document.setText", "revision one").HasValue()); QVERIFY(!Session.Snapshot().dirty_);
        QVERIFY(Session.Close(CloseDisposition::KeepChanges).HasValue()); QVERIFY(!Session.Capabilities().edit_);
        QVERIFY(!Session.CompleteOpen(1, OpenedDocument{Source, "late"})); QVERIFY(!Weak.expired()); // immutable ticket holds its lease
        Ticket.source_.lease_.reset(); Started.Value().source_.lease_.reset(); QVERIFY(Weak.expired());
    }
    void failedOpenAndSaveRetryConflict()
    {
        DocumentSession Session("stable");
        QVERIFY(Session.CompleteOpen(1, Error{ErrorCategory::AccessDenied, "denied"}));
        QCOMPARE(Session.Snapshot().lifecycle_, DocumentLifecycle::Failed); QVERIFY(Session.Capabilities().close_);
        auto Generation = Session.BeginOpen(); QVERIFY(Generation.HasValue());
        SourceIdentity Source; Source.localPath_ = "source.txt"; Source.originalWritable_ = true;
        QVERIFY(!Session.CompleteOpen(1, OpenedDocument{Source, "stale"}));
        QVERIFY(Session.CompleteOpen(Generation.Value(), OpenedDocument{Source, "base"}));
        QVERIFY(Session.Execute("document.setText", "dirty").HasValue());
        auto Ticket = Session.Save({SaveDisposition::Original, "source.txt"}); QVERIFY(Ticket.HasValue());
        QVERIFY(Session.CompleteSave(Ticket.Value(), {false, {}, Error{ErrorCategory::StaleRevision, "changed"}}));
        QCOMPARE(Session.Snapshot().saveState_, DocumentSaveState::Conflict); QVERIFY(Session.Capabilities().save_);
        QVERIFY(!Session.BeginOpen().HasValue()); QVERIFY(!Session.Close(CloseDisposition::KeepChanges).HasValue());
        auto Retry = Session.Save({SaveDisposition::SaveAs, "new.txt"}); QVERIFY(Retry.HasValue());
        Source.localPath_ = "new.txt";
        QVERIFY(Session.CompleteSave(Retry.Value(), {true, Source}));
        QVERIFY(!Session.Snapshot().dirty_); QCOMPARE(Session.Snapshot().source_.localPath_, std::string("new.txt"));
        QVERIFY(!Session.Execute("unknown", "text").HasValue());
        QVERIFY(!Session.Execute("document.setText", std::string("\xff", 1)).HasValue());
        QVERIFY(!Session.Execute("document.setText", std::string("\xed\xa0\x80", 3)).HasValue());
        QCOMPARE(Session.Text(), std::string("dirty"));
    }
    void openingCancellationRejectsLateCompletions()
    {
        QTemporaryDir Root; auto Path = Root.filePath("source.txt"); QVERIFY(Put(Path, "base"));
        DocumentWorkspaceWidget Widget(Root.filePath("recovery"));
        auto Id = Widget.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString();
        Widget.Handle({{"action", "reload"}, {"documentId", Id}});
        QVERIFY(!Widget.Handle({{"action", "close"}, {"documentId", Id}, {"discard", true}}).contains("error"));
        auto New = Widget.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString(); QVERIFY(New != Id);
        QTRY_VERIFY(Ready(Widget, New)); QVERIFY(Document(Widget, Id).isEmpty());
        const auto State = Document(Widget, New);
        QVERIFY(!Widget.Handle({{"action", "replaceText"}, {"documentId", New}, {"generation", State["generation"]}, {"revision", State["revision"]}, {"text", "current"}}).contains("error"));
        QVERIFY(Widget.Handle({{"action", "replaceText"}, {"documentId", New}, {"revision", State["revision"]}, {"text", "stale"}}).contains("error"));
        Widget.Handle({{"action", "reload"}, {"documentId", New}, {"discard", true}});
        QVERIFY(Widget.Handle({{"action", "cancel"}, {"documentId", New}, {"generation", State["generation"]}}).contains("error"));
        QTRY_VERIFY(Ready(Widget, New));
        QCOMPARE(Widget.Snapshot()["documents"].toArray().size(), 1);
        QVERIFY(Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "late"}}).contains("error"));
    }
    void workspaceActivationAndRetirement()
    {
        DocumentWorkspace Workspace;
        auto One = std::make_shared<DocumentSession>("one"), Two = std::make_shared<DocumentSession>("two");
        QVERIFY(Workspace.Add(One).HasValue()); QVERIFY(!Workspace.Add(One).HasValue()); QVERIFY(Workspace.Add(Two).HasValue());
        QVERIFY(Workspace.Activate("one").HasValue()); QCOMPARE(Workspace.ActiveId(), std::string("one"));
        QVERIFY(Workspace.Remove("one", CloseDisposition::Discard).HasValue());
        QVERIFY(!Workspace.Find("one")); QCOMPARE(Workspace.ActiveId(), std::string("two"));
        QVERIFY(!Workspace.Add(One).HasValue()); QVERIFY(!Workspace.Activate("expired").HasValue());
    }
    void recoveryGenerationsFallbackAndCleanup()
    {
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path()); RecoverySnapshot Record;
        Record.document_.id_ = std::string(32, 'a'); Record.document_.revision_ = 3; Record.document_.savedRevision_ = 1;
        Record.document_.source_.localPath_ = "original.txt"; Record.document_.source_.sourceToken_ = "expired";
        Record.bytes_ = "first";
        auto First = Store.Store(Record); QVERIFY(First.HasValue());
        Record.document_.revision_ = 4; Record.bytes_ = "second"; auto Second = Store.Store(Record); QVERIFY(Second.HasValue());
        QVERIFY(Put(QString::fromStdString(Second.Value().snapshotPath_), "incomplete"));
        QVERIFY(Put(Root.filePath(QString::fromStdString(Record.document_.id_) + ".json"), "incomplete"));
        auto Listed = Store.List(); QVERIFY(Listed.HasValue()); QCOMPARE(Listed.Value().size(), std::size_t(1));
        QCOMPARE(Listed.Value()[0].bytes_, std::string("first")); QVERIFY(!Listed.Value()[0].document_.source_.originalWritable_);
        QVERIFY(Put(Root.filePath("orphan.snapshot.json"), "{}")); QCOMPARE(Store.List().Value().size(), std::size_t(1));
        QVERIFY(!Store.Read(Root.filePath("../outside.json").toStdString()).HasValue());
        QLockFile Lock(Root.filePath("index.lock")); QVERIFY(Lock.tryLock(0));
        auto Failed = Store.Store(Record); QVERIFY(!Failed.HasValue()); Lock.unlock();
        QVERIFY(QFile::exists(QString::fromStdString(First.Value().snapshotPath_)));
        QVERIFY(Store.Remove(Record.document_.id_, 3).HasValue());
        QVERIFY(!QFile::exists(QString::fromStdString(First.Value().snapshotPath_)));
    }
    void localSaveConflictSaveAsRetryAndReopen()
    {
        QTemporaryDir Root; auto Path = Root.filePath("source.txt"); QVERIFY(Put(Path, "base\r\n"));
        DocumentWorkspaceWidget Widget(Root.filePath("recovery")); Widget.show();
        auto Id = Widget.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
        Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "edited\n"}});
        QVERIFY(Put(Path, "external\r\n")); Widget.Handle({{"action", "save"}, {"documentId", Id}});
        QTRY_COMPARE(Document(Widget, Id)["saveState"].toString(), QString("conflict")); QCOMPARE(Bytes(Path), QByteArray("external\r\n"));
        QVERIFY(Widget.findChild<QAction*>("saveDocument")->isEnabled());
        auto Output = Root.filePath("copy.txt"); QVERIFY(Put(Output, "existing\n"));
        Widget.Handle({{"action", "saveAs"}, {"documentId", Id}, {"path", Output}});
        QTRY_COMPARE(Document(Widget, Id)["saveState"].toString(), QString("conflict")); QCOMPARE(Bytes(Output), QByteArray("existing\n"));
        Widget.Handle({{"action", "saveAs"}, {"documentId", Id}, {"path", Path + "/invalid-child.txt"}});
        QTRY_COMPARE(Document(Widget, Id)["saveState"].toString(), QString("failed"));
        QVERIFY(Document(Widget, Id)["dirty"].toBool());
        Widget.Handle({{"action", "saveAs"}, {"documentId", Id}, {"path", Output}, {"overwrite", true}});
        QTRY_VERIFY(!Document(Widget, Id)["saving"].toBool()); QVERIFY(Document(Widget, Id)["error"].toString().isEmpty());
        QCOMPARE(Bytes(Output), QByteArray("edited\r\n")); QVERIFY(!Document(Widget, Id)["dirty"].toBool());
        Widget.Handle({{"action", "reload"}, {"documentId", Id}}); QTRY_VERIFY(Ready(Widget, Id));
        QCOMPARE(Widget.findChild<DocumentViewPane*>()->Content(), QByteArray("edited\r\n"));
        Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "overwrite source\n"}});
        QVERIFY(Put(Output, "another external\n")); Widget.Handle({{"action", "save"}, {"documentId", Id}});
        QTRY_COMPARE(Document(Widget, Id)["saveState"].toString(), QString("conflict"));
        Widget.Handle({{"action", "save"}, {"documentId", Id}, {"overwrite", true}});
        QTRY_VERIFY(!Document(Widget, Id)["saving"].toBool()); QCOMPARE(Bytes(Output), QByteArray("overwrite source\r\n"));
    }
    void unicodeScalarsAndUndoRoundTrip()
    {
        QTemporaryDir Root; auto Path = Root.filePath("unicode.txt");
        const auto Initial = QString("A\u00a0B\u2028C\r\n").toUtf8(); QVERIFY(Put(Path, Initial));
        DocumentWorkspaceWidget Widget(Root.filePath("recovery"));
        auto Id = Widget.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
        const auto Changed = QString("A\u00a0B\u2028C changed\n");
        Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", Changed}});
        Widget.Handle({{"action", "undo"}, {"documentId", Id}}); QVERIFY(!Document(Widget, Id)["dirty"].toBool());
        Widget.Handle({{"action", "redo"}, {"documentId", Id}}); Widget.Handle({{"action", "save"}, {"documentId", Id}});
        QTRY_VERIFY(!Document(Widget, Id)["saving"].toBool()); QCOMPARE(Bytes(Path), QString("A\u00a0B\u2028C changed\r\n").toUtf8());
        QVERIFY(!Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", QString("A\u2029B")}}).contains("error"));
        Widget.Handle({{"action","undo"},{"documentId",Id}});
        QVERIFY(Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", QString(QChar(0xd800))}}).contains("error"));
        QVERIFY(!Document(Widget, Id)["dirty"].toBool());
        auto Unsupported = Root.filePath("paragraph.txt"); QVERIFY(Put(Unsupported, QString("A\u2029B").toUtf8()));
        QVERIFY(!ReadTextFile(Unsupported).HasValue());
    }
    void sourceAuthorityAndPermissionRefresh()
    {
        QTemporaryDir Root; auto Path = Root.filePath("readonly.txt"); QVERIFY(Put(Path, "base"));
        const auto Permissions = QFile::permissions(Path); auto Restore = qScopeGuard([&] { QFile::setPermissions(Path, Permissions); });
        QVERIFY(QFile::setPermissions(Path, QFileDevice::ReadOwner | QFileDevice::ReadUser));
        DocumentWorkspaceWidget Widget(Root.filePath("recovery"), true);
        auto Id = Widget.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
        QVERIFY(!Document(Widget, Id)["canSave"].toBool()); QVERIFY(Document(Widget, Id)["canSaveAs"].toBool());
        QVERIFY(QFile::setPermissions(Path, Permissions)); Widget.Handle({{"action", "reload"}, {"documentId", Id}}); QTRY_VERIFY(Ready(Widget, Id));
        QVERIFY(Document(Widget, Id)["canSave"].toBool());
        Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "local inside helper"}});
        Widget.Handle({{"action", "save"}, {"documentId", Id}}); QTRY_VERIFY(!Document(Widget, Id)["saving"].toBool());
        QCOMPARE(Bytes(Path), QByteArray("local inside helper"));
        const auto Slot = Widget.Handle({{"action", "openHost"}, {"path", Path}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Slot));
        QVERIFY(!Document(Widget, Slot)["canSave"].toBool()); QVERIFY(Document(Widget, Slot)["canSaveAs"].toBool());
    }
    void closeCancelSaveAndDiscardRecovery()
    {
        QTemporaryDir Root; QVERIFY(Put(Root.filePath("one.txt"), "one")); QVERIFY(Put(Root.filePath("two.txt"), "two"));
        DocumentWorkspaceWidget Widget(Root.filePath("recovery")); Widget.show();
        auto One = Widget.Handle({{"action", "open"}, {"path", Root.filePath("one.txt")}})["documentId"].toString();
        auto Two = Widget.Handle({{"action", "open"}, {"path", Root.filePath("two.txt")}})["documentId"].toString();
        QTRY_VERIFY(Ready(Widget, One) && Ready(Widget, Two));
        Widget.Handle({{"action", "replaceText"}, {"documentId", One}, {"text", "dirty one"}});
        Widget.Handle({{"action", "replaceText"}, {"documentId", Two}, {"text", "dirty two"}});
        Widget.Checkpoint(); QTRY_VERIFY(Widget.IsIdle());
        Widget.SetClosePrompt([](const QString&) { return int(QMessageBox::Cancel); });
        QVERIFY(!Widget.RequestClose()); QCOMPARE(Widget.Snapshot()["documents"].toArray().size(), 2);
        Widget.SetClosePrompt([](const QString&) { return int(QMessageBox::NoButton); });
        QVERIFY(!Widget.RequestClose()); QCOMPARE(Widget.Snapshot()["documents"].toArray().size(), 2);
        QCOMPARE(Bytes(Root.filePath("one.txt")), QByteArray("one")); QCOMPARE(Bytes(Root.filePath("two.txt")), QByteArray("two"));
        Widget.SetClosePrompt([Two](const QString& Id) { return int(Id == Two ? QMessageBox::Save : QMessageBox::Cancel); });
        QVERIFY(!Widget.RequestClose()); QTRY_VERIFY(Document(Widget, Two).isEmpty());
        QCOMPARE(Bytes(Root.filePath("two.txt")), QByteArray("dirty two")); QVERIFY(Document(Widget, One)["dirty"].toBool());
        Widget.SetClosePrompt([](const QString&) { return int(QMessageBox::Discard); }); Widget.RequestClose();
        QTRY_VERIFY(Widget.Snapshot()["documents"].toArray().isEmpty()); QTRY_VERIFY(Widget.IsIdle());
        DocumentRecoveryStore Store(Root.filePath("recovery")); QCOMPARE(Store.List().Value().size(), std::size_t(0));
    }
    void chromePaneThemeAndTablessShell()
    {
        QTemporaryDir Root; auto Path = Root.filePath("file.txt"); QVERIFY(Put(Path, "content"));
        DocumentWorkspaceWidget Widget(Root.filePath("recovery")); QVERIFY(Widget.SetTabbed(false)); Widget.show();
        const auto ExpectedDpr = qEnvironmentVariable("HC_DOCUMENT_EXPECT_DPR").toDouble();
        if (ExpectedDpr > 0) QVERIFY(qAbs(Widget.devicePixelRatioF() - ExpectedDpr) < 0.01);
        auto Id = Widget.Handle({{"action", "open"}, {"path", Path}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget, Id));
        QVERIFY(Widget.findChild<QTabWidget*>("documentTabs")->isHidden()); QVERIFY(!Widget.SetTabbed(true));
        auto* Pane = Widget.findChild<DocumentViewPane*>(); QVERIFY(Pane); QVERIFY(Pane->findChild<QToolBar*>("viewToolbar"));
        auto Settings = Widget.ChromeSettings(); Settings["mainMenuMode"] = "hide"; Settings["mainToolbarMode"] = "hide";
        QVERIFY(Widget.ApplyChromeSettings(Settings)); QVERIFY(Widget.findChild<QMenuBar*>("mainMenu")->isHidden());
        QVERIFY(Widget.findChild<QToolBar*>("mainToolbar")->isHidden()); QVERIFY(Widget.findChild<QAction*>("workspaceDisplayOptions"));
        QVERIFY(Document(Widget, Id)["canSave"].toBool()); // chrome never changes editing authority
        Widget.activateWindow(); Widget.raise(); Pane->InputWidget()->setFocus();
        Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "keyboard content"}});
        QTest::keyClick(Pane->InputWidget(), Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(!Document(Widget, Id)["saving"].toBool());
        QCOMPARE(Bytes(Path), QByteArray("keyboard content")); QVERIFY(!Document(Widget, Id)["dirty"].toBool());
        Settings["viewOverrides"] = QJsonObject{{"text", QJsonObject{{"mainMenuMode", "show"}}}};
        QVERIFY(Widget.ApplyChromeSettings(Settings)); QVERIFY(!Widget.findChild<QMenuBar*>("mainMenu")->isHidden());
        hc::settings::Repository Repository(Root.filePath("settings")); auto Appearance = Repository.Read(hc::settings::Section::Appearance).toObject();
        Appearance["documentWorkspace"] = Settings; QVERIFY(Repository.Put(hc::settings::Section::Appearance, Appearance));
        hc::settings::Repository Reloaded(Root.filePath("settings")); QCOMPARE(Reloaded.Object()["schemaVersion"].toInt(), 1);
        QCOMPARE(Reloaded.Read(hc::settings::Section::Appearance).toObject()["documentWorkspace"].toObject(), Settings);
        auto& Theme = fm::style::ThemeManager::instance(); auto Scheme = Theme.scheme(); auto Family = Theme.monoFontFamily(); auto Size = Theme.monoFontPx();
        auto Restore = qScopeGuard([&] { Theme.setScheme(Scheme); Theme.setMonoFont(Family, Size); });
        Theme.setScheme(fm::style::ThemeManager::Scheme::Dark); Theme.setMonoFont("Consolas", 15);
        QCOMPARE(Pane->findChild<QTextView*>()->editorFont().family(), QString("Consolas")); QCOMPARE(Pane->Content(), QByteArray("keyboard content")); QVERIFY(!Document(Widget, Id)["dirty"].toBool());
        QVERIFY(Pane->findChildren<QPlainTextEdit*>().isEmpty());
        QVERIFY(!Widget.Handle({{"action", "open"}, {"path", Root.filePath("other.txt")}})["documentId"].isString());
    }
    void recoveryListAndSaveAsOnlyRestoration()
    {
        QTemporaryDir Root; const auto Recovery = Root.filePath("recovery"); QVERIFY(Put(Root.filePath("file.txt"), "base"));
        DocumentWorkspaceWidget Widget(Recovery); auto Id = Widget.Handle({{"action", "open"}, {"path", Root.filePath("file.txt")}})["documentId"].toString();
        QTRY_VERIFY(Ready(Widget, Id)); Widget.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "recovered 한글"}});
        Widget.Checkpoint(); QTRY_VERIFY(Widget.IsIdle()); Widget.Handle({{"action", "recoveryList"}});
        QTRY_VERIFY(!Widget.Snapshot()["recoveryList"].toObject()["loading"].toBool());
        auto Entries = Widget.Snapshot()["recoveryList"].toObject()["entries"].toArray(); QCOMPARE(Entries.size(), 1);
        auto Restored = Widget.Handle({{"action", "recover"}, {"path", Entries[0].toObject()["path"]}})["documentId"].toString(); QVERIFY(!Restored.isEmpty());
        QTRY_VERIFY(Ready(Widget, Restored)); QVERIFY(!Document(Widget, Restored)["canSave"].toBool()); QVERIFY(Document(Widget, Restored)["canSaveAs"].toBool());
        Widget.Handle({{"action", "saveAs"}, {"documentId", Restored}, {"path", Root.filePath("restored.txt")}});
        QTRY_VERIFY(QFile::exists(Root.filePath("restored.txt"))); QCOMPARE(Bytes(Root.filePath("restored.txt")), QString("recovered 한글").toUtf8());
    }
    void visibleInputAndStableMenus_data()
    {
        QTest::addColumn<bool>("Hosted");
        QTest::newRow("f4") << false;
        QTest::newRow("f3-helper") << true;
    }
    void visibleInputAndStableMenus()
    {
        QFETCH(bool, Hosted);
        QTemporaryDir Root; const auto Path=Root.filePath("keyboard.txt"); QVERIFY(Put(Path,"base"));
        auto& Theme=fm::style::ThemeManager::instance(); const auto Scheme=Theme.scheme();
        const auto Restore=qScopeGuard([&]{Theme.setScheme(Scheme);});
        Theme.setScheme(fm::style::ThemeManager::Scheme::Dark);
        DocumentWorkspaceWidget Widget(Root.filePath("recovery"),Hosted); Widget.resize(1200,600); Widget.show();
        const auto Id=Widget.Handle({{"action",Hosted?"openHost":"open"},{"path",Path}})["documentId"].toString();
        QTRY_VERIFY(Ready(Widget,Id));
        auto* Pane=Widget.findChild<DocumentViewPane*>(); QVERIFY(Pane); auto* Input=Pane->InputWidget(); QVERIFY(Input);
        Widget.Handle({{"action","focus"}}); QCOMPARE(Widget.focusProxy(),Input);
        QVERIFY(Pane->ViewState()["inputFocused"].toBool());
        auto* Bar=Widget.findChild<QMenuBar*>("mainMenu"); QVERIFY(Bar);
        auto* Menu=Bar->actions().last()->menu(); QVERIFY(Menu);
        auto* Toolbar=Pane->findChild<QToolBar*>("viewToolbar"); QVERIFY(Toolbar);
        QPointer<QMenu> Stable=Menu; QAction* Lock=nullptr; QAction* MenuLock=nullptr;
        int Controls=0;
        for(auto* Action:Menu->actions()) if(!Action->isSeparator()){
            QVERIFY2(!Action->text().trimmed().isEmpty(),"blank text menu row");
            QVERIFY(!qobject_cast<QWidgetAction*>(Action));
            if(Action->menu())++Controls;
            if(Action->text().contains("읽기 전용"))MenuLock=Action;
        }
        QVERIFY(Controls>=4); QVERIFY(MenuLock);
        for(auto* Action:Toolbar->actions()){
            const auto Key=Action->property("mv.toolbarActionKey").toString();
            if(Key=="text.readOnly")Lock=Action;
            if(Key=="text.font"||Key=="text.wordWrap"||Key=="text.readOnly"){
                const auto Image=Action->icon().pixmap(QSize(24,24),QIcon::Normal,QIcon::Off).toImage();
                QVERIFY(!Image.isNull()); int Bright=0;
                for(int Y=0;Y<Image.height();++Y)for(int X=0;X<Image.width();++X){
                    const auto Color=Image.pixelColor(X,Y);
                    if(Color.alpha()>100&&qGray(Color.rgb())>100)++Bright;
                }
                QVERIFY2(Bright>10,qPrintable(Key+" dark icon is invisible"));
            }
            if(auto* WidgetAction=qobject_cast<QWidgetAction*>(Action))
                if(auto* Control=WidgetAction->defaultWidget())QVERIFY(Toolbar->isAncestorOf(Control));
        }
        QVERIFY(Lock); QVERIFY(!Lock->isChecked());
        QTest::keyClick(Input,Qt::Key_End,Qt::ControlModifier); QTest::keyClicks(Input," typed");
        QCOMPARE(Pane->Content(),QString("base typed")); QVERIFY(Document(Widget,Id)["dirty"].toBool());
        QInputMethodEvent Commit; Commit.setCommitString(QString::fromUtf8(" 한글"));
        QApplication::sendEvent(Input,&Commit); QCOMPARE(Pane->Content(),QString::fromUtf8("base typed 한글"));
        QCOMPARE(Stable.data(),Bar->actions().last()->menu());
        MenuLock->trigger(); QVERIFY(Lock->isChecked()); QVERIFY(Pane->ViewState()["readOnly"].toBool());
        QTest::keyClicks(Input,"blocked"); QCOMPARE(Pane->Content(),QString::fromUtf8("base typed 한글"));
        Lock->trigger(); QVERIFY(!MenuLock->isChecked()); QTest::keyClicks(Input,"!");
        QCOMPARE(Pane->Content(),QString::fromUtf8("base typed 한글!"));
        Widget.Checkpoint(); QTRY_VERIFY(Widget.IsIdle()); QCOMPARE(Stable.data(),Bar->actions().last()->menu());
        auto* FileMenu=Bar->actions().first()->menu(); QVERIFY(FileMenu);
        QTest::mouseClick(Bar,Qt::LeftButton,Qt::NoModifier,Bar->actionGeometry(Bar->actions().first()).center());
        QTRY_VERIFY(FileMenu->isVisible());
        const auto Hover=Bar->actionGeometry(Bar->actions().last()).center();
        QMouseEvent Move(QEvent::MouseMove,Hover,Bar->mapToGlobal(Hover),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(Bar,&Move);
        QTRY_VERIFY(Menu->isVisible()); QVERIFY(!FileMenu->isVisible());
        const auto Capture=qEnvironmentVariable("HC_DOCUMENT_UI_CAPTURE");
        if(!Capture.isEmpty()){QVERIFY(Widget.grab().save(Capture+(Hosted?"/f3-workspace.png":"/f4-workspace.png")));QVERIFY(Menu->grab().save(Capture+(Hosted?"/f3-menu.png":"/f4-menu.png")));}
        Menu->close();
        Widget.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);}); Widget.RequestClose();
    }
    void sdkBindingReusesOnlyUnchangedView()
    {
        QTemporaryDir Root; const auto Path=Root.filePath("snapshot.txt"); const QByteArray Original="sdk snapshot"; QVERIFY(Put(Path,Original));
        DocumentWorkspaceWidget Widget(Root.filePath("recovery"),true);
        const auto Id=Widget.Handle({{"action","openHost"},{"path",Path}})["documentId"].toString(); QTRY_VERIFY(Ready(Widget,Id));
        auto* Pane=Widget.findChild<DocumentViewPane*>(); QPointer<QWidget> Input=Pane->InputWidget();
        quint64 Sequence=0; QString Workspace;
        const auto Send=[&](QString Type,QJsonObject Payload){
            auto Result=Widget.HandleMessage({{"protocolVersion",int(HostVersion)},{"requestId",QString::number(++Sequence)},{"workspaceId",Workspace},
                {"messageType",Type},{"payloadLength",QJsonDocument(Payload).toJson(QJsonDocument::Compact).size()},{"payload",Payload}});
            Workspace=Result["workspaceId"].toString(); return Result;
        };
        QCOMPARE(Send("Hello",{})["messageType"].toString(),QString("Ready"));
        const auto DisplayName = QString::fromUtf8("원본 문서.md");
        QJsonObject Binding{{"path",Path},{"displayName",DisplayName},{"hostSlot",true},{"broker",true},{"sourceToken",QString(32,'a')},{"bindExistingHost",true},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(Original,QCryptographicHash::Sha256).toHex())}};
        const auto Result=Send("OpenDocuments",{{"documents",QJsonArray{Binding}}})["payload"].toObject()["documents"].toArray()[0].toObject();
        QVERIFY(Result["boundExistingHost"].toBool()); QCOMPARE(Result["documentId"].toString(),Id);
        QCOMPARE(Pane->InputWidget(),Input.data()); QCOMPARE(Widget.Snapshot()["documents"].toArray().size(),1);
        QVERIFY(Document(Widget,Id)["canSave"].toBool());
        auto* Tabs=Widget.findChild<QTabWidget*>("documentTabs"); QVERIFY(Tabs);
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()),DisplayName);
        QCOMPARE(Document(Widget,Id)["path"].toString(),Path);
        Widget.Handle({{"action","reload"},{"documentId",Id}}); QTRY_VERIFY(Widget.IsIdle());
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()),DisplayName);
        auto Invalid=Binding; Invalid["displayName"]="../wrong.txt";
        QCOMPARE(Send("OpenDocuments",{{"documents",QJsonArray{Invalid}}})["messageType"].toString(),QString("Error"));
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()),DisplayName);
        // The SDK can reload this slot; a mismatched snapshot must take the full open path.
        Widget.Handle({{"action","openHost"},{"path",Path}}); QTRY_VERIFY(Widget.IsIdle());
        Binding["sha256"]=QString(64,'0'); Binding["sourceToken"]=QString(32,'b');
        const auto Reopened=Send("OpenDocuments",{{"documents",QJsonArray{Binding}}})["payload"].toObject()["documents"].toArray()[0].toObject();
        QVERIFY(!Reopened["boundExistingHost"].toBool()); QVERIFY(Reopened["documentId"].toString()!=Id);
        QTRY_VERIFY(Widget.IsIdle());
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()),DisplayName);
        const auto NewId=Reopened["documentId"].toString();
        const auto Copy=Root.filePath("renamed.txt");
        QVERIFY(!Widget.Handle({{"action","saveAs"},{"documentId",NewId},{"path",Copy}}).contains("error")); QTRY_VERIFY(Widget.IsIdle());
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()),QString("renamed.txt")); QCOMPARE(Bytes(Copy),Original);
    }
    void helperNativeKeyboardRoute()
    {
        QTemporaryDir Root; auto Path = Root.filePath("helper.txt"); QVERIFY(Put(Path, "base\n"));
        QWidget Parent; Parent.resize(800, 500); Parent.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        Parent.raise();Parent.activateWindow();SetForegroundWindow(reinterpret_cast<HWND>(Parent.winId()));
        if(!QTest::qWaitFor([&]{return GetForegroundWindow()==reinterpret_cast<HWND>(Parent.winId());},2000))
            QSKIP("Interactive foreground activation unavailable before viewer creation; run native keyboard coverage on an active desktop");
        auto Dll = QCoreApplication::applicationDirPath() + '/' + hc::layout::HyperViewerPackage + "/HyperViewer.dll";
        auto Module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Dll.utf16()), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); QVERIFY(Module);
        using Create = HWND (*)(HWND, LPRECT, DWORD);
        RECT Rect{0, 0, 800, 500}; auto Viewer = reinterpret_cast<Create>(GetProcAddress(Module, "DVP_CreateViewer"))(reinterpret_cast<HWND>(Parent.winId()), &Rect, 0); QVERIFY(Viewer);
        auto Cleanup = qScopeGuard([&] { DestroyWindow(Viewer); FreeLibrary(Module); });
        auto Name = Path.toStdWString(); QVERIFY(SendMessageW(Viewer, WM_APP + 0xf02, 0, reinterpret_cast<LPARAM>(Name.c_str())));
        QJsonObject State; QTRY_VERIFY_WITH_TIMEOUT((State = Native(Viewer, Module, {{"action", "state"}}))["documents"].toArray().size() == 1, 15000);
        QVERIFY(qAbs(State["dpr"].toDouble() - Parent.devicePixelRatioF()) < 0.01);
        auto Id = State["documents"].toArray()[0].toObject()["documentId"].toString();
        QTRY_COMPARE(Native(Viewer, Module, {{"action", "state"}})["documents"].toArray()[0].toObject()["lifecycle"].toString(), QString("ready"));
        // The SDK anchor owns a separate top-level viewer. Present it before
        // exercising the SDK focus route and real keyboard delivery.
        const auto Presented=Native(Viewer,Module,{{"action","present"}});
        QVERIFY(Presented["accepted"].toBool());
        auto Helper=reinterpret_cast<HWND>(Presented["nativeWindow"].toString().toULongLong(nullptr,16));
        QVERIFY(Helper); QVERIFY(IsWindowVisible(Helper)); QVERIFY(!GetWindow(Viewer,GW_CHILD));
        SetForegroundWindow(Helper); QTRY_COMPARE(GetForegroundWindow(),Helper);
        SendMessageW(Viewer,WM_SETFOCUS,0,0);
        QTRY_VERIFY(Native(Viewer,Module,{{"action","state"}})["documents"].toArray()[0].toObject()["view"].toObject()["inputFocused"].toBool());
        INPUT Keys[2]{}; Keys[0].type=Keys[1].type=INPUT_KEYBOARD;
        Keys[0].ki.wVk=Keys[1].ki.wVk='Z'; Keys[1].ki.dwFlags=KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(2,Keys,sizeof(INPUT)),UINT(2));
        QTest::qWait(100);
        const auto KeyboardCopy=Root.filePath("native-keyboard.txt");
        Native(Viewer,Module,{{"action","saveAs"},{"documentId",Id},{"path",KeyboardCopy}});
        QTRY_VERIFY(!Native(Viewer,Module,{{"action","state"}})["documents"].toArray()[0].toObject()["saving"].toBool());
        QTRY_VERIFY(QFile::exists(KeyboardCopy)); QVERIFY2(Bytes(KeyboardCopy).toLower().contains('z'),"native character did not reach the editor");
    }
    void helperUsesP1SessionAndConflictFlow()
    {
        QTemporaryDir Root; auto Path = Root.filePath("helper.txt"); QVERIFY(Put(Path, "base\n"));
        QWidget Parent; Parent.resize(800, 500); Parent.show();
        auto Dll = QCoreApplication::applicationDirPath() + '/' + hc::layout::HyperViewerPackage + "/HyperViewer.dll";
        auto Module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Dll.utf16()), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); QVERIFY(Module);
        using Create = HWND (*)(HWND, LPRECT, DWORD);
        RECT Rect{0, 0, 800, 500}; auto Viewer = reinterpret_cast<Create>(GetProcAddress(Module, "DVP_CreateViewer"))(reinterpret_cast<HWND>(Parent.winId()), &Rect, 0); QVERIFY(Viewer);
        auto Cleanup = qScopeGuard([&] { DestroyWindow(Viewer); FreeLibrary(Module); });
        auto Name = Path.toStdWString(); QVERIFY(SendMessageW(Viewer, WM_APP + 0xf02, 0, reinterpret_cast<LPARAM>(Name.c_str())));
        QJsonObject State; QTRY_VERIFY_WITH_TIMEOUT((State = Native(Viewer, Module, {{"action", "state"}}))["documents"].toArray().size() == 1, 15000);
        QVERIFY(qAbs(State["dpr"].toDouble() - Parent.devicePixelRatioF()) < 0.01);
        auto Id = State["documents"].toArray()[0].toObject()["documentId"].toString();
        QTRY_COMPARE(Native(Viewer, Module, {{"action", "state"}})["documents"].toArray()[0].toObject()["lifecycle"].toString(), QString("ready"));
        QVERIFY(Native(Viewer,Module,{{"action","present"}})["accepted"].toBool());
        Native(Viewer, Module, {{"action", "replaceText"}, {"documentId", Id}, {"text", "helper revision one\n"}});
        auto Copy = Root.filePath("copy.txt"); Native(Viewer, Module, {{"action", "saveAs"}, {"documentId", Id}, {"path", Copy}});
        QTRY_VERIFY(QFile::exists(Copy)); QCOMPARE(Bytes(Copy), QByteArray("helper revision one\n"));
        QTRY_VERIFY(!Native(Viewer, Module, {{"action", "state"}})["documents"].toArray()[0].toObject()["saving"].toBool());
        Native(Viewer, Module, {{"action", "replaceText"}, {"documentId", Id}, {"text", "helper revision two\n"}});
        QVERIFY(Put(Copy, "external\n")); Native(Viewer, Module, {{"action", "save"}, {"documentId", Id}});
        QTRY_COMPARE(Native(Viewer, Module, {{"action", "state"}})["documents"].toArray()[0].toObject()["saveState"].toString(), QString("conflict"));
        auto Saved = Native(Viewer, Module, {{"action", "save"}, {"documentId", Id}, {"overwrite", true}});
        QVERIFY2(!Saved.contains("error"), QJsonDocument(Saved).toJson(QJsonDocument::Compact).constData());
        // Poll owner state, not the destination: QFile readers can deny delete
        // sharing and accidentally inject a rename failure during the commit.
        QTRY_VERIFY(!Native(Viewer, Module, {{"action", "state"}})["documents"].toArray()[0].toObject()["saving"].toBool());
        const auto Completed = Native(Viewer, Module, {{"action", "state"}})["documents"].toArray()[0].toObject();
        QVERIFY2(Completed["error"].toString().isEmpty(), QJsonDocument(Completed).toJson(QJsonDocument::Compact).constData());
        QCOMPARE(Bytes(Copy), QByteArray("helper revision two\n"));
        Native(Viewer, Module, {{"action", "reload"}, {"documentId", Id}});
        QTRY_COMPARE(Native(Viewer, Module, {{"action", "state"}})["documents"].toArray()[0].toObject()["lifecycle"].toString(), QString("ready"));
    }
};
int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv);QTemporaryDir ViewConfig;App.setProperty("documentViewsConfigDir",ViewConfig.path());
    if (App.arguments().contains("--probe-dpr")) { std::cout << App.primaryScreen()->devicePixelRatio() << std::endl; return 0; }
    DocumentP1Tests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstDocumentP1.moc"
