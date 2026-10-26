#include <QtTest>
#include "UniqueLibs/document/documentViewPane.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include <QtConcurrent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QVBoxLayout>
#include <QToolBar>
#include <QInputMethodEvent>
#include <QScreen>
#include <iostream>
#include <QMenu>
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/documentHostBroker.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "pluginHost/pluginProcess.hpp"
#include "pluginHost/manifestFile.hpp"
#include "pluginHost/dopusAbi.hpp"
#include <future>
#include <TlHelp32.h>
#include <QSet>

using namespace hc::document;
using namespace hc::storage;
using namespace hc::plugins;
namespace
{
#ifdef HC_TEST_RHWP_HOST_RUNTIME
    struct CapturedRhwpTree
    {
        std::vector<HANDLE> handles_;
        ~CapturedRhwpTree() { for (auto Handle : handles_) CloseHandle(Handle); }
        bool Capture(DWORD Root)
        {
            HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (Snapshot == INVALID_HANDLE_VALUE) return false;
            auto Close = qScopeGuard([&] { CloseHandle(Snapshot); });
            std::vector<PROCESSENTRY32W> Entries; PROCESSENTRY32W Entry{}; Entry.dwSize = sizeof(Entry);
            if (!Process32FirstW(Snapshot, &Entry)) return false;
            do { Entries.push_back(Entry); } while (Process32NextW(Snapshot, &Entry));
            QSet<DWORD> Owners{Root}; bool Added = true;
            while (Added) {
                Added = false;
                for (const auto& Item : Entries) if (Owners.contains(Item.th32ParentProcessID) && !Owners.contains(Item.th32ProcessID)) {
                    Owners.insert(Item.th32ProcessID); Added = true;
                }
            }
            bool Helper = false, Engine = false;
            for (const auto& Item : Entries) if (Item.th32ProcessID != Root && Owners.contains(Item.th32ProcessID)) {
                auto Handle = OpenProcess(SYNCHRONIZE, FALSE, Item.th32ProcessID);
                if (!Handle) return false;
                handles_.push_back(Handle);
                Helper |= _wcsicmp(Item.szExeFile, L"HyperEditorWeb.exe") == 0;
                Engine |= _wcsicmp(Item.szExeFile, L"QtWebEngineProcess.exe") == 0;
            }
            return Helper && Engine;
        }
        bool Exited() const
        { return !handles_.empty() && std::all_of(handles_.begin(), handles_.end(), [](HANDLE Handle) { return WaitForSingleObject(Handle, 0) == WAIT_OBJECT_0; }); }
    };
#endif
    bool Put(const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    template<class F> auto Worker(F Work)
    {
        auto Future = std::async(std::launch::async, std::move(Work));
        while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { QCoreApplication::processEvents(); QTest::qWait(1); }
        return Future.get();
    }
    QJsonObject Document(const QJsonObject& State, const QString& Id)
    { for (auto Value : State["documents"].toArray()) if (Value.toObject()["documentId"].toString() == Id) return Value.toObject(); return {}; }
    bool Ready(const QJsonObject& State, const QString& Id)
    { auto Value = Document(State, Id); return !Value.isEmpty() && !Value["loading"].toBool() && Value["error"].toString().isEmpty(); }
    QJsonObject Native(HWND Viewer, HMODULE Module, QJsonObject Request)
    {
        using Call = BOOL (*)(HWND, const char*, UINT, char*, UINT*);
        auto Invoke = reinterpret_cast<Call>(GetProcAddress(Module, "HC_DocumentCommand"));
        const auto Input = QJsonDocument(Request).toJson(QJsonDocument::Compact); QByteArray Output(HostPayloadLimit, Qt::Uninitialized); UINT Size = Output.size();
        if (!Invoke || !Invoke(Viewer, Input.constData(), Input.size(), Output.data(), &Size)) return {{"error", "not-ready"}};
        Output.resize(Size); return QJsonDocument::fromJson(Output).object();
    }
}
class DocumentP0Tests final : public QObject
{
    Q_OBJECT
private slots:
    void utf8SavePreservesBomEolAndOriginalOnConflict()
    {
        QTemporaryDir Root; const auto Path = Root.filePath("한글 파일.txt");
        const QByteArray Initial = QByteArray("\xef\xbb\xbf") + QByteArray("one\r\ntwo\r\n"); QVERIFY(Put(Path, Initial));
        auto Opened = Worker([&] { return ReadTextFile(Path); }); QVERIFY(Opened.HasValue());
        const auto Bytes = EncodeText("새 문서\n둘째 줄\n", Opened.Value().bom_, Opened.Value().eol_);
        auto Saved = Worker([&] { return SaveTextFile(Path, Bytes, {true, Opened.Value().source_.id_.revision_}, Opened.Value().hash_); });
        QVERIFY2(Saved.HasValue(), Saved.HasValue() ? "" : Saved.Failure().code_.c_str()); QCOMPARE(Read(Path), Bytes);
        auto Current = Worker([&] { return ReadTextFile(Path); }); QVERIFY(Current.HasValue());
        QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly)); const auto Time = File.fileTime(QFileDevice::FileModificationTime); File.close();
        auto External = Bytes; External[External.size() - 3] = 'x'; QVERIFY(Put(Path, External));
        QVERIFY(File.open(QIODevice::ReadWrite)); QVERIFY(File.setFileTime(Time, QFileDevice::FileModificationTime)); File.close();
        auto Conflict = Worker([&] { return SaveTextFile(Path, Bytes, {true, Current.Value().source_.id_.revision_}, Current.Value().hash_); });
        QVERIFY(!Conflict.HasValue()); QCOMPARE(Conflict.Failure().category_, ErrorCategory::StaleRevision); QCOMPARE(Read(Path), External);
        std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        auto Cancelled = Worker([&] { return SaveTextFile(Root.filePath("cancel.txt"), "x", {}, {}, Context); });
        QVERIFY(!Cancelled.HasValue()); QVERIFY(!QFile::exists(Root.filePath("cancel.txt")));
        QVERIFY(Put(Root.filePath("binary.txt"), QByteArray("\0\1", 2))); QVERIFY(!ReadTextFile(Root.filePath("binary.txt")).HasValue());
        const auto Inf = Root.filePath("setup.inf"); QVERIFY(Put(Inf, "[Version]\r\nSignature=\"$Windows NT$\"\r\n"));
        auto InfDocument = Worker([&] { return ReadDocumentFile(Inf); }); QVERIFY(InfDocument.HasValue());
        QCOMPARE(InfDocument.Value().viewId_, QString("text")); QVERIFY(InfDocument.Value().text_.contains("[Version]"));
    }
    void imageFileLargerThanTextBudgetOpens()
    {
        QTemporaryDir Root; const auto Path = Root.filePath("large.bmp");
        QImage Image(5000, 5000, QImage::Format_RGB32); QVERIFY(!Image.isNull());
        Image.fill(Qt::blue); QVERIFY(Image.save(Path, "BMP"));
        QVERIFY(QFileInfo(Path).size() > DocumentByteLimit);
        Image = QImage{};
        auto Opened = Worker([&] { return ReadDocumentFile(Path); });
        QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : Opened.Failure().code_.c_str());
        QCOMPARE(Opened.Value().viewId_, QString("image"));
        QCOMPARE(Opened.Value().image_.size(), QSize(5000, 5000));
    }
    void twoTabsRevisionCloseAndRecovery()
    {
        QTemporaryDir Root; QVERIFY(Put(Root.filePath("one.txt"), "one\r\n")); QVERIFY(Put(Root.filePath("two.txt"), "two\n"));
        DocumentWorkspaceWidget Workspace(Root.filePath("recovery")); Workspace.show();
        const auto ExpectedDpr = qEnvironmentVariable("HC_DOCUMENT_EXPECT_DPR").toDouble();
        if (ExpectedDpr > 0) QVERIFY(qAbs(Workspace.devicePixelRatioF() - ExpectedDpr) < 0.01);
        const auto One = Workspace.Handle({{"action", "open"}, {"path", Root.filePath("one.txt")}})["documentId"].toString();
        const auto Two = Workspace.Handle({{"action", "open"}, {"path", Root.filePath("two.txt")}})["documentId"].toString();
        QTRY_VERIFY(Ready(Workspace.Snapshot(), One) && Ready(Workspace.Snapshot(), Two));
        auto* Layout = qobject_cast<QVBoxLayout*>(Workspace.layout()); QVERIFY(Layout);
        QCOMPARE(Layout->itemAt(1)->widget()->objectName(), QString("mainToolbar")); QCOMPARE(Layout->itemAt(2)->widget()->objectName(), QString("documentTabs"));
        QCOMPARE(Workspace.findChildren<QToolBar*>("viewToolbar").size(), 2);
        Workspace.activateWindow(); Workspace.raise();
        auto* Tabs = Workspace.findChild<QTabWidget*>("documentTabs"); QVERIFY(Tabs);
        auto* Editor = qobject_cast<DocumentViewPane*>(Tabs->currentWidget())->InputWidget(); QVERIFY(Editor);
        Editor->setFocus(); QTest::keyClick(Editor, Qt::Key_Tab, Qt::ControlModifier);
        QCOMPARE(Workspace.Snapshot()["activeDocumentId"].toString(), One);
        QTest::keyClick(Editor, Qt::Key_Tab, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(Workspace.Snapshot()["activeDocumentId"].toString(), Two);
        Editor = qobject_cast<DocumentViewPane*>(Tabs->currentWidget())->InputWidget(); Editor->setFocus();
        QInputMethodEvent InputMethod; InputMethod.setCommitString("한글 조합 입력");
        QCoreApplication::sendEvent(Editor, &InputMethod);
        QVERIFY(qobject_cast<DocumentViewPane*>(Tabs->currentWidget())->Content().contains(QString("한글 조합 입력").toUtf8()));
        qobject_cast<DocumentViewPane*>(Tabs->currentWidget())->Undo(); QVERIFY(!Document(Workspace.Snapshot(), Two)["dirty"].toBool());
        QVERIFY(!Workspace.Handle({{"action", "replaceText"}, {"documentId", One}, {"text", "changed\n"}}).contains("error"));
        QVERIFY(Document(Workspace.Snapshot(), One)["dirty"].toBool()); QVERIFY(!Document(Workspace.Snapshot(), Two)["dirty"].toBool());
        QVERIFY(Workspace.Handle({{"action", "close"}, {"documentId", One}}).contains("error"));
        Workspace.Handle({{"action", "save"}, {"documentId", One}});
        Workspace.Handle({{"action", "replaceText"}, {"documentId", One}, {"text", "later edit\n"}});
        QTRY_VERIFY(!Document(Workspace.Snapshot(), One)["saving"].toBool());
        QCOMPARE(Read(Root.filePath("one.txt")), QByteArray("changed\r\n")); QVERIFY(Document(Workspace.Snapshot(), One)["dirty"].toBool());
        Workspace.Checkpoint(); const auto Recovery = Root.filePath("recovery/") + One + ".json"; QTRY_VERIFY(QFileInfo::exists(Recovery));
        auto Restored = Workspace.Handle({{"action", "recover"}, {"path", Recovery}})["documentId"].toString(); QVERIFY(!Restored.isEmpty());
        QTRY_VERIFY(Ready(Workspace.Snapshot(), Restored));
        Workspace.Handle({{"action", "undo"}, {"documentId", One}}); QVERIFY(!Document(Workspace.Snapshot(), One)["dirty"].toBool());
        QVERIFY(Workspace.Handle({{"action", "activate"}, {"documentId", "stale"}}).contains("error"));
        QVERIFY(Workspace.Handle({{"action", "state"}, {"protocolVersion", 99}}).contains("error"));
    }
    void brokerRetainsNewEditsAndRejectsReplay()
    {
        QTemporaryDir Root; const auto Path = Root.filePath("source.txt"); QVERIFY(Put(Path, "base\n"));
        auto Input = ReadTextFile(Path); QVERIFY(Input.HasValue());
        DocumentHostBroker Broker; auto Reader = MakeNativeReadProvider(LocatorKind::Local); auto Writer = MakeNativeWritableStorage(LocatorKind::Local, Reader);
        const auto Token = Broker.Bind(Input.Value().source_, Writer, {}, Input.Value().hash_); QVERIFY(!Token.isEmpty());
        DocumentWorkspaceWidget Workspace(Root.filePath("recovery"), true);
        const auto Id = Workspace.Handle({{"action", "open"}, {"path", Path}, {"sourceToken", Token}, {"broker", true}})["documentId"].toString(); QTRY_VERIFY(Ready(Workspace.Snapshot(), Id));
        Workspace.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "saved revision\n"}});
        auto Prepared = Workspace.Handle({{"action", "prepareSave"}, {"documentId", Id}}); Prepared["documentId"] = Id;
        Workspace.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "new revision\n"}});
        auto Committed = Worker([&] { return Broker.Commit(Prepared); }); QVERIFY2(Committed["committed"].toBool(), qPrintable(Committed["error"].toString()));
        QVERIFY(!Workspace.Handle(Committed).contains("error")); QVERIFY(Document(Workspace.Snapshot(), Id)["dirty"].toBool()); QCOMPARE(Read(Path), QByteArray("saved revision\n"));
        QVERIFY(!Broker.Commit(Prepared)["committed"].toBool()); QVERIFY(Workspace.Handle(Committed).contains("error"));
        Broker.Close(); Prepared["saveToken"] = QString(32, 'a'); QVERIFY(!Broker.Commit(Prepared)["committed"].toBool());
    }
    void nativeSdkRhwpDocument_data()
    {
        QTest::addColumn<QString>("FixtureName"); QTest::addColumn<QByteArray>("Pin");
        QTest::newRow("hwp") << QString("c02.hwp") << QByteArray("86e607233a2218928fabe5969155116a46d4ab4400db1402fe45b0082149496c");
        QTest::newRow("hwpx") << QString("c05.hwpx") << QByteArray("8d9df52bb09803fa1754317360e27709621555b3e1163a0d7eb4ad9abab5b26b");
    }
    void nativeSdkRhwpDocument()
    {
#ifndef HC_TEST_RHWP_HOST_RUNTIME
        QSKIP("Qualified diagnostic runtime not configured");
#else
        QFETCH(QString, FixtureName); QFETCH(QByteArray, Pin);
        const auto Corpus = qEnvironmentVariable("HC_RHWP_TEST_SDK_CORPUS");
        if (Corpus.isEmpty()) QSKIP("Pinned SDK engine corpus not requested");
        const auto Bytes = Read(QDir(Corpus).filePath(FixtureName));
        QCOMPARE(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex(), Pin);
        const auto Suffix = QFileInfo(FixtureName).suffix();
        QTemporaryDir Root; const auto First = Root.filePath("first." + Suffix), Next = Root.filePath("next." + Suffix);
        QVERIFY(Put(First, Bytes)); QVERIFY(Put(Next, Bytes));
        QWidget Parent; Parent.resize(800, 500); Parent.show();
        const auto Dll = QCoreApplication::applicationDirPath() + '/' + hc::layout::HyperViewerPackage + "/HyperViewer.dll";
        auto Module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Dll.utf16()), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); QVERIFY(Module);
        HWND Viewer = nullptr; auto Cleanup = qScopeGuard([&] { if (Viewer && IsWindow(Viewer)) DestroyWindow(Viewer); if (auto Uninit = reinterpret_cast<dopus::Uninit>(GetProcAddress(Module,"DVP_Uninit"))) Uninit(); FreeLibrary(Module); });
        auto Create = reinterpret_cast<dopus::CreateViewer>(GetProcAddress(Module, "DVP_CreateViewer")); QVERIFY(Create);
        RECT Rect{0, 0, 800, 500}; Viewer = Create(reinterpret_cast<HWND>(Parent.winId()), &Rect, 0); QVERIFY(Viewer);
        const auto Load = [&](const QString& Path) { const auto Wide = Path.toStdWString(); return SendMessageW(Viewer, dopus::ViewerLoadW, 0, reinterpret_cast<LPARAM>(Wide.c_str())); };
        QVERIFY(Load(First)); QJsonObject State;
        const auto Poll=[&]{State=Native(Viewer,Module,{{"action","state"}});return Ready(State,State["activeDocumentId"].toString());};
        QTRY_VERIFY_WITH_TIMEOUT(Poll(),45000);
        const auto Id=State["activeDocumentId"].toString();QVERIFY(!Id.isEmpty());
        QVERIFY(Document(State,Id)["view"].toObject()["readOnly"].toBool());
        QVERIFY(!Document(State,Id)["canSave"].toBool());
        QCOMPARE(Native(Viewer,Module,{{"action","hostRuntime"}})["processLimit"].toInt(),0);
        const auto Added=Native(Viewer,Module,{{"action","open"},{"path",Next}})["documentId"].toString();
        QTRY_VERIFY_WITH_TIMEOUT(Ready(Native(Viewer,Module,{{"action","state"}}),Added),45000);
        QCOMPARE(Native(Viewer,Module,{{"action","state"}})["documents"].toArray().size(),2);
        CapturedRhwpTree Tree;QVERIFY(Tree.Capture(GetCurrentProcessId()));
        Native(Viewer,Module,{{"action","close"},{"documentId",Added}});
        Native(Viewer,Module,{{"action","close"},{"documentId",Id}});
        QTRY_VERIFY_WITH_TIMEOUT(Native(Viewer,Module,{{"action","state"}})["documents"].toArray().isEmpty(),30000);
        QTRY_VERIFY_WITH_TIMEOUT(Tree.Exited(),10000);
        QCOMPARE(Read(First),Bytes);QCOMPARE(Read(Next),Bytes);
        DestroyWindow(Viewer);Viewer=nullptr;
