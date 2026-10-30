#include <QtTest>
#include <QTemporaryDir>
#include <QMenu>
#include <QTimer>
#include <QProcess>
#include <atomic>
#include <thread>
#include <QApplication>
#include <QScopeGuard>
#include <QImage>
#include <QDialog>
#include <QLineEdit>
#include <QVBoxLayout>
#include <qt_windows.h>
#include "../nativeTestInput.hpp"
#include "UniqueLibs/qtViewerHost.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/document/documentOpenController.hpp"
#include "UniqueLibs/commandUiConfig.hpp"
using namespace hc::document;
using namespace hc::storage;
namespace {
HandlerDescriptor Handler(std::string Id, std::vector<std::string> Extensions = {"*"}, bool Edit = true) {
    HandlerDescriptor Result; Result.stableId_ = Id; Result.name_ = Id; Result.extensions_ = Extensions;
    Result.view_ = true; Result.edit_ = Edit; Result.editable_ = Edit; Result.available_ = true; Result.maxFiles_ = 16; return Result;
}
class Stream final : public IReadStream {
public:
    QByteArray bytes_ = "lease test"; size_t offset_ = 0;
    bool CanSeek() const override { return true; }
    Task<ReadChunk> Read(size_t Max, OperationContext) override { ReadChunk Chunk; const auto Count = std::min(Max, size_t(bytes_.size()) - offset_); auto* Start = reinterpret_cast<const std::byte*>(bytes_.constData()) + offset_; Chunk.bytes_.assign(Start, Start + Count); offset_ += Count; Chunk.eof_ = offset_ == size_t(bytes_.size()); return Ready<ReadChunk>(Chunk); }
    Task<Unit> Seek(uint64_t At, OperationContext) override { offset_ = std::min(At, uint64_t(bytes_.size())); return Ready<Unit>(Unit{}); }
};
class Content final : public IContentSource {
public:
    EntrySnapshot entry_; std::atomic<int> verifies_{0}; std::atomic<bool> release_{true}; bool changed_ = false;
    EntrySnapshot Describe() const override { return entry_; }
    Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext) override { return Ready<std::shared_ptr<IReadStream>>(std::shared_ptr<IReadStream>(std::make_shared<Stream>())); }
    Task<Unit> Verify(OperationContext Context) override {
        ++verifies_; while (!release_) { if (auto Stop = Context.Check({"test"})) return Ready<Unit>(*Stop); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        return Ready<Unit>(changed_ ? Result<Unit>(Error{ErrorCategory::StaleRevision, "test-source-changed"}) : Result<Unit>(Unit{}));
    }
};
std::shared_ptr<Content> Source(const std::string& Name = "sample.txt") {
    auto Result = std::make_shared<Content>(); Result->entry_.name_ = Name; Result->entry_.locator_ = {LocatorKind::Local, "C:/sample.txt"}; return Result;
}
DocumentHandlerCatalog Catalog(std::vector<HandlerDescriptor> Handlers) {
    DocumentHandlerCatalog Result;
    for (auto Descriptor : Handlers) { Result.registry_.Register(Descriptor); DocumentHandlerTarget Target; Target.descriptor_ = Descriptor; Result.targets_.emplace(Descriptor.stableId_, Target); }
    return Result;
}
QMenu* Menu() { return qobject_cast<QMenu*>(QApplication::activePopupWidget()); }
}
class DocumentP4Tests final : public QObject {
    Q_OBJECT
private slots:
    void builtinExclusionFallsThrough() {
        QTemporaryDir Temp; hc::settings::Repository Settings(Temp.path());
        const QJsonObject Rule{{"version", 1}, {"enabled", true}, {"mode", "excludeExtensions"}, {"extensions", QJsonArray{"PDF", "foo", "tar.gz"}}};
        auto Plugins = Settings.Read(hc::settings::Section::Plugins).toObject();
        Plugins["builtin"] = QJsonObject{{"builtin.hyperviewer", QJsonObject{{"application", Rule}}}, {"builtin.hypereditor", QJsonObject{{"application", Rule}}}};
        QVERIFY(Settings.Put(hc::settings::Section::Plugins, Plugins));
        hc::plugins::PluginViewerOptions Options; Options.binaryPath_ = QCoreApplication::applicationFilePath(); Options.executable_ = Options.binaryPath_; Options.approval_.approved_ = true;
        auto External = BuildPluginViewerTarget("external", Options, {"pdf", "foo"});
        auto Result = BuildDocumentHandlerCatalog(Settings, QCoreApplication::applicationDirPath(), {External});
        QCOMPARE(DocumentHandlerResolver::Resolve(Result.registry_, DocumentIntent::View, {{"sample.PDF"}}, "builtin.hyperviewer").candidates_, std::vector<std::string>{External.descriptor_.stableId_});
        // Content-text fallback must not reintroduce an explicitly excluded type.
        QCOMPARE(DocumentHandlerResolver::Resolve(Result.registry_, DocumentIntent::View, {{"sample.foo"}, false, true, {true}}, "builtin.hyperviewer").candidates_, std::vector<std::string>{External.descriptor_.stableId_});
        QVERIFY(!DocumentHandlerResolver::Supports(*Result.registry_.Find("builtin.hypereditor"), DocumentIntent::Edit, {"a.tar.gz"}));
        QVERIFY(DocumentHandlerResolver::Supports(*Result.registry_.Find("builtin.hypereditor"), DocumentIntent::Edit, {"a.newformat"}));
        QVERIFY(DocumentHandlerResolver::Supports(*Result.registry_.Find("builtin.hyperviewer"), DocumentIntent::View, {"a.newformat"}, {true}));
        QVERIFY(DocumentHandlerResolver::Supports(*Result.registry_.Find("builtin.hyperviewer"), DocumentIntent::View, {"book.epub"}));
    }
    void candidatesAndMixedSelection() {
        DocumentHandlerRegistry Registry; QVERIFY(Registry.Register(Handler("text", {"*.TXT", "c?"}))); QVERIFY(!Registry.Register(Handler("text")));
        QVERIFY(Registry.Register(Handler("pdf", {"pdf"}))); QVERIFY(Registry.Register(Handler("all")));
        auto Resolved = DocumentHandlerResolver::Resolve(Registry, DocumentIntent::Edit, {{"one.TXT", "two.pdf"}}, "all");
        QCOMPARE(Resolved.candidates_, std::vector<std::string>{"all"});
        Resolved = DocumentHandlerResolver::Resolve(Registry, DocumentIntent::View, {{"one.txt"}}, "all");
        QCOMPARE(Resolved.candidates_, (std::vector<std::string>{"all", "text"}));
        DocumentHandlerRegistry Split; Split.Register(Handler("text", {"txt"})); Split.Register(Handler("pdf", {"pdf"}));
        auto Mixed = DocumentHandlerResolver::Resolve(Split, DocumentIntent::View, {{"a.txt", "b.pdf", "c.unknown"}});
        QVERIFY(Mixed.candidates_.empty()); QCOMPARE(Mixed.groups_.size(), size_t(3)); QCOMPARE(Mixed.groups_.at("unknown"), std::vector<size_t>{2});
        QCOMPARE(DocumentHandlerResolver::Resolve(Split, DocumentIntent::View, {{"folder"}, true}).reason_, std::string("document-directory"));
        QCOMPARE(DocumentHandlerResolver::Resolve(Split, DocumentIntent::View, {{"a.txt"}, false, false}).reason_, std::string("document-provider-unsupported"));
        auto ReadOnly = Handler("read-only", {"*"}, false); ReadOnly.edit_ = true; QVERIFY(!DocumentHandlerResolver::Supports(ReadOnly, DocumentIntent::Edit, {"a.txt"}));
    }
    void registeredViewerFormatsAndContentText() {
        QTemporaryDir Temp; QVERIFY(Temp.isValid()); hc::settings::Repository Settings(Temp.path());
        hc::plugins::PluginViewerOptions Options; Options.binaryPath_ = QCoreApplication::applicationFilePath();
        Options.executable_ = Options.binaryPath_; Options.approval_.approved_ = true;
        auto Target = BuildPluginViewerTarget("PEViewer.wlx64", Options, {"exe", "dll"});
        auto Catalog = BuildDocumentHandlerCatalog(Settings, QCoreApplication::applicationDirPath(), {Target});
        auto Candidates = [&](const char* Name, DocumentIntent Intent) {
            return DocumentHandlerResolver::Resolve(Catalog.registry_, Intent, {{Name}}).candidates_;
        };
        QCOMPARE(Candidates("collage.png", DocumentIntent::View), std::vector<std::string>{"builtin.hyperviewer"});
        QVERIFY(Candidates("setup.inf", DocumentIntent::View).empty());
        QCOMPARE(DocumentHandlerResolver::Resolve(Catalog.registry_, DocumentIntent::View,
            {{"setup.inf"}, false, true, {true}}).candidates_, std::vector<std::string>{"builtin.hyperviewer"});
        QCOMPARE(Candidates("setup.inf", DocumentIntent::Edit), std::vector<std::string>{"builtin.hypereditor"});
        QCOMPARE(Candidates("tool.EXE", DocumentIntent::View), std::vector<std::string>{Target.descriptor_.stableId_});

        Options.applicationRule_ = QJsonObject{{"mode", "extensions"}, {"extensions", QJsonArray{"exe", "png"}}};
        Target = BuildPluginViewerTarget("PEViewer.wlx64", Options, {"exe", "dll"});
        QVERIFY(DocumentHandlerResolver::Supports(Target.descriptor_, DocumentIntent::View, {"tool.exe"}));
        QVERIFY(!DocumentHandlerResolver::Supports(Target.descriptor_, DocumentIntent::View, {"collage.png"}));
        QVERIFY(!DocumentHandlerResolver::Supports(Target.descriptor_, DocumentIntent::View, {"tool.dll"}));
    }
    void localInfUsesContentProbe() {
        QTemporaryDir Temp; QVERIFY(Temp.isValid());
        const auto Path = Temp.filePath("setup.inf");
        QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly));
        QVERIFY(File.write("[Version]\r\nSignature=\"$Windows NT$\"\r\n") > 0); File.close();
        hc::settings::Repository Settings(Temp.path());
        const auto Candidates = BuildDocumentHandlerCatalog(Settings, QCoreApplication::applicationDirPath());
        auto Input = Source("setup.inf"); Input->entry_.locator_.root_ = Path.toStdString();
        QWidget Window; int Launches = 0; QString LaunchedId;
        DocumentOpenController Controller(&Window, [&] { return Candidates; },
            [&](const auto& Target, auto, auto Completion) {
                LaunchedId = QString::fromStdString(Target.descriptor_.stableId_);
                ++Launches; Completion(true, {}); return true;
            });
        QSignalSpy Finished(&Controller, &DocumentOpenController::Finished);
        QVERIFY(Controller.Open(Controller.Capture({DocumentIntent::View, {{Input, {}}}, [] { return true; }})));
        QTRY_COMPARE(Launches, 1);
        QCOMPARE(LaunchedId, QString("builtin.hyperviewer"));
        QCOMPARE(Finished.back()[1].toString(), QString("ready"));
    }
    void migrationPreservesLegacyAndShortcutOverrides() {
        QTemporaryDir Temp; const QJsonArray Items{QJsonObject{{"name", "선택한 편집기"}, {"program", "C:/custom/editor.exe"}, {"arguments", "--line 4 \"%FILE%\""}, {"extensions", QJsonArray{"txt"}}, {"stableId", "duplicate"}},
            QJsonObject{{"name", "두 번째"}, {"program", "C:/missing/editor.exe"}, {"stableId", "duplicate"}}};
        auto Legacy = hc::settings::Defaults(); Legacy["editors"] = Items; Legacy["shortcuts"] = QJsonObject{{"F3", QJsonValue::Null}, {"Alt+V", "cm_List"}, {"F4", "hc.file.copy"}, {"Alt+E", "cm_ExternalEditorMenu"}};
        QFile File(Temp.filePath("settings.json")); QVERIFY(File.open(QIODevice::WriteOnly)); const auto Bytes = QJsonDocument(Legacy).toJson(); QCOMPARE(File.write(Bytes), Bytes.size()); File.close();
        hc::settings::Repository Settings(Temp.path()); QVERIFY(Settings.Error().isEmpty());
        const auto Migrated = Settings.Read(hc::settings::Section::Editors).toObject(); const auto Entries = Migrated["items"].toArray();
        QCOMPARE(Entries.size(), 2); QCOMPARE(Entries[0].toObject()["arguments"], Items[0].toObject()["arguments"]);
        QVERIFY(Entries[0].toObject()["stableId"] != Entries[1].toObject()["stableId"]);
        QCOMPARE(Settings.Object()["schemaVersion"].toInt(), 1); QCOMPARE(Migrated["documentHandlers"].toObject()["schemaVersion"].toInt(), 1);
        auto Expected = Legacy; hc::defaults::MigrateCommandSettings(Expected);
        QCOMPARE(Settings.Read(hc::settings::Section::Shortcuts), Expected["shortcuts"]);
        QFile Original(Temp.filePath("settings.json")); QVERIFY(Original.open(QIODevice::ReadOnly)); QCOMPARE(Original.readAll(), Bytes); Original.close();
        QVERIFY(Settings.Commit()); hc::settings::Repository Reload(Temp.path()); QCOMPARE(Reload.Object(), Settings.Object());
        QVERIFY(Reload.Put(hc::settings::Section::Editors, Items)); QCOMPARE(Reload.Read(hc::settings::Section::Editors).toObject()["documentHandlers"], Migrated["documentHandlers"]);
        const auto Keys = hc::ui::EffectiveShortcuts(Settings.Read(hc::settings::Section::General).toObject(), Legacy["shortcuts"].toObject());
        QVERIFY(!Keys.contains(QKeyCombination(Qt::Key_F3))); QCOMPARE(hc::ui::CommandId(Keys.value(QKeyCombination(Qt::Key_F4))), QString("hc.file.copy"));
        QCOMPARE(hc::ui::CommandId(Keys.value(QKeyCombination(Qt::AltModifier, Qt::Key_V))), QString("hc.viewer.open"));
        QCOMPARE(hc::ui::CommandId(Keys.value(QKeyCombination(Qt::AltModifier, Qt::Key_E))), QString("hc.file.edit"));
        const auto Defaults = hc::ui::EffectiveShortcuts({}, {}); QCOMPARE(hc::ui::CommandId(Defaults.value(QKeyCombination(Qt::Key_F3))), QString("hc.viewer.open")); QCOMPARE(hc::ui::CommandId(Defaults.value(QKeyCombination(Qt::Key_F4))), QString("hc.file.edit"));
    }
    void missingAndDuplicateApps() {
        QTemporaryDir Temp; hc::settings::Repository Settings(Temp.path());
        const auto Program = QCoreApplication::applicationFilePath();
        QVERIFY(Settings.Put(hc::settings::Section::Editors, QJsonArray{QJsonObject{{"name", "existing"}, {"program", Program}, {"extensions", QJsonArray{"txt"}}}, QJsonObject{{"name", "missing"}, {"program", Temp.filePath("missing.exe")}, {"extensions", QJsonArray{"txt"}}}}));
        const auto Result = BuildDocumentHandlerCatalog(Settings, Temp.path()); const auto Edit = DocumentHandlerResolver::Resolve(Result.registry_, DocumentIntent::Edit, {{"a.txt"}});
        QCOMPARE(Edit.candidates_.size(), size_t(1)); QVERIFY(Edit.candidates_[0].starts_with("external."));
        auto Config = Settings.Read(hc::settings::Section::Editors).toObject(); auto Child = Config["documentHandlers"].toObject(); Child["schemaVersion"] = 77; Config["documentHandlers"] = Child;
        QVERIFY(Settings.Put(hc::settings::Section::Editors, Config)); const auto Unknown = BuildDocumentHandlerCatalog(Settings, Temp.path());
        QVERIFY(DocumentHandlerResolver::Resolve(Unknown.registry_, DocumentIntent::Edit, {{"a.txt"}}).candidates_.empty()); QCOMPARE(Settings.Read(hc::settings::Section::Editors).toObject()["documentHandlers"].toObject()["schemaVersion"].toInt(), 77);
    }
    void productRhwpEditorCatalog() {
        QTemporaryDir Temp; hc::settings::Repository Settings(Temp.path());
        const auto Result = BuildDocumentHandlerCatalog(Settings, QCoreApplication::applicationDirPath());
        const auto At = Result.targets_.find("builtin.rhwp-editor");
        if (At == Result.targets_.end()) QSKIP("RHWP editor product package not configured");
        const auto& Target = At->second;
        QVERIFY(Target.rhwpEditor_); QVERIFY(Target.descriptor_.available_);
        QVERIFY(DocumentHandlerResolver::Supports(Target.descriptor_, DocumentIntent::Edit, {"a.HWP", "b.hwpx"}));
        QVERIFY(!DocumentHandlerResolver::Supports(Target.descriptor_, DocumentIntent::View, {"a.hwp"}));
        for (const auto* Name : {"a.hwp", "a.hwpx", "a.HWP", "a.HWPX"})
            QCOMPARE(DocumentHandlerResolver::Resolve(Result.registry_, DocumentIntent::Edit, {{Name}}).candidates_,
                std::vector<std::string>{"builtin.rhwp-editor"});
        QCOMPARE(DocumentOpenController::Arguments(Target.arguments_, {"C:/한글 문서.hwp"}),
            (QStringList{"--rhwp-edit", "--", QDir::toNativeSeparators("C:/한글 문서.hwp")}));
        const auto Generic = Result.registry_.Find("builtin.hypereditor"); QVERIFY(Generic);
        QVERIFY(!DocumentHandlerResolver::Supports(*Generic, DocumentIntent::Edit, {"a.HWP"}));
        QVERIFY(DocumentHandlerResolver::Supports(*Generic, DocumentIntent::Edit, {"source.unknown"}));
    }
    void productRhwpViewOnlyCatalog() {
        QTemporaryDir Temp; hc::settings::Repository Settings(Temp.path());
        const auto Result = BuildDocumentHandlerCatalog(Settings, QCoreApplication::applicationDirPath());
        const auto At = Result.targets_.find("builtin.hyperviewer");
        QVERIFY(At != Result.targets_.end());
        const auto& Target = At->second;
        QVERIFY(Target.descriptor_.available_);
        QVERIFY(DocumentHandlerResolver::Supports(Target.descriptor_, DocumentIntent::View, {"a.HWP", "b.hwpx"}));
        QVERIFY(!DocumentHandlerResolver::Supports(Target.descriptor_, DocumentIntent::Edit, {"a.hwp"}));
        QCOMPARE(Target.descriptor_.delivery_, FileDelivery::Workspace);
        QCOMPARE(Target.descriptor_.kind_, HandlerKind::Plugin);
        QVERIFY(!Result.targets_.contains("builtin.rhwp-viewer"));
        QVERIFY(DocumentHandlerResolver::Supports(Target.descriptor_,DocumentIntent::View,{"a.pdf","b.hwp","c.hwpx"}));
        for (const auto* Name : {"a.hwp", "a.hwpx", "a.HWP", "a.HWPX"})
            QCOMPARE(DocumentHandlerResolver::Resolve(Result.registry_, DocumentIntent::View, {{Name}}).candidates_,
                std::vector<std::string>{"builtin.hyperviewer"});
    }
    void zeroCandidatesAndCancelDoNoIo() {
        QWidget Window; auto Candidates = Catalog({}); int Launches = 0;
        DocumentOpenController Controller(&Window, [&] { return Candidates; }, [&](const auto&, auto, auto) { ++Launches; return true; }); QSignalSpy Finished(&Controller, &DocumentOpenController::Finished);
        auto Input = Source(); DocumentOpenRequest Request{DocumentIntent::View, {{Input, {}}}, [] { return true; }};
        QTimer::singleShot(30, [] { if (Menu()) Menu()->close(); }); auto Token = Controller.Capture(Request); QVERIFY(Controller.Open(Token)); QCOMPARE(Finished.back()[1].toString(), QString("cancelled")); QCOMPARE(Input->verifies_.load(), 0);
        Candidates = Catalog({Handler("a"), Handler("b")}); QTimer::singleShot(30, [] { if (Menu()) Menu()->close(); }); Token = Controller.Capture(Request); QVERIFY(Controller.Open(Token)); QCOMPARE(Input->verifies_.load(), 0); QCOMPARE(Launches, 0);
        Token = Controller.Capture(Request); Controller.Cancel(Token); QVERIFY(!Controller.Open(Token)); QVERIFY(!Controller.Open("untrusted-token"));
    }
    void singleAndMultipleMenuExecution() {
        QWidget Window; auto A = Handler("a"); A.kind_ = HandlerKind::Plugin; auto B = A; B.stableId_ = "b";
        Window.resize(640,480); Window.show(); QVERIFY(QTest::qWaitForWindowExposed(&Window));
        QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(Window.winId())));
        auto Candidates = Catalog({A}); int Launches = 0; std::string Selected;
        DocumentOpenController Controller(&Window, [&] { return Candidates; }, [&](const auto& Target, auto Inputs, auto Completion) { ++Launches; Selected = Target.descriptor_.stableId_; QTimer::singleShot(0, &Window, [Completion] { Completion(true, {}); }); return Inputs.size() == 1; }); QSignalSpy Finished(&Controller, &DocumentOpenController::Finished);
        auto Input = Source(); DocumentOpenRequest Request{DocumentIntent::View, {{Input, {}}}, [] { return true; }};
        QVERIFY(Controller.Open(Controller.Capture(Request))); QTRY_COMPARE(Launches, 1); QTRY_COMPARE(Finished.back()[1].toString(), QString("ready"));
        for (auto Intent : {DocumentIntent::View, DocumentIntent::Edit}) {
            Candidates = Catalog({A, B}); Candidates.defaultView_ = "b"; Candidates.defaultEdit_ = "b"; Request.intent_ = Intent;
            QTimer::singleShot(30, [] {
                QPointer<QMenu> Popup = Menu(); QVERIFY(Popup);
                const auto Close = qScopeGuard([&] { if (Popup) Popup->close(); });
                auto* Action = Popup->findChild<QAction*>("hc.document.handler.b"); QVERIFY(Action);
                QCOMPARE(Popup->defaultAction(), Action);
                // Menu placement depends on the screen edge and current pointer.
                // Select the advertised default explicitly, then exercise Enter.
                QTest::mouseMove(Popup, Popup->actionGeometry(Action).center());
                QTRY_COMPARE(Popup->activeAction(), Action); QTest::keyClick(Popup, Qt::Key_Return);
            });
            QVERIFY(Controller.Open(Controller.Capture(Request))); QTRY_COMPARE(Selected, std::string("b")); Selected.clear();
        }
    }
    void staleSourceAndContextPreventLaunch() {
        QWidget Window; auto A = Handler("a"); A.kind_ = HandlerKind::Plugin; auto Candidates = Catalog({A}); int Launches = 0; bool Current = true;
        DocumentOpenController Controller(&Window, [&] { return Candidates; }, [&](const auto&, auto, auto) { ++Launches; return true; }); QSignalSpy Finished(&Controller, &DocumentOpenController::Finished);
        auto Input = Source(); Input->changed_ = true; DocumentOpenRequest Request{DocumentIntent::View, {{Input, {}}}, [&] { return Current; }};
        auto Token = Controller.Capture(Request); QVERIFY(Controller.Open(Token)); QTRY_COMPARE(Finished.size(), 1); QCOMPARE(Finished.back()[1].toString(), QString("failed")); QCOMPARE(Launches, 0);
        Input->changed_ = false; Input->release_ = false; Token = Controller.Capture(Request); QVERIFY(Controller.Open(Token)); QTRY_VERIFY(Input->verifies_ >= 2); Current = false; Input->release_ = true; QTRY_COMPARE(Finished.size(), 2); QCOMPARE(Finished.back()[1].toString(), QString("rejected")); QCOMPARE(Launches, 0);
        Current = true; Input->release_ = false; Token = Controller.Capture(Request); QVERIFY(Controller.Open(Token)); QTRY_VERIFY(Input->verifies_ >= 3); Candidates = Catalog({}); Input->release_ = true; QTRY_COMPARE(Finished.size(), 3); QCOMPARE(Finished.back()[2].toString(), QString("document-handler-changed")); QCOMPARE(Launches, 0);
    }
    void preparedViewerAdoption() {
        QTemporaryDir Temp;hc::settings::Repository Settings(Temp.path());
        const auto RealFile=qEnvironmentVariable("HC_F3_INPUT_FILE");
        const auto Path=RealFile.isEmpty()?Temp.filePath("tiny.txt"):RealFile;
        QFile File(Path);
        if(RealFile.isEmpty()){QVERIFY(File.open(QIODevice::WriteOnly));File.write(QByteArray(369,'x'));File.close();}
        auto Provider=MakeNativeReadProvider(LocatorKind::Local);auto Entry=Provider->Resolve({LocatorKind::Local,Path.toStdString()},{}).get();QVERIFY(Entry.HasValue());
        auto Source=std::make_shared<ProviderContentSource>(Provider,Entry.Value());
        QWidget Main; QVBoxLayout Layout(&Main); QLineEdit Input; Layout.addWidget(&Input);
        Main.show(); Input.setFocus(); QVERIFY(QTest::qWaitForWindowActive(&Main));
        QtViewerHost Host(&Main,[]{return MakeNativeMaterializer(QDir::tempPath().toStdString());});
        auto Drain=qScopeGuard([&]{Host.CloseAll();while(!Host.IsIdle())QTest::qWait(10);});
        const auto Options=BuildDocumentHandlerCatalog(Settings,QCoreApplication::applicationDirPath()).targets_.at("builtin.hyperviewer").plugin_;
        SetActiveWindow(reinterpret_cast<HWND>(Main.winId()));const auto Active=GetActiveWindow();Host.PreparePlugin(Options);
        auto* Prepared=Main.findChild<QDialog*>("hc.document.prepared");QVERIFY(Prepared);QVERIFY(Prepared->isHidden());
        auto* Surface=Prepared->findChild<QtPluginViewerSurface*>();QVERIFY(Surface);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(),15000);QVERIFY(Prepared->isHidden());QVERIFY(GetActiveWindow()==Active||!GetActiveWindow());
        const auto Pid=Surface->Details().host_.processId_;
        const auto InitialToken=Surface->Details().documentState_["documents"].toArray()[0].toObject()["sourceToken"].toString();
        QVERIFY(ActivateTestWindow(reinterpret_cast<HWND>(Main.winId()))); Input.setFocus();
        bool Completed=false;QElapsedTimer Timer;Timer.start();
        auto* Opened=Host.OpenPlugin(Options,{{Source,{}}},"Prepared F3",[&](bool Ok,const QString&){Completed=Ok;});
        QCOMPARE(Opened,Prepared);QTRY_VERIFY_WITH_TIMEOUT(Completed,30000);
        qInfo()<<"prepared F3 ready ms"<<Timer.elapsed();QCOMPARE(Surface->Details().host_.processId_,Pid);
        QCOMPARE(Surface->Details().documentState_["documents"].toArray().size(),1);
        QCOMPARE(Surface->Snapshot().source_.name_,Entry.Value().name_);
        const auto Loaded=Surface->Details().documentState_["documents"].toArray()[0].toObject();
        QVERIFY(Loaded["sourceToken"].toString()!=InitialToken);QVERIFY(!Loaded["view"].toObject()["readOnly"].toBool());
        if(!RealFile.isEmpty()) {
            QCOMPARE(Loaded["view"].toObject()["viewId"].toString(),QString("image"));
            QTRY_VERIFY_WITH_TIMEOUT(!Surface->Details().documentState_["documents"].toArray()[0].toObject()["view"].toObject()["rendering"].toBool(),30000);
            QVERIFY(Surface->Details().documentState_["documents"].toArray()[0].toObject()["view"].toObject()["renderError"].toString().isEmpty());
        }
        QTRY_VERIFY_WITH_TIMEOUT(Main.findChild<QDialog*>("hc.document.prepared")!=nullptr,5000);
        if(qEnvironmentVariableIsSet("HC_F3_INTERACTIVE_TEST")) {
            // Orca enumerates unowned top-level windows. Preserve this fixture's
            // root HWND and complete embedded hierarchy while exposing its shell.
            SetWindowLongPtrW(reinterpret_cast<HWND>(Opened->winId()),GWLP_HWNDPARENT,0);
            QFile Evidence(qEnvironmentVariable("HC_F3_INTERACTIVE_TEST"));QVERIFY(Evidence.open(QIODevice::WriteOnly));
            Evidence.write(QJsonDocument(QJsonObject{{"windowId",qint64(Opened->winId())},{"pid",QCoreApplication::applicationPid()},{"file",Path}}).toJson());Evidence.close();
            QTRY_VERIFY_WITH_TIMEOUT(Surface->Details().documentState_["documents"].toArray()[0].toObject()["dirty"].toBool(),90000);
            const auto Id=Surface->Details().documentState_["activeDocumentId"].toString();auto Save=Surface->SendDocument({{"action","save"},{"documentId",Id}});QVERIFY(Save.HasValue());
            QTRY_VERIFY_WITH_TIMEOUT(Surface->Details().commandSequence_>=Save.Value(),5000);
            QTRY_VERIFY_WITH_TIMEOUT(!Surface->Details().documentState_["documents"].toArray()[0].toObject()["dirty"].toBool(),5000);
            QVERIFY(File.open(QIODevice::ReadOnly));auto Saved=File.readAll();File.close();QVERIFY(Saved.contains('z')||Saved.contains('Z'));
            QVERIFY(Evidence.open(QIODevice::WriteOnly));Evidence.write(QJsonDocument(QJsonObject{{"keyboardAndSave",true},{"bytes",Saved.size()}}).toJson());Evidence.close();
        }
        QPointer<QDialog> Closing=Opened;
        const auto ViewerWindow = VisibleTestProcessWindow(static_cast<DWORD>(Pid)); QVERIFY(ViewerWindow);
        QVERIFY(ActivateTestWindow(ViewerWindow));
        QVERIFY(Surface->SendDocument({{"action", "focus"}}).HasValue());
        const auto ViewerThread = GetWindowThreadProcessId(ViewerWindow, nullptr);
        const auto HasViewerFocus = [&] {
            GUITHREADINFO Info{sizeof(Info)};
            return GetGUIThreadInfo(ViewerThread, &Info) && GetForegroundWindow() == ViewerWindow
                && (Info.hwndFocus == ViewerWindow || IsChild(ViewerWindow, Info.hwndFocus));
        };
        QTRY_VERIFY(HasViewerFocus());
        INPUT Escape[2]{}; Escape[0].type = Escape[1].type = INPUT_KEYBOARD;
        Escape[0].ki.wVk = Escape[1].ki.wVk = VK_ESCAPE; Escape[1].ki.dwFlags = KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(2, Escape, sizeof(INPUT)), UINT(2));
        // ESC either disposes the viewer or keeps it hidden as the next warm viewer.
        QTRY_VERIFY_WITH_TIMEOUT(Closing.isNull()||(Closing->isHidden()&&Closing->objectName()=="hc.document.prepared"),10000);
        QTRY_COMPARE(GetForegroundWindow(),reinterpret_cast<HWND>(Main.winId()));
        QTRY_VERIFY(Input.hasFocus());
        // Alien Qt widgets receive native keys through their existing ancestor HWND.
        // Calling winId() here would create a new HWND after focus was restored.
        QTRY_COMPARE(GetFocus(),reinterpret_cast<HWND>(Input.effectiveWinId()));
        for(const auto Character:QString("returned")) {
            INPUT Keys[2]{};for(auto& Key:Keys){Key.type=INPUT_KEYBOARD;Key.ki.wScan=Character.unicode();Key.ki.dwFlags=KEYEVENTF_UNICODE;}
            Keys[1].ki.dwFlags|=KEYEVENTF_KEYUP;QCOMPARE(SendInput(2,Keys,sizeof(INPUT)),UINT(2));
        }
        QTRY_COMPARE(Input.text(),QString("returned"));
        // A background close must leave the user's newly activated window alone.
        Completed = false;
        QPointer<QDialog> Background = Host.OpenPlugin(Options, {{Source,{}}}, "Background F3", [&](bool Ok,const QString&){Completed=Ok;});
        QVERIFY(Background); QTRY_VERIFY_WITH_TIMEOUT(Completed,30000);
        auto* BackgroundSurface = Background->findChild<QtPluginViewerSurface*>(); QVERIFY(BackgroundSurface);
        QProcess Other; Other.start(QCoreApplication::applicationFilePath(), {"--p4-focus-sentinel"});
        const auto StopOther = qScopeGuard([&] { Other.kill(); Other.waitForFinished(); });
        QVERIFY(Other.waitForStarted()); HWND OtherWindow = nullptr;
        QTRY_VERIFY((OtherWindow = VisibleTestProcessWindow(DWORD(Other.processId()))) != nullptr);
        QVERIFY(ActivateTestWindow(OtherWindow));
        QVERIFY(BackgroundSurface->SendDocument({{"action","escape"}}).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(Background.isNull() || (Background->isHidden() && Background->objectName()=="hc.document.prepared"),10000);
        QTest::qWait(150); QCOMPARE(GetForegroundWindow(),OtherWindow);
        Host.CloseAll();QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(),15000);
    }
    void closedViewerBecomesWarm() {
        QTemporaryDir Temp;hc::settings::Repository Settings(Temp.path());
        auto Provider=MakeNativeReadProvider(LocatorKind::Local);
        const auto Input=[&](const QString& Name) {
            QFile File(Temp.filePath(Name));(void)File.open(QIODevice::WriteOnly);File.write(Name.toUtf8());File.close();
            auto Entry=Provider->Resolve({LocatorKind::Local,Temp.filePath(Name).toStdString()},{}).get();
            return std::vector<hc::plugins::HostedViewerSession::DocumentInput>{{std::make_shared<ProviderContentSource>(Provider,Entry.Value()),{}}};
        };
        QWidget Main;Main.show();QVERIFY(QTest::qWaitForWindowExposed(&Main));
        QtViewerHost Host(&Main,[]{return MakeNativeMaterializer(QDir::tempPath().toStdString());});
        auto Drain=qScopeGuard([&]{Host.CloseAll();while(!Host.IsIdle())QTest::qWait(10);});
        const auto Options=BuildDocumentHandlerCatalog(Settings,QCoreApplication::applicationDirPath()).targets_.at("builtin.hyperviewer").plugin_;
        Host.PreparePlugin(Options);
        auto* Warm=Main.findChild<QDialog*>("hc.document.prepared");QVERIFY(Warm);
        auto* Surface=Warm->findChild<QtPluginViewerSurface*>();QVERIFY(Surface);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(),15000);
        const auto Pid=Surface->Details().host_.processId_;
        const auto Open=[&](const QString& Name) {
            bool Done=false,Ok=false;QString Error;QElapsedTimer Timer;Timer.start();
            auto* Window=Host.OpenPlugin(Options,Input(Name),Name,[&](bool Result,const QString& Code){Done=true;Ok=Result;Error=Code;});
            [&]{QTRY_VERIFY_WITH_TIMEOUT(Done,30000);}();
            qInfo()<<Name<<"ready ms"<<Timer.elapsed()<<Error;
            return Ok?Window:nullptr;
        };
        const auto Escape=[&](QDialog* Window) {
            QPointer<QDialog> Guard=Window;QTest::keyClick(Window,Qt::Key_Escape);
            [&]{QTRY_VERIFY_WITH_TIMEOUT(Guard&&Guard->isHidden()&&Guard->objectName()=="hc.document.prepared",10000);}();
        };
        // The re-warm scheduled by the first open is still starting when ESC
        // arrives, so the closed viewer becomes the warm viewer instead.
        auto* First=Open("first.txt");QCOMPARE(First,Warm);
        Escape(First);if(QTest::currentTestFailed())return;
        QCOMPARE(Main.findChildren<QDialog*>("hc.document.prepared").size(),1);
        // F3 right after ESC adopts the viewer while it still resets.
        auto* Second=Open("second.txt");QCOMPARE(Second,Warm);
        QCOMPARE(Surface->Details().host_.processId_,Pid);
        auto Documents=Surface->Details().documentState_["documents"].toArray();
        QCOMPARE(Documents.size(),1);QVERIFY(!Documents[0].toObject()["loading"].toBool());
        QCOMPARE(Surface->Snapshot().source_.name_,std::string("second.txt"));
        Escape(Second);if(QTest::currentTestFailed())return;
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(),15000);
        auto* Third=Open("third.txt");QCOMPARE(Third,Warm);QCOMPARE(Surface->Details().host_.processId_,Pid);
        QCOMPARE(Surface->Snapshot().source_.name_,std::string("third.txt"));
    }
    void preparedViewerOpensLargeImage() {
        QTemporaryDir Temp; QVERIFY(Temp.isValid()); hc::settings::Repository Settings(Temp.path());
        const auto Path=Temp.filePath("too-large.jpg");
        QImage Image(4100,4100,QImage::Format_RGB32); QVERIFY(!Image.isNull()); Image.fill(Qt::blue); QVERIFY(Image.save(Path,"JPEG"));
        auto Provider=MakeNativeReadProvider(LocatorKind::Local);
        auto Entry=Provider->Resolve({LocatorKind::Local,Path.toStdString()},{}).get(); QVERIFY(Entry.HasValue());
        QWidget Main; Main.show();
        QtViewerHost Host(&Main,[]{return MakeNativeMaterializer(QDir::tempPath().toStdString());});
        const auto Drain=qScopeGuard([&]{Host.CloseAll();while(!Host.IsIdle())QTest::qWait(10);});
        const auto Options=BuildDocumentHandlerCatalog(Settings,QCoreApplication::applicationDirPath()).targets_.at("builtin.hyperviewer").plugin_;
        Host.PreparePlugin(Options);
        auto* Prepared=Main.findChild<QDialog*>("hc.document.prepared"); QVERIFY(Prepared);
        auto* Surface=Prepared->findChild<QtPluginViewerSurface*>(); QVERIFY(Surface);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(),15000);
        auto Source=std::make_shared<ProviderContentSource>(Provider,Entry.Value());
        // Until the replacing open completes, the state still describes the placeholder.
        bool Completed=false;
        auto* Opened=Host.OpenPlugin(Options,{{Source,{}}},"Oversized F3",[&](bool,const QString&){Completed=true;}); QCOMPARE(Opened,Prepared);
        QTRY_VERIFY_WITH_TIMEOUT(Completed, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->Details().documentState_["documents"].toArray().size() == 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!Surface->Details().documentState_["documents"].toArray()[0].toObject()["loading"].toBool(), 30000);
        QCOMPARE(Surface->Details().documentState_["documents"].toArray()[0].toObject()["view"].toObject()["viewId"].toString(), QString("image"));
        QVERIFY(Surface->Details().documentState_["documents"].toArray()[0].toObject()["error"].toString().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!Surface->Details().documentState_["documents"].toArray()[0].toObject()["view"].toObject()["rendering"].toBool(),30000);
        QVERIFY(Surface->Details().documentState_["documents"].toArray()[0].toObject()["view"].toObject()["renderError"].toString().isEmpty());
        Opened->close();
    }
    void realProductOpensMixedDocuments() {
        QTemporaryDir Temp; hc::settings::Repository Settings(Temp.path()); const auto TextPath = Temp.filePath("한글 소스.go");
        QFile Text(TextPath); QVERIFY(Text.open(QIODevice::WriteOnly)); Text.write("P4 text document"); Text.close();
        const auto ImagePath = Temp.filePath("image.png"); QImage Image(32, 24, QImage::Format_RGB32); Image.fill(Qt::blue); QVERIFY(Image.save(ImagePath));
        auto Reader = hc::storage::MakeNativeReadProvider(LocatorKind::Local);
        std::vector<hc::plugins::HostedViewerSession::DocumentInput> Inputs;
        for (const auto& File : {TextPath, ImagePath}) { const auto Entry = Reader->Resolve({LocatorKind::Local, File.toStdString()}, {}).get(); QVERIFY(Entry.HasValue()); Inputs.push_back({std::make_shared<ProviderContentSource>(Reader, Entry.Value()), {}}); }
        QWidget Window; Window.show(); QtViewerHost Host(&Window, [] { return hc::storage::MakeNativeMaterializer(QDir::tempPath().toStdString()); });
        const auto Catalog = BuildDocumentHandlerCatalog(Settings, QCoreApplication::applicationDirPath());
        QVERIFY(Catalog.registry_.Find("builtin.hyperviewer")->available_); QVERIFY(Catalog.registry_.Find("builtin.hypereditor")->available_);
        QCOMPARE(DocumentHandlerResolver::Resolve(Catalog.registry_, DocumentIntent::View, {{"한글 소스.go", "image.png"}}).candidates_, std::vector<std::string>{"builtin.hyperviewer"});
        QCOMPARE(DocumentHandlerResolver::Resolve(Catalog.registry_, DocumentIntent::Edit, {{"한글 소스.go", "image.png"}}).candidates_, std::vector<std::string>{"builtin.hypereditor"});
        DocumentOpenController Controller(&Window, [&] { return Catalog; }, [&](const auto& Target, auto Documents, auto Completion) { return Host.OpenPlugin(Target.plugin_, std::move(Documents), "HyperViewer", Completion) != nullptr; });
        QSignalSpy Finished(&Controller, &DocumentOpenController::Finished); QVERIFY(Controller.Open(Controller.Capture({DocumentIntent::View, Inputs, [] { return true; }})));
        QTRY_VERIFY_WITH_TIMEOUT(Finished.size() >= 2, 30000);
        if (Finished.back()[1].toString() != "ready") { if (auto* FailedSurface = Window.findChild<QtPluginViewerSurface*>()) qInfo().noquote() << QJsonDocument(FailedSurface->Details().documentState_).toJson(QJsonDocument::Compact); }
        QVERIFY2(Finished.back()[1].toString() == "ready", qPrintable(Finished.back()[2].toString()));
        auto* Surface = Window.findChild<QtPluginViewerSurface*>(); QVERIFY(Surface); const auto Documents = Surface->Details().documentState_["documents"].toArray(); QCOMPARE(Documents.size(), 2);
        QSet<QString> Views; for (const auto& Value : Documents) { const auto Record = Value.toObject(); QVERIFY(!Record["loading"].toBool()); QVERIFY(Record["error"].toString().isEmpty()); QVERIFY(!Record["sourceToken"].toString().isEmpty()); Views.insert(Record["view"].toObject()["viewId"].toString()); }
        QCOMPARE(Views, (QSet<QString>{"text", "image"})); Host.CloseAll(); QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 15000);
    }
    void cancelDuringVerification() {
        QWidget Window; auto A = Handler("a"); A.kind_ = HandlerKind::Plugin; auto Candidates = Catalog({A}); int Launches = 0;
        DocumentOpenController Controller(&Window, [&] { return Candidates; }, [&](const auto&, auto, auto) { ++Launches; return true; }); QSignalSpy Finished(&Controller, &DocumentOpenController::Finished);
        auto Input = Source(); Input->release_ = false; const auto Token = Controller.Capture({DocumentIntent::View, {{Input, {}}}, [] { return true; }});
        QVERIFY(Controller.Open(Token)); QTRY_COMPARE(Input->verifies_.load(), 1); Controller.Cancel(Token); QTRY_COMPARE(Finished.size(), 1); QCOMPARE(Finished.back()[1].toString(), QString("cancelled")); QCOMPARE(Launches, 0);
    }
    void executableLeaseLivesUntilProcessExit() {
        QTemporaryDir Temp; QWidget Window; auto A = Handler("exe"); auto Candidates = Catalog({A});
        auto& Target = Candidates.targets_.at("exe"); Target.executable_ = QCoreApplication::applicationFilePath(); Target.arguments_ = "--p4-child \"" + Temp.filePath("argv.json") + "\" %FILE%";
        DocumentOpenController Controller(&Window, [&] { return Candidates; }, {}); QSignalSpy Finished(&Controller, &DocumentOpenController::Finished);
        auto Input = Source(); Input->entry_.locator_.kind_ = LocatorKind::Archive; Input->entry_.size_ = 10; Input->entry_.id_ = {{"test"}, "lease", IdentityScope::NavigationSession, ResourceRevision{"1"}};
        QVERIFY(Controller.Open(Controller.Capture({DocumentIntent::Edit, {{Input, {}}}, [] { return true; }})));
        QTRY_VERIFY(!Finished.empty()); QCOMPARE(Finished.back()[1].toString(), QString("started"));
        auto* Process = Controller.findChild<QProcess*>(); QVERIFY(Process); const auto FilePath = Process->arguments().back(); QVERIFY(QFileInfo::exists(FilePath));
        QPointer<QProcess> Guard = Process; const auto Cleanup = qScopeGuard([&] { if (Guard && Guard->state() != QProcess::NotRunning) { Guard->kill(); Guard->waitForFinished(); } });
        QVERIFY(!Window.close()); QTRY_VERIFY(QFileInfo::exists(Temp.filePath("argv.json")));
        QFile Args(Temp.filePath("argv.json")); QVERIFY(Args.open(QIODevice::ReadOnly)); const auto Values = QJsonDocument::fromJson(Args.readAll()).array(); QCOMPARE(Values[Values.size() - 1].toString(), FilePath);
        QTRY_VERIFY_WITH_TIMEOUT(Guard.isNull(), 5000); QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(FilePath), 5000); QVERIFY(Window.close());
    }
    void nativeEditorSurvivesWindowClose() {
        QTemporaryDir Temp; QWidget Window; Window.show(); auto A = Handler("exe"); auto Candidates = Catalog({A});
        auto& Target = Candidates.targets_.at("exe"); Target.executable_ = QCoreApplication::applicationFilePath(); Target.arguments_ = "--p4-child \"" + Temp.filePath("argv.json") + "\" %FILE%";
        DocumentOpenController Controller(&Window, [&] { return Candidates; }, {}); QSignalSpy Finished(&Controller, &DocumentOpenController::Finished);
        QVERIFY(Controller.Open(Controller.Capture({DocumentIntent::Edit, {{Source(), {}}}, [] { return true; }}))); QTRY_VERIFY(!Finished.empty()); QCOMPARE(Finished.back()[1].toString(), QString("started"));
        QPointer<QProcess> Process = Controller.findChild<QProcess*>(); QVERIFY(Process); const auto Cleanup = qScopeGuard([&] { if (Process && Process->state() != QProcess::NotRunning) { Process->kill(); Process->waitForFinished(); } });
        QVERIFY(Window.close()); QVERIFY(Process->parent() == nullptr); QCOMPARE(Process->state(), QProcess::Running); QTRY_VERIFY_WITH_TIMEOUT(Process.isNull(), 5000);
    }
    void argumentArrayPreservesSpacesAndTokens() {
        const auto Args = DocumentOpenController::Arguments("--line 4 \"%FILE%\" --literal \"x y\"", {"C:/한글 이름/a.txt"});
        QCOMPARE(Args, (QStringList{"--line", "4", QDir::toNativeSeparators("C:/한글 이름/a.txt"), "--literal", "x y"}));
        QCOMPARE(DocumentOpenController::Arguments("--open %FILES%", {"C:/a.txt", "C:/b b.txt"}), (QStringList{"--open", "C:\\a.txt", "C:\\b b.txt"}));
    }
};
int main(int Argc, char** Argv) {
    if (Argc > 1 && QByteArray(Argv[1]) == "--p4-focus-sentinel") {
        QApplication App(Argc, Argv); QWidget Window;
        Window.setWindowTitle("Background-close focus sentinel"); Window.resize(480,240); Window.show();
        QTimer::singleShot(30000, &App, &QCoreApplication::quit); return App.exec();
    }
    if (Argc > 2 && QByteArray(Argv[1]) == "--p4-child") {
        QCoreApplication App(Argc, Argv); QJsonArray Values; for (const auto& Arg : App.arguments()) Values.append(Arg);
        QFile File(QString::fromLocal8Bit(Argv[2])); if (!File.open(QIODevice::WriteOnly)) return 2; File.write(QJsonDocument(Values).toJson()); File.close(); QThread::msleep(700); return 0;
    }
    QApplication App(Argc, Argv); DocumentP4Tests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstDocumentP4.moc"