#endif
    }
    void nativeSdkContainerAndTwoHelperTabs()
    {
        QTemporaryDir Root; QVERIFY(Put(Root.filePath("one.txt"), "one\n")); QVERIFY(Put(Root.filePath("two.txt"), "two\n"));
        QWidget Parent; Parent.resize(800, 500); Parent.show();
        const auto Dll = QCoreApplication::applicationDirPath() + '/' + hc::layout::HyperViewerPackage + "/HyperViewer.dll";
        auto Module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Dll.utf16()), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); QVERIFY(Module);
        HWND Viewer = nullptr; auto Cleanup = qScopeGuard([&] { if (Viewer && IsWindow(Viewer)) DestroyWindow(Viewer); if (auto Uninit = reinterpret_cast<dopus::Uninit>(GetProcAddress(Module,"DVP_Uninit"))) Uninit(); FreeLibrary(Module); });
        for (auto Name : {"DVP_InitEx", "DVP_Uninit", "DVP_IdentifyW", "DVP_IdentifyFileW", "DVP_CreateViewer", "HC_DocumentCommand"}) QVERIFY(GetProcAddress(Module, Name));
        auto Create = reinterpret_cast<dopus::CreateViewer>(GetProcAddress(Module, "DVP_CreateViewer")); RECT Rect{0, 0, 800, 500};
        Viewer = Create(reinterpret_cast<HWND>(Parent.winId()), &Rect, 0); QVERIFY(Viewer);
        DWORD Owner = 0; GetWindowThreadProcessId(Viewer, &Owner); QCOMPARE(Owner, GetCurrentProcessId()); QVERIFY(IsChild(reinterpret_cast<HWND>(Parent.winId()), Viewer));
        auto Name = Root.filePath("one.txt").toStdWString(); QElapsedTimer Timer; Timer.start();
        QVERIFY(SendMessageW(Viewer, dopus::ViewerLoadW, 0, reinterpret_cast<LPARAM>(Name.c_str()))); QVERIFY(Timer.elapsed() < 100);
        QJsonObject State; QElapsedTimer ReadyTimer; ReadyTimer.start();
        do { State = Native(Viewer, Module, {{"action", "state"}}); QTest::qWait(50); } while (!State.contains("documents") && ReadyTimer.elapsed() < 12000);
        QVERIFY2(State.contains("documents"), QJsonDocument(State).toJson(QJsonDocument::Compact).constData());
        const auto Runtime = Native(Viewer, Module, {{"action", "hostRuntime"}});
        QCOMPARE(Runtime["version"].toInt(), 1);
        QCOMPARE(Runtime["rhwpRuntimeRoot"].toString(), QCoreApplication::applicationDirPath());
        QVERIFY(Runtime["rhwpHostCommands"].toBool()); QVERIFY(!Runtime["rhwpHostEdit"].toBool());
        QCOMPARE(Native(Viewer, Module, {{"action", "rhwpState"}, {"version", 1}, {"documentId", "stale"}})["error"].toString(), QString("rhwp-host-stale-document"));
        QCOMPARE(Native(Viewer, Module, {{"action", "rhwpOpen"}, {"version", "1"}, {"path", Root.filePath("test.hwp")}})["error"].toString(), QString("rhwp-host-schema"));
        if (!Runtime["rhwpPolicyAvailable"].toBool()) {
            QCOMPARE(Native(Viewer, Module, {{"action", "rhwpOpen"}, {"version", 1}, {"path", Root.filePath("test.hwp")}})["error"].toString(), QString("rhwp-runtime-policy-unavailable"));
            QCOMPARE(Native(Viewer, Module, {{"action", "openHost"}, {"path", Root.filePath("test.hwpx")}})["error"].toString(), QString("rhwp-runtime-policy-unavailable"));
        }
#ifdef HC_TEST_RHWP_HOST_RUNTIME
        QVERIFY(Runtime["rhwpPolicyAvailable"].toBool());
#else
        auto RejectedHwp = Root.filePath("unsupported.HWP").toStdWString();
        QVERIFY(!SendMessageW(Viewer, dopus::ViewerLoadW, 0, reinterpret_cast<LPARAM>(RejectedHwp.c_str())));
        auto RejectedHwpx = Root.filePath("unsupported.hwpx").toStdWString();
        QVERIFY(!SendMessageW(Viewer, dopus::ViewerLoadW, 0, reinterpret_cast<LPARAM>(RejectedHwpx.c_str())));
        // An SDK load uses the current process; the caller owns its Job policy.
#endif
        QCOMPARE(Native(Viewer, Module, {{"action", "openHost"}, {"path", Root.filePath("two.txt")}, {"protocolVersion", 99}})["error"].toString(), QString("document-protocol-version"));
        QCOMPARE(Native(Viewer, Module, {{"action", "cancelHost"}, {"protocolVersion", 99}})["error"].toString(), QString("document-protocol-version"));
        QCOMPARE(State["documents"].toArray().size(), 1);
        const auto One = State["documents"].toArray()[0].toObject()["documentId"].toString();
        QTRY_VERIFY(Ready(Native(Viewer, Module, {{"action", "state"}}), One));
        auto Added = Native(Viewer, Module, {{"action", "open"}, {"path", Root.filePath("two.txt")}}); const auto Two = Added["documentId"].toString(); QVERIFY(!Two.isEmpty());
        QTRY_VERIFY(Ready(Native(Viewer, Module, {{"action", "state"}}), Two));
        QVERIFY(!Native(Viewer, Module, {{"action", "replaceText"}, {"documentId", One}, {"text", "helper edit 한글\n"}}).contains("error"));
        const auto Output = Root.filePath("saved.txt"); Native(Viewer, Module, {{"action", "saveAs"}, {"documentId", One}, {"path", Output}});
        QTRY_VERIFY(QFile::exists(Output)); QCOMPARE(Read(Output), QString("helper edit 한글\n").toUtf8());
        QVERIFY(Native(Viewer, Module, {{"action", "present"}})["accepted"].toBool());
        QVERIFY(!GetWindow(Viewer, GW_CHILD));
        QVERIFY(qAbs(Native(Viewer, Module, {{"action", "state"}})["dpr"].toDouble() - Parent.devicePixelRatioF()) < 0.01);
        Native(Viewer, Module, {{"action", "activate"}, {"documentId", Two}});
        QCOMPARE(Native(Viewer, Module, {{"action", "state"}})["documents"].toArray().size(), 2);
        QVERIFY(Native(Viewer, Module, {{"action", "state"}, {"protocolVersion", 9}}).contains("error"));
        // SDK-driven re-load replaces only its slot after preserving the dirty
        // revision; the internally opened second tab must remain alive.
        Native(Viewer, Module, {{"action", "replaceText"}, {"documentId", One}, {"text", "forced replacement recovery\n"}});
        auto RecoveryRoot = Native(Viewer, Module, {{"action", "state"}})["recoveryRoot"].toString();
        auto Next = Root.filePath("two.txt").toStdWString(); QVERIFY(SendMessageW(Viewer, dopus::ViewerLoadW, 0, reinterpret_cast<LPARAM>(Next.c_str())));
        QTRY_VERIFY(Document(Native(Viewer, Module, {{"action", "state"}}), One).isEmpty());
        QCOMPARE(Native(Viewer, Module, {{"action", "state"}})["documents"].toArray().size(), 2);
        QVERIFY(!Document(Native(Viewer, Module, {{"action", "state"}}), Two).isEmpty());
        const auto RecordPath = RecoveryRoot + "/" + One + ".json"; QVERIFY(QFile::exists(RecordPath));
        QCOMPARE(QByteArray::fromBase64(QJsonDocument::fromJson(Read(RecordPath)).object()["bytes"].toString().toLatin1()), QByteArray("forced replacement recovery\n"));
        DestroyWindow(Viewer); Viewer = nullptr;
        DocumentRecoveryStore Recovery(RecoveryRoot); QVERIFY(Recovery.Remove(One.toStdString(), UINT64_MAX).HasValue());
    }
    void nativeSdkIdentifiesTextContentAndLargeImages()
    {
        QTemporaryDir Root;
        const auto Dll = QCoreApplication::applicationDirPath() + '/' + hc::layout::HyperViewerPackage + "/HyperViewer.dll";
        auto Module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Dll.utf16()), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); QVERIFY(Module);
        const auto Release = qScopeGuard([&] { FreeLibrary(Module); });
        auto Identify = reinterpret_cast<dopus::IdentifyFile<wchar_t>>(GetProcAddress(Module, "DVP_IdentifyFileW")); QVERIFY(Identify);
        const auto Query = [&](const QString& Path) {
            auto Name = Path.toStdWString(); dopus::ViewerFileInfo<wchar_t> Info{}; Info.size_ = sizeof(Info);
            return Identify(nullptr, Name.data(), &Info, nullptr) != FALSE;
        };
        const auto Inf = Root.filePath("setup.inf"); QVERIFY(Put(Inf, "[Version]\r\nSignature=\"$Windows NT$\"\r\n"));
        QVERIFY(Query(Inf));
        const auto Epub = Root.filePath("book.EPUB"); QVERIFY(Put(Epub, "PK")); QVERIFY(Query(Epub));
        const auto Binary = Root.filePath("data.unknown"); QVERIFY(Put(Binary, QByteArray("\0\1", 2)));
        QVERIFY(!Query(Binary));
        const auto Image = Root.filePath("large.jpg"); QFile File(Image); QVERIFY(File.open(QIODevice::WriteOnly));
        QVERIFY(File.seek(qint64(DocumentByteLimit) + 1)); QVERIFY(File.putChar('\0')); File.close();
        QVERIFY(Query(Image));
    }
    void isolatedEpubViewerEntry()
    {
        const auto Path = qEnvironmentVariable("HC_EPUB_POC_EPUB");
        if (Path.isEmpty()) QSKIP("Set HC_EPUB_POC_EPUB to an image-bearing EPUB fixture");
        const auto Package = QCoreApplication::applicationDirPath() + '/' + hc::layout::HyperViewerPackage + "/hyperviewer.json";
        auto Inspected = InspectManifestFile(Package); QVERIFY(Inspected.HasValue());
        const auto Manifest = Inspected.Value().manifest_;
        PluginApproval Approval{Manifest, true, false, Manifest.permissions_};
        auto Started = Worker([&] { return PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", Package, Approval); });
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Process = std::move(Started.Value());
        auto Cleanup = qScopeGuard([&] { Worker([&] { Process->Close(); return 0; }); });
        QWidget Parent; Parent.resize(1100, 800); Parent.show();
        auto Opened = Worker([&] { return Process->ViewerCall(WireKind::ViewerOpen, {{"parent", QString::number(Parent.winId(), 16)}, {"file", Path}, {"width", 1100}, {"height", 800}, {"flags", 0}}); });
        QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : Opened.Failure().code_.c_str());
        auto Call = [&](QJsonObject Request) {
            auto Reply = Worker([&] { return Process->ViewerCall(WireKind::DocumentCommand, Request); });
            return Reply.HasValue() ? Reply.Value() : QJsonObject{{"error", QString::fromStdString(Reply.Failure().code_)}};
        };
        QJsonObject State;
        QTRY_VERIFY_WITH_TIMEOUT((State = Call({{"action", "state"}}))["documents"].toArray().size() == 1, 15000);
        const auto Id = State["documents"].toArray()[0].toObject()["documentId"].toString();
        QVERIFY(Call({{"action", "present"}})["accepted"].toBool());
        QJsonObject Loaded;
        QTRY_VERIFY_WITH_TIMEOUT((Loaded = Document(Call({{"action", "state"}}), Id))["view"].toObject()["ready"].toBool()
            || !Loaded["error"].toString().isEmpty(), 60000);
        QVERIFY2(Loaded["view"].toObject()["ready"].toBool(), QJsonDocument(Loaded).toJson(QJsonDocument::Compact).constData());
        const auto DocumentState = Document(Call({{"action", "state"}}), Id);
        const auto View = DocumentState["view"].toObject();
        QCOMPARE(View["viewId"].toString(), QString("epub")); QVERIFY(View["readOnly"].toBool());
        QVERIFY(!View["epub"].toObject()["images"].toArray().isEmpty());
        QVERIFY(!DocumentState["canSave"].toBool());
        QVERIFY(Call({{"action", "save"}, {"documentId", Id}}).contains("error"));
        QVERIFY(Worker([&] { return Process->ViewerCall(WireKind::ViewerClose, {}); }).HasValue());
        Worker([&] { Process->Close(); return 0; });
        const auto Snapshot = Process->Snapshot(); QVERIFY(!Snapshot.running_); QCOMPARE(Snapshot.activeProcesses_, 0u);
    }
    void isolatedJobBrokerClipboardAndHelperReap_data()
    { QTest::addColumn<bool>("Forced"); QTest::newRow("normal-close") << false; QTest::newRow("forced-host-exit") << true; }
    void isolatedJobBrokerClipboardAndHelperReap()
    {
        QTemporaryDir Root; const auto Path = Root.filePath("source.txt"); QVERIFY(Put(Path, "isolated\n"));
        QFETCH(bool, Forced);
        const auto Package = QCoreApplication::applicationDirPath() + '/' + hc::layout::HyperViewerPackage + "/hyperviewer.json";
        auto Inspected = InspectManifestFile(Package); QVERIFY2(Inspected.HasValue(), Inspected.HasValue() ? "" : Inspected.Failure().code_.c_str());
        const auto Manifest = Inspected.Value().manifest_;
        auto ReadOnlyManifest = Manifest; ReadOnlyManifest.permissions_.erase(Permission::WriteContent);
        QVERIFY(!Validate(ReadOnlyManifest).HasValue()); // The editable helper requires explicit write approval.
        PluginApproval Approval{Manifest, true, false, Manifest.permissions_};
        auto Started = Worker([&] { return PluginProcess::Start(QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe", Package, Approval); });
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str()); auto Process = std::move(Started.Value());
        QWidget Parent; Parent.resize(800, 500); Parent.show();
        auto Opened = Worker([&] { return Process->ViewerCall(WireKind::ViewerOpen, {{"parent", QString::number(Parent.winId(), 16)}, {"file", Path}, {"width", 800}, {"height", 500}, {"flags", 0}}); });
        QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : Opened.Failure().code_.c_str());
        auto Call = [&](QJsonObject Request) {
            auto Reply = Worker([&] { return Process->ViewerCall(WireKind::DocumentCommand, Request); });
            return Reply.HasValue() ? Reply.Value() : QJsonObject{{"error", QString::fromStdString(Reply.Failure().code_)}};
        };
        QJsonObject State; QTRY_VERIFY_WITH_TIMEOUT((State = Call({{"action", "state"}})).contains("documents") && !State["documents"].toArray().isEmpty(), 15000);
        const auto HostSlot = State["documents"].toArray()[0].toObject()["documentId"].toString(); QTRY_VERIFY(Ready(Call({{"action", "state"}}), HostSlot));
        const auto Hello=Call({{"protocolVersion",int(HostVersion)},{"requestId","1"},{"workspaceId",""},{"documentId",""},{"generation",""},{"messageType","Hello"},{"payloadLength",2},{"payload",QJsonObject{}}});
        QCOMPARE(Hello["messageType"].toString(),QString("Ready"));
        auto Input = Worker([&] { return ReadTextFile(Path); }); QVERIFY(Input.HasValue());
        DocumentHostBroker Broker; auto Reader = MakeNativeReadProvider(LocatorKind::Local);
        auto Token = Broker.Bind(Input.Value().source_, MakeNativeWritableStorage(LocatorKind::Local, Reader), {}, Input.Value().hash_);
        const auto Id = Call({{"action", "open"}, {"path", Path}, {"sourceToken", Token}, {"broker", true}})["documentId"].toString(); QVERIFY(!Id.isEmpty()); QTRY_VERIFY(Ready(Call({{"action", "state"}}), Id));
        Call({{"action", "replaceText"}, {"documentId", Id}, {"text", "isolated edit\n"}});
        auto Prepared = Call({{"action", "prepareSave"}, {"documentId", Id}}); Prepared["documentId"] = Id;
        auto Committed = Worker([&] { return Broker.Commit(Prepared); }); QVERIFY2(Committed["committed"].toBool(), qPrintable(Committed["error"].toString()));
        QVERIFY(!Call(Committed).contains("error")); QCOMPARE(Read(Path), QByteArray("isolated edit\n"));
        Call({{"action", "selectAll"}, {"documentId", Id}}); auto Copy = Call({{"action", "clipboard"}, {"documentId", Id}, {"mode", "copy"}}); QVERIFY(!Copy.contains("error"));
        auto Clipboard = Call({{"action", "state"}})["clipboard"].toObject(); QCOMPARE(Clipboard["text"].toString(), QString("isolated edit\n"));
        QVERIFY(!Call({{"action", "clipboardResult"}, {"token", Clipboard["token"]}, {"accepted", true}}).contains("error"));
        Call({{"action", "clipboard"}, {"documentId", Id}, {"mode", "paste"}}); Clipboard = Call({{"action", "state"}})["clipboard"].toObject();
        Call({{"action", "clipboardResult"}, {"token", Clipboard["token"]}, {"accepted", true}, {"text", "중계된 붙여넣기\n"}});
        QVERIFY(Document(Call({{"action", "state"}}), Id)["dirty"].toBool());
        Call({{"action", "checkpoint"}}); State = Call({{"action", "state"}});
        const auto RecoveryPath = State["recoveryRoot"].toString() + "/" + Id + ".json"; QTRY_VERIFY(QFile::exists(RecoveryPath));
        auto Snapshot = Worker([&] { return Process->Snapshot(); }); QVERIFY(Snapshot.activeProcesses_ >= 1u); QVERIFY(Snapshot.totalProcesses_ >= 1u);
        const auto Runtime=Call({{"action","hostRuntime"}});QCOMPARE(Runtime["processLimit"].toInt(),0);QCOMPARE(Runtime["jobMemoryBytes"].toString(),QString("0"));QVERIFY(!Runtime["uiRestricted"].toBool());
        if (Forced) {
            auto Host = OpenProcess(PROCESS_TERMINATE, FALSE, Snapshot.processId_); QVERIFY(Host);
            QVERIFY(TerminateProcess(Host, 0xe0000200)); CloseHandle(Host);
        }
        else {
            auto Closed = Worker([&] { return Process->ViewerCall(WireKind::ViewerClose, {}); }); QVERIFY(Closed.HasValue());
        }
        Worker([&] { Process->Close(); return 0; }); Snapshot = Process->Snapshot(); QVERIFY(!Snapshot.running_); QCOMPARE(Snapshot.activeProcesses_, 0u);
        auto Record = QJsonDocument::fromJson(Read(RecoveryPath)).object(); QCOMPARE(QByteArray::fromBase64(Record["bytes"].toString().toLatin1()), QString("중계된 붙여넣기\n").toUtf8());
        DocumentRecoveryStore Recovery(State["recoveryRoot"].toString());
        QVERIFY(Recovery.Remove(Id.toStdString(), UINT64_MAX).HasValue()); // Only this test UUID.
    }
};
int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv);QTemporaryDir ViewConfig;App.setProperty("documentViewsConfigDir",ViewConfig.path());
    if (App.arguments().contains("--probe-dpr")) { std::cout << App.primaryScreen()->devicePixelRatio() << std::endl; return 0; }
    DocumentP0Tests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstDocumentP0.moc"
