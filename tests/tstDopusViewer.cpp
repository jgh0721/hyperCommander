#include "pluginHost/hostedViewerSession.hpp"
#include "pluginHost/hostedDataProviders.hpp"
#include "pluginHost/pipeChannel.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/qtViewerHost.hpp"
#include "memoryStorage.hpp"
#include <QtTest>
#include <QApplication>
#include <qt_windows.h>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <QScopeGuard>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QAbstractButton>
#include <TlHelp32.h>
#include <future>
#include <cstring>

using namespace hc::plugins;
using namespace hc::storage;
namespace
{
    template<class F> auto Worker(F&& Function)
    {
        auto Future = std::async(std::launch::async, std::forward<F>(Function));
        while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) QTest::qWait(5);
        return Future.get();
    }
    HWND Window(const QJsonObject& Reply, const char* Key)
    { return reinterpret_cast<HWND>(quintptr(Reply[Key].toString().toULongLong(nullptr, 16))); }
    std::shared_ptr<IContentSource> Content(const std::string& Name)
    {
        auto Provider = std::make_shared<hc::storage::test::MemoryProvider>();
        const auto Id = Provider->Add(Provider->Root(), Name, {std::byte{'x'}});
        struct VirtualSource final : IContentSource
        {
            std::shared_ptr<IContentSource> source_;
            EntrySnapshot Describe() const override { auto Entry = source_->Describe(); Entry.locator_ = {LocatorKind::Plugin, "test:memory:\\" + Entry.name_}; return Entry; }
            Task<Unit> Verify(OperationContext Context) override { return source_->Verify(Context); }
            Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context) override { return source_->OpenRead(Context); }
        };
        auto Source = std::make_shared<VirtualSource>(); Source->source_ = std::make_shared<ProviderContentSource>(Provider, Provider->Stat(Id, {}).get().Value()); return Source;
    }
}
class DopusViewerTests : public QObject
{
    Q_OBJECT
private:
    bool prepare(QTemporaryDir& Package, PluginViewerOptions& Options, bool Optional = true)
    {
        Options.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
        Options.manifestPath_ = Package.filePath("plugin.json"); Options.binaryPath_ = Package.filePath("viewer.dll");
        if (!QFile::copy(QCoreApplication::applicationDirPath() + "/hc_dopus_fixture.dll", Options.binaryPath_)) return false;
        QFile Binary(Options.binaryPath_); if (!Binary.open(QIODevice::ReadOnly)) return false;
        PluginManifest Manifest; Manifest.id_ = "hc.dopus.viewer"; Manifest.version_ = "1.0"; Manifest.binary_ = "viewer.dll";
        Manifest.sha256_ = QCryptographicHash::hash(Binary.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
        Manifest.format_ = PluginFormat::DopusViewer; Manifest.extensions_ = {Extension::Viewer};
        if (Optional) { Manifest.extensions_.insert(Extension::Metadata); Manifest.extensions_.insert(Extension::ContentConvert); }
        Manifest.permissions_ = {Permission::ReadContent, Permission::UserInterface};
        Options.approval_ = {Manifest, true, false, Manifest.permissions_}; Options.limits_.requestMs_ = 1000;
        QFile Json(Options.manifestPath_); return Json.open(QIODevice::WriteOnly) && Json.write(EncodeManifest(Manifest)) > 0;
    }
    QString file(QTemporaryDir& Directory, const QString& Name)
    { QFile Value(Directory.filePath(Name)); if (!Value.open(QIODevice::WriteOnly) || Value.write("x") != 1) return {}; return Value.fileName(); }
    bool documentFixture(QTemporaryDir& Package, PluginViewerOptions& Options)
    {
        if (!prepare(Package, Options)) return false;
        auto& Manifest = Options.approval_.reviewed_; Manifest.id_ = "builtin.hyperviewer"; Manifest.compatibility_ = "hyperviewer-v1";
        Manifest.binary_ = "Addons/WLX/HyperViewer/HyperViewer.dll"; Manifest.packageRoot_ = Package.path().toStdString(); Manifest.extensions_ = {Extension::Viewer};
        Manifest.permissions_.insert(Permission::ChildProcess); Manifest.permissions_.insert(Permission::WriteContent); Options.approval_.granted_ = Manifest.permissions_;
        const auto Binary = Package.filePath(QString::fromStdString(Manifest.binary_));
        if (!QDir().mkpath(QFileInfo(Binary).absolutePath()) || !QFile::rename(Options.binaryPath_, Binary)) return false;
        Options.binaryPath_ = Binary; QFile Json(Options.manifestPath_);
        return Json.open(QIODevice::WriteOnly) && Json.write(EncodeManifest(Manifest)) > 0;
    }
private slots:
    void queuedReplacementRetainsOnlyExistingGrants_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto* Mode : {"native-modal-detached", "local-before-send", "initial-sdk-rejection", "after-prepare-rejection"})
            QTest::newRow(Mode) << QString(Mode);
    }
    void queuedReplacementRetainsOnlyExistingGrants()
    {
        QFETCH(QString, Mode); const bool Local = Mode == "local-before-send", InitialFailure = Mode == "initial-sdk-rejection";
        QTemporaryDir Package, Input; PluginViewerOptions Options; QVERIFY(Local ? documentFixture(Package, Options) : prepare(Package, Options)); Options.limits_.heartbeatMs_ = 200;
        QWidget Parent, Other; Parent.resize(400, 300); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent)); const auto OtherParent = Other.winId();
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_); }); QVERIFY(Started.HasValue());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        // The observable different-parent rejection below exercises the actual
        // client grant fence; no unrestricted replacement profile is involved.
        QJsonObject Open{{"parent", QString::number(Parent.winId(), 16)}, {"file", file(Input, InitialFailure ? "reject.native" : "full.native")}, {"width", 400}, {"height", 300}, {"flags", 0}};
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open); }); HWND Old = nullptr;
        std::stop_source Stop; std::future<Result<QJsonObject>> PendingWork, PendingOpen;
        unsigned ModalMatches = 0;
        const auto ModalWindow = [&]() -> HWND {
            ModalMatches = 0; if (!IsWindow(Old)) return nullptr;
            struct ModalSearch { DWORD processId_, threadId_; HWND window_ = nullptr; unsigned matches_ = 0; };
            ModalSearch Search{Host->Snapshot().processId_, GetWindowThreadProcessId(Old, nullptr)};
            // Named properties use different atom tables inside this restricted
            // Job. Identify the single actual fixture dialog without relaxing it.
            EnumWindows([](HWND Window, LPARAM Param) -> BOOL {
                auto& Search = *reinterpret_cast<ModalSearch*>(Param); DWORD Pid = 0;
                const auto Thread = GetWindowThreadProcessId(Window, &Pid);
                if (Pid != Search.processId_ || Thread != Search.threadId_ || !IsWindowVisible(Window)) return TRUE;
                wchar_t Class[32]{}; if (GetClassNameW(Window, Class, 32) && wcscmp(Class, L"#32770") == 0) { Search.window_ = Window; ++Search.matches_; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&Search));
            ModalMatches = Search.matches_; return ModalMatches == 1 ? Search.window_ : nullptr;
        };
        const auto Release = qScopeGuard([&] { Stop.request_stop(); if (Old && IsWindow(Old)) {
            if (const auto Dialog = ModalWindow()) PostMessageW(Dialog, WM_CLOSE, 0, 0);
            if (Local) Worker([&] { DWORD_PTR Reply = 0; SendMessageTimeoutW(Old, WM_APP + 98, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &Reply); });
        } });
        bool Complete = false;
        const auto Diagnose = qScopeGuard([&] { if (Complete) return; const auto Dialog = ModalWindow(); DWORD DialogPid = 0; const auto Thread = GetWindowThreadProcessId(Dialog, &DialogPid);
            qWarning() << "Grant regression failure" << Mode << "old/modal/thread/pid/valid/visible/marker/matches" << quintptr(Old) << quintptr(Dialog) << Thread << DialogPid
                << bool(IsWindow(Dialog)) << bool(IsWindowVisible(Dialog)) << GetDlgCtrlID(Old) << ModalMatches << Host->Snapshot().runtime_;
        });
        const auto NativeQuery = [&](UINT Message, DWORD_PTR& Value) {
            DWORD Error = 0; const auto Ok = Worker([&] { SetLastError(0);
                const auto Sent = SendMessageTimeoutW(Old, Message, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &Value); Error = Sent ? 0 : GetLastError(); return bool(Sent); });
            if (!Ok) qWarning() << "Grant regression native query failed" << Mode << Message << Error << "old/parent/root/valid" << quintptr(Old)
                << quintptr(GetParent(Old)) << quintptr(GetAncestor(Old, GA_ROOT)) << bool(IsWindow(Old)) << Host->Snapshot().runtime_;
            return Ok;
        };
        if (InitialFailure) { QVERIFY(!Opened.HasValue()); QCOMPARE(Opened.Failure().code_, std::string("host-viewer-load-rejected")); }
        else { QVERIFY(Opened.HasValue()); Old = Window(Opened.Value(), "viewer"); QVERIFY(IsWindow(Old)); }
        if (InitialFailure || Mode == "after-prepare-rejection") {
            if (!InitialFailure) { Open["file"] = file(Input, "reject.native"); auto Rejected = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open); });
                QVERIFY(!Rejected.HasValue()); QCOMPARE(Rejected.Failure().code_, std::string("host-viewer-load-rejected")); QVERIFY(!IsWindow(Old)); }
            Open["parent"] = QString::number(OtherParent, 16); Open["file"] = file(Input, "full-recovered.native");
            Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open); }); QVERIFY(Opened.HasValue()); QVERIFY(IsChild(reinterpret_cast<HWND>(OtherParent), Window(Opened.Value(), "viewer")));
        } else {
            const auto OriginalParent = GetParent(Old), OriginalRoot = GetAncestor(Old, GA_ROOT); const auto OriginalId = GetDlgCtrlID(Old);
            DWORD_PTR CountBefore = 0, AbortBefore = 1; QVERIFY(NativeQuery(WM_APP + 97, CountBefore)); QCOMPARE(CountBefore, DWORD_PTR(1));
            QVERIFY(NativeQuery(WM_APP + 95, AbortBefore)); QCOMPARE(AbortBefore, DWORD_PTR(0));
            qInfo() << "Grant regression original hierarchy" << Mode << quintptr(Old) << quintptr(OriginalParent) << quintptr(OriginalRoot);
            if (Local) {
                OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                PendingWork = std::async(std::launch::async, [&, Context] { return Host->ViewerCall(WireKind::DocumentCommand, {{"action", "fixtureLargeWait"}}, Context); });
                QTRY_VERIFY_WITH_TIMEOUT(Host->Snapshot().runtime_["kind"].toInt() == int(WireKind::DocumentCommand) && Host->Snapshot().runtime_["workState"] == "Running", 1000);
                QTRY_VERIFY_WITH_TIMEOUT(PendingWork.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 1000);
                auto Detached = PendingWork.get(); QVERIFY(!Detached.HasValue()); QCOMPARE(Detached.Failure().code_, std::string("host-caller-detached"));
            } else {
                QVERIFY(PostMessageW(Old, WM_APP + 99, 0, 0)); QTRY_COMPARE_WITH_TIMEOUT(GetDlgCtrlID(Old), 101, 1000);
                QTRY_VERIFY_WITH_TIMEOUT(ModalWindow() != nullptr, 1000);
            }
            Open["file"] = file(Input, "full-replacement.native"); OperationContext Context; Context.stopToken_ = Stop.get_token();
            PendingOpen = std::async(std::launch::async, [&, Context, Request = Open] { return Host->ViewerCall(WireKind::ViewerOpen, Request, Context); });
            if (Local) QTRY_COMPARE_WITH_TIMEOUT(Host->Snapshot().runtime_["clientQueued"].toInt(), 1, 1000);
            else QTRY_VERIFY_WITH_TIMEOUT(Host->Snapshot().runtime_["kind"].toInt() == int(WireKind::ViewerOpen) && Host->Snapshot().runtime_["workState"] == "Queued", 1000);
            Stop.request_stop(); QTRY_VERIFY_WITH_TIMEOUT(PendingOpen.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 1000);
            const auto Cancelled = PendingOpen.get(); QVERIFY(!Cancelled.HasValue());
            if (Local) QCOMPARE(Cancelled.Failure().code_, std::string("host-cancelled-before-start"));
            else {
                QCOMPARE(Cancelled.Failure().code_, std::string("host-caller-detached")); auto Terminal = Worker([&] { return Host->AwaitPending(); });
                QVERIFY(!Terminal.HasValue()); QCOMPARE(Terminal.Failure().code_, std::string("host-cancelled-before-start"));
            }
            // The document fixture is a standalone workspace; its real parent
            // need not be the logical input anchor. Preserve the exact initial
            // hierarchy for both profiles instead of inventing an embedding.
            QVERIFY(IsWindow(Old)); QCOMPARE(GetParent(Old), OriginalParent); QCOMPARE(GetAncestor(Old, GA_ROOT), OriginalRoot);
            auto Foreign = Open; Foreign["parent"] = QString::number(OtherParent, 16); OperationContext ForeignWait; ForeignWait.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            const auto Denied = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Foreign, ForeignWait); }); QVERIFY(!Denied.HasValue()); QCOMPARE(Denied.Failure().code_, std::string("host-viewer-already-open"));
            DWORD_PTR Reply = 0;
            if (Local) { QVERIFY(NativeQuery(WM_APP + 98, Reply)); auto Work = Worker([&] { return Host->AwaitPending(); }); QVERIFY(Work.HasValue()); QVERIFY(!Work.Value()["abortSignaled"].toBool()); }
            else {
                const auto Dialog = ModalWindow(); QVERIFY(IsWindow(Dialog)); DWORD DialogPid = 0; QVERIFY(GetWindowThreadProcessId(Dialog, &DialogPid)); QCOMPARE(DialogPid, Host->Snapshot().processId_);
                SetLastError(0); const auto Posted = PostMessageW(Dialog, WM_CLOSE, 0, 0); const auto Error = GetLastError();
                QVERIFY2(Posted, qPrintable(QString("actual modal close error=%1 hwnd=%2").arg(Error).arg(quintptr(Dialog), 0, 16)));
                QTRY_VERIFY_WITH_TIMEOUT(GetDlgCtrlID(Old) == OriginalId && ModalWindow() == nullptr, 1000);
            }
            // Query the DLL window only after the actual modal/SDK loop exits,
            // with the caller GUI pumping to avoid sent-message activation cycles.
            DWORD_PTR CountAfter = 0, Abort = 1; QVERIFY(NativeQuery(WM_APP + 97, CountAfter)); QCOMPARE(CountAfter, CountBefore);
            QVERIFY(NativeQuery(WM_APP + 95, Abort)); QCOMPARE(Abort, DWORD_PTR(0));
            QVERIFY(Worker([&] { return Host->Ping(); }).HasValue()); QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerResize, {{"width", 320}, {"height", 200}}); }).HasValue()); QVERIFY(IsWindow(Old));
        }
        QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue()); Worker([&] { Host->Close(); });
        QVERIFY(!Host->Snapshot().forcedTermination_); QCOMPARE(Host->Snapshot().exitCode_.value_or(1), 0u); Complete = true;
    }
    void completedSdkWithStoppedResponseWriter()
    {
        QTemporaryDir Package, Input; PluginViewerOptions Options; QVERIFY(documentFixture(Package, Options)); Options.limits_.heartbeatMs_ = 200;
        QWidget Parent; Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_); }); QVERIFY(Started.HasValue());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, {{"parent", QString::number(Parent.winId(), 16)}, {"file", file(Input, "full.native")}, {"width", 400}, {"height", 300}, {"flags", 0}}); }); QVERIFY(Opened.HasValue());
        const auto Viewer = Window(Opened.Value(), "viewer"); OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        auto Pending = std::async(std::launch::async, [&] { return Host->ViewerCall(WireKind::DocumentCommand, {{"action", "fixtureLargeWait"}}, Context); });
        QTRY_VERIFY_WITH_TIMEOUT(Host->Snapshot().runtime_["kind"].toInt() == int(WireKind::DocumentCommand)
            && Host->Snapshot().runtime_["workState"] == "Running", 1000);
        const auto Pid = Host->Snapshot().processId_; WinHandle Threads(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)); QVERIFY(Threads.Valid());
        THREADENTRY32 Entry{sizeof(Entry)}; QVERIFY(Thread32First(Threads.Get(), &Entry)); WinHandle Suspended;
        do {
            if (Entry.th32OwnerProcessID != Pid) continue;
            WinHandle Thread(OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME, FALSE, Entry.th32ThreadID)); QVERIFY(Thread.Valid());
            PWSTR Name = nullptr; if (FAILED(GetThreadDescription(Thread.Get(), &Name))) continue;
            const bool Match = wcscmp(Name, L"HC viewer command I/O") == 0; LocalFree(Name);
            if (Match) { QVERIFY(SuspendThread(Thread.Get()) != DWORD(-1)); Suspended = std::move(Thread); break; }
        } while (Thread32Next(Threads.Get(), &Entry));
        QVERIFY(Suspended.Valid()); const auto Resume = qScopeGuard([&] { ResumeThread(Suspended.Get()); });
        QElapsedTimer Timer; Timer.start(); QVERIFY(PostMessageW(Viewer, WM_APP + 98, 0, 0));
        QTRY_COMPARE_WITH_TIMEOUT(Host->Snapshot().runtime_["workState"].toString(), QString("Completed"), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(Pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 4000);
        const auto Reply = Pending.get(); QVERIFY(!Reply.HasValue()); QCOMPARE(Reply.Failure().userAction_, std::string("result-unknown"));
        QTRY_VERIFY_WITH_TIMEOUT(!Host->Snapshot().running_, 1000); const auto Snapshot = Host->Snapshot(); QVERIFY(Snapshot.failure_);
        QCOMPARE(Snapshot.failure_->code_, std::string("host-command-delivery-stalled")); QCOMPARE(Snapshot.runtime_["probeFailures"].toInt(), 0);
        qInfo() << "Completed SDK response writer stall reaped ms" << Timer.elapsed() << Snapshot.runtime_;
    }
    void cancellationDuringPreviousViewerTeardown()
    {
        QTemporaryDir Package, Input; PluginViewerOptions Options; QVERIFY(prepare(Package, Options)); Options.limits_.heartbeatMs_ = 200;
        QWidget Parent; Parent.resize(400, 300); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_); }); QVERIFY(Started.HasValue());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        QJsonObject Open{{"parent", QString::number(Parent.winId(), 16)}, {"file", file(Input, "full-slowclose.native")}, {"width", 400}, {"height", 300}, {"flags", 0}};
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open); }); QVERIFY(Opened.HasValue()); const auto Old = Window(Opened.Value(), "viewer");
        Open["file"] = file(Input, "full-cancel.native"); std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        auto Pending = std::async(std::launch::async, [&] { return Host->ViewerCall(WireKind::ViewerOpen, Open, Context); });
        QTRY_COMPARE_WITH_TIMEOUT(GetDlgCtrlID(Old), 96, 2000);
        Stop.request_stop();
        QTRY_VERIFY_WITH_TIMEOUT(Pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 1000);
        const auto Detached = Pending.get(); QVERIFY(!Detached.HasValue()); QCOMPARE(Detached.Failure().code_, std::string("host-caller-detached"));
        QTRY_COMPARE_WITH_TIMEOUT(Host->Snapshot().runtime_["cancelState"].toString(), QString("CancellationRequested"), 1000);
        QVERIFY(PostMessageW(Old, WM_APP + 96, 0, 0));
        auto Cancelled = Worker([&] { return Host->AwaitPending(); }); QVERIFY(!Cancelled.HasValue());
        QCOMPARE(Cancelled.Failure().code_, std::string("host-cancelled")); QVERIFY(!IsWindow(Old));
        Open["file"] = file(Input, "full.native");
        Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open); }); QVERIFY(Opened.HasValue());
        DWORD_PTR Count = 0; QVERIFY(SendMessageTimeoutW(Window(Opened.Value(), "viewer"), WM_APP + 97, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &Count));
        QCOMPARE(Count, DWORD_PTR(2)); // The cancelled replacement never entered IdentifyFile.
        QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue());
        Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().forcedTermination_);
    }
    void detachedLargeDocumentResponse_data()
    {
        QTest::addColumn<bool>("Rejoin");
        QTest::newRow("discard-late-result") << false;
        QTest::newRow("rejoin-terminal-slot") << true;
    }
    void detachedLargeDocumentResponse()
    {
        QFETCH(bool, Rejoin);
        QTemporaryDir Package, Input; PluginViewerOptions Options; QVERIFY(documentFixture(Package, Options));
        // Exercise the document ABI without weakening its production admission
        // rules: use a fully valid, explicitly approved built-in-shaped fixture.
        Options.limits_.heartbeatMs_ = 200;
        QWidget Parent; Parent.resize(400, 300); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_); });
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Path = file(Input, "full.native");
        const QJsonObject Open{{"parent", QString::number(Parent.winId(), 16)}, {"file", Path}, {"width", 400}, {"height", 300}, {"flags", 0}};
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open); }); QVERIFY(Opened.HasValue());
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        auto Reply = Worker([&] { return Host->ViewerCall(WireKind::DocumentCommand, {{"action", "fixtureLarge"}}, Context); });
        QVERIFY(!Reply.HasValue()); QCOMPARE(Reply.Failure().code_, std::string("host-caller-detached"));
        QVERIFY(Host->Snapshot().running_); QVERIFY(Host->Snapshot().callerDetached_);
        QTRY_COMPARE_WITH_TIMEOUT(Host->Snapshot().runtime_["cancelState"].toString(), QString("CancellationNotSupported"), 1000);
        OperationContext Next; Next.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        // A 700 KiB response is consumed after the caller left, before this ping.
        if (Rejoin) {
            auto Terminal = Worker([&] { return Host->AwaitPending(Next); });
            QVERIFY(Terminal.HasValue()); QCOMPARE(Terminal.Value()["padding"].toString().size(), 700 * 1024);
            QCOMPARE(Terminal.Value()["completed"].toInt(), 1);
        }
        QVERIFY(Worker([&] { return Host->Ping(Next); }).HasValue());
        auto State = Worker([&] { return Host->ViewerCall(WireKind::DocumentCommand, {{"action", "fixtureState"}}, Next); });
        QVERIFY(State.HasValue()); QCOMPARE(State.Value()["completed"].toInt(), 1); QVERIFY(!State.Value()["abortSignaled"].toBool());
        State = Worker([&] { return Host->ViewerCall(WireKind::DocumentCommand, {{"action", "fixtureState"}}, Next); });
        QVERIFY(State.HasValue()); QVERIFY(!State.Value()["abortSignaled"].toBool());
        QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}, Next); }).HasValue());
        QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open, Next); }).HasValue());
        Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().forcedTermination_);
    }
    void dataCancellationKeepsViewerAlive()
    {
        QTemporaryDir Package, Input; PluginViewerOptions Options; QVERIFY(prepare(Package, Options)); Options.limits_.heartbeatMs_ = 200;
        QWidget Parent; Parent.resize(400, 300); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_); }); QVERIFY(Started.HasValue());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Path = file(Input, "full.native");
        const QJsonObject Open{{"parent", QString::number(Parent.winId(), 16)}, {"file", Path}, {"width", 400}, {"height", 300}, {"flags", 0}};
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open); }); QVERIFY(Opened.HasValue()); const auto Viewer = Window(Opened.Value(), "viewer");
        OperationContext Data; Data.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        const auto DataPath = file(Input, "cooperative-data.bitmap");
        auto Cancelled = Worker([&] { return Host->DataCall(WireKind::MetadataQuery,
            {{"action", "value"}, {"path", DataPath}, {"field", 0}, {"unit", 0}, {"defer", false}}, Data); });
        QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().code_, std::string("host-caller-detached"));
        QTRY_COMPARE_WITH_TIMEOUT(Host->Snapshot().runtime_["cancelState"].toString(), QString("CancellationRequested"), 1000);
        OperationContext Next; Next.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        QVERIFY(Worker([&] { return Host->Ping(Next); }).HasValue());
        DWORD_PTR Signalled = 1; QVERIFY(SendMessageTimeoutW(Viewer, WM_APP + 95, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &Signalled));
        QCOMPARE(Signalled, DWORD_PTR(0)); QVERIFY(IsWindow(Viewer));
        QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}, Next); }).HasValue());
        QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Open, Next); }).HasValue());
        Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().forcedTermination_);
    }
    void archiveClosePreservesDopusDraft()
    {
        QTemporaryDir Package; PluginViewerOptions Options; QVERIFY(prepare(Package, Options));
        Options.approval_.reviewed_.permissions_.insert(Permission::WriteContent); Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
        QFile Manifest(Options.manifestPath_); QVERIFY(Manifest.open(QIODevice::WriteOnly)); Manifest.write(EncodeManifest(Options.approval_.reviewed_)); Manifest.close();
        auto Provider = std::make_shared<hc::storage::test::MemoryProvider>();
        const auto Id = Provider->Add(Provider->Root(), "preview.native", {std::byte{'x'}});
        auto Source = std::make_shared<ProviderContentSource>(Provider, Provider->Stat(Id, {}).get().Value());
        QByteArray Published;
        Source->SetArchiveWriteback([&](const std::string& Path, OperationContext) -> Result<Unit> {
            QFile Draft(QString::fromStdString(Path)); if (!Draft.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Unavailable, "draft-missing"};
            Published = Draft.readAll(); return Unit{};
        });
        QWidget Parent; Parent.show(); PluginViewerService Service([&] { return MakeNativeMaterializer(Package.path().toStdString()); });
        const auto Cleanup = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(5); });
        Options.purpose_ = hc::viewer::ViewerPurpose::QuickPreview;
        auto Opened = Service.Open(Options, Source, QString::number(Parent.winId(), 16), 400, 300); QVERIFY(Opened.HasValue()); auto Session = Opened.Value();
        QTRY_COMPARE_WITH_TIMEOUT(Session->Snapshot().status_, hc::viewer::ViewerStatus::Ready, 8000);
        QVERIFY(Session->Details().externalArchive_); const auto Path = Session->Details().externalArchiveDraft_;
        QFile Draft(Path); QVERIFY(Draft.open(QIODevice::WriteOnly)); Draft.write("DOpus saved draft"); Draft.close();
        auto Closed = Session->CloseArchive(); QVERIFY(Closed.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Session->Details().commandSequence_ >= Closed.Value(), 5000);
        QVERIFY(!Session->Details().commandError_); QVERIFY(Session->Details().externalArchiveClosed_); QVERIFY(Published.isEmpty());
        auto Commit = Session->CloseArchive(1); QVERIFY(Commit.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Session->Details().commandSequence_ >= Commit.Value(), 5000);
        QVERIFY(Session->Details().externalArchiveDone_); QCOMPARE(Published, QByteArray("DOpus saved draft"));
        Session->Close(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 5000); QVERIFY(!QFileInfo::exists(Path));
    }
    void applicationScopeSurvivesForcedSelection()
    {
        QTemporaryDir Package; PluginViewerOptions Options; QVERIFY(prepare(Package, Options));
        Options.applicationRule_ = QJsonObject{{"version", 1}, {"enabled", true}, {"mode", "extensions"}, {"extensions", QJsonArray{"text"}}};
        QWidget Parent; Parent.resize(400, 300); Parent.show();
        PluginViewerService Service([&] { return MakeNativeMaterializer(Package.path().toStdString()); });
        const auto Cleanup = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(5); });
        PluginViewerSelectionPolicy Policy; Policy.force_ = true;
        auto Excluded = Service.OpenAutomatic({Options}, Content("sample.bitmap"), QString::number(Parent.winId(), 16), 400, 300, Policy);
        QVERIFY(Excluded.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Excluded.Value()->Details().idle_, 5000);
        QVERIFY(Excluded.Value()->Details().noMatch_); QVERIFY(Excluded.Value()->Snapshot().status_ != hc::viewer::ViewerStatus::Ready);
        Options.applicationRule_ = QJsonObject{{"version", 1}, {"enabled", true}, {"mode", "pattern"}, {"expression", "*.bitmap"}};
        auto Included = Service.OpenAutomatic({Options}, Content("sample.bitmap"), QString::number(Parent.winId(), 16), 400, 300, Policy);
        QVERIFY(Included.HasValue()); QTRY_COMPARE_WITH_TIMEOUT(Included.Value()->Snapshot().status_, hc::viewer::ViewerStatus::Ready, 5000);
        Included.Value()->Close(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 5000);
    }
    void nativeAndBitmapLifecycle_data()
    {
        QTest::addColumn<QString>("Name");
        QTest::newRow("bitmap") << "한글.bitmap";
        QTest::newRow("native") << "full-한글.native";
        QTest::newRow("preview") << "preview-한글.native";
        QTest::newRow("plain-text") << "한글.text";
    }
    void nativeAndBitmapLifecycle()
    {
        QFETCH(QString, Name); QTemporaryDir Package; PluginViewerOptions Options; QVERIFY(prepare(Package, Options));
        QWidget Parent; Parent.resize(480, 320); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_); });
        QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Path = file(Package, Name); QVERIFY(!Path.isEmpty());
        auto Probe = Worker([&] { return Host->Probe(); }); QVERIFY(Probe.HasValue()); QCOMPARE(Probe.Value()["format"].toString(), QString("dopus-viewer"));
        for (int Index = 0; Index < 8; ++Index)
        {
            QJsonObject Request{{"parent", QString::number(Parent.winId(), 16)}, {"file", Path}, {"width", 480}, {"height", 320}, {"flags", 0}};
            if (Name.startsWith("preview")) Request["preview"] = true;
            auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
            QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : Opened.Failure().code_.c_str());
            const auto Child = Window(Opened.Value(), "viewer"); QVERIFY(IsWindow(Child)); QVERIFY(IsChild(reinterpret_cast<HWND>(Parent.winId()), Child));
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerResize, {{"width", 300}, {"height", 200}}); }).HasValue());
            RECT Rect{}; QVERIFY(GetWindowRect(Child, &Rect)); QCOMPARE(Rect.right - Rect.left, 300L); QCOMPARE(Rect.bottom - Rect.top, 200L);
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerFocus, {}); }).HasValue());
            if (Name.endsWith(".native") || Name.endsWith(".text"))
            {
                wchar_t Text[1024]{}; DWORD_PTR Length = 0; QVERIFY(SendMessageTimeoutW(Child, WM_GETTEXT, 1024, reinterpret_cast<LPARAM>(Text), SMTO_ABORTIFHUNG, 1000, &Length));
                QVERIFY(QString::fromWCharArray(Text).contains(Name.endsWith(".native") ? Name : QString("converted fixture text")));
            }
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue()); QVERIFY(!IsWindow(Child));
            // Native destruction must see a signalled abort; a later metadata
            // query in the same process must see it reset by the host.
            auto Value = Worker([&] { return Host->DataCall(WireKind::MetadataQuery,
                {{"action", "value"}, {"path", Path}, {"field", 1}, {"unit", 0}, {"defer", false}}); });
            QVERIFY(Value.HasValue()); QCOMPARE(Value.Value()["value"].toString(), QString("2"));
        }
        Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().forcedTermination_); QCOMPARE(Host->Snapshot().exitCode_.value_or(1), 0u);
    }
    void rejectedOpenAndConversionRecovery()
    {
        QTemporaryDir Package; PluginViewerOptions Options; QVERIFY(prepare(Package, Options));
        QWidget Parent; Parent.show();
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_); }); QVERIFY(Started.HasValue());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Path = file(Package, "reject.native");
        QJsonObject Request{{"parent", QString::number(Parent.winId(), 16)}, {"file", Path}, {"width", 100}, {"height", 100}, {"flags", 0}, {"preview", "invalid"}};
        auto Invalid = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        QVERIFY(!Invalid.HasValue()); QCOMPARE(Invalid.Failure().category_, ErrorCategory::InvalidRequest);
        Request["preview"] = false;
        auto Rejected = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        QVERIFY(!Rejected.HasValue()); QCOMPARE(Rejected.Failure().code_, std::string("host-viewer-load-rejected"));
        const auto Text = file(Package, "recovery.text");
        auto Converted = Worker([&] { return Host->DataCall(WireKind::ContentConvert, {{"path", Text}}); });
        QVERIFY(Converted.HasValue()); QCOMPARE(QByteArray::fromBase64(Converted.Value()["bytes"].toString().toLatin1()), QByteArray("converted fixture text"));
        Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().forcedTermination_); QCOMPARE(Host->Snapshot().exitCode_.value_or(1), 0u);
    }
    void optionalServicesAndDenial()
    {
        for (bool Optional : {true, false})
        {
            QTemporaryDir Package; PluginViewerOptions Options; QVERIFY(prepare(Package, Options, Optional));
            auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_); }); QVERIFY(Started.HasValue());
            auto Host = std::shared_ptr<PluginProcess>(std::move(Started.Value())); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host->Close(); }); });
            const auto Path = file(Package, "input.text");
            auto Converted = Worker([&] { return Host->DataCall(WireKind::ContentConvert, {{"path", Path}}); });
            if (!Optional)
            {
                QVERIFY(!Converted.HasValue()); QCOMPARE(Converted.Failure().category_, ErrorCategory::AccessDenied);
                QVERIFY(!Worker([&] { return Host->DataCall(WireKind::MetadataQuery, {{"action", "fields"}}); }).HasValue()); continue;
            }
            QVERIFY(Converted.HasValue()); QCOMPARE(QByteArray::fromBase64(Converted.Value()["bytes"].toString().toLatin1()), QByteArray("converted fixture text"));
            const auto Large = file(Package, "large.text");
            auto OverBudget = Worker([&] { return Host->DataCall(WireKind::ContentConvert, {{"path", Large}}); });
            QVERIFY(!OverBudget.HasValue()); QCOMPARE(OverBudget.Failure().category_, ErrorCategory::BudgetExceeded);
            for (const auto& [Name, Type] : std::vector<std::pair<QString, int>>{{"html.text", 2}, {"rich.text", 1}})
            {
                const auto Input = file(Package, Name);
                auto Reply = Worker([&] { return Host->DataCall(WireKind::ContentConvert, {{"path", Input}}); }); QVERIFY(Reply.HasValue()); QCOMPARE(Reply.Value()["textType"].toInt(), Type);
            }
            auto Materializer = MakeNativeMaterializer(Package.path().toStdString()); QVERIFY(Materializer.HasValue());
            auto Metadata = Worker([&] { return MakeHostedDopusMetadataProvider(Host, Materializer.Value()); }); QVERIFY(Metadata.HasValue());
            auto Fields = Worker([&] { return Metadata.Value()->Fields({}).get(); }); QVERIFY(Fields.HasValue()); QCOMPARE(Fields.Value().size(), 6u);
            auto Value = Worker([&] { return Metadata.Value()->Query(Content("sample.bitmap"), {1, 0, false}, {}).get(); });
            QVERIFY(Value.HasValue()); QCOMPARE(std::get<std::int64_t>(Value.Value().value_), 2);
            auto Converter = Worker([&] { return MakeHostedDopusConverter(Host, Materializer.Value()); }); QVERIFY(Converter.HasValue());
            auto ContentResult = Worker([&] { return Converter.Value()->Convert(Content("sample.text"), {}).get(); }); QVERIFY(ContentResult.HasValue());
            QCOMPARE(ContentResult.Value().type_, ConvertedTextType::Plain); QCOMPARE(ContentResult.Value().bytes_.size(), 22u);
            Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().forcedTermination_); QCOMPARE(Host->Snapshot().exitCode_.value_or(1), 0u);
        }
    }
    void automaticSurfaceAndCancellation()
    {
        QTemporaryDir Package; PluginViewerOptions Options; QVERIFY(prepare(Package, Options));
        QWidget Parent; Parent.resize(500, 300); Parent.show();
        auto Mode = QtViewerSurface::PluginViewerSelectionMode::Automatic;
        QtViewerHost Host(&Parent, [&] { return MakeNativeMaterializer(Package.path().toStdString()); },
            [&] { return std::vector<QtViewerSurface::PluginViewerChoice>{{"dopus", "DOpus", Options}}; }, [&] { return Mode; });
        const auto Cleanup = qScopeGuard([&] { Host.CloseAll(); while (!Host.IsIdle()) QTest::qWait(5); });
        auto* Surface = Host.CreateSurface(&Parent); Surface->resize(500, 300); Surface->show();
        QSignalSpy Ready(Surface, &QtViewerSurface::Ready), Failed(Surface, &QtViewerSurface::Failed);
        const auto Failure = [&] {
            const auto Details = Surface->findChild<QtPluginViewerSurface*>()->Details();
            const auto Outer = Surface->Snapshot();
            return QString::fromStdString(Details.viewer_.error_ ? Details.viewer_.error_->code_ : Details.commandError_ ? Details.commandError_->code_ : "not-ready") + " (" + Details.preparationStage_ + ") provider="
                + QString::fromStdString(Outer.provider_) + " failed=" + QString::number(Failed.size()) + " pinned=" + QString::number(Surface->IsPinned());
        };
        // Ctrl+Q supplies its own reviewed candidate list, independently of
        // the full viewer's selection preference, through OpenPreview.
        Surface->OpenPreview(Content("sample.bitmap"), {{"dopus", "DOpus", Options}});
        QTRY_VERIFY2_WITH_TIMEOUT(Surface->IsReady(), qPrintable(Failure()), 5000); QCOMPARE(Failed.size(), 0); QCOMPARE(Ready.size(), 1);
        Surface->Close(); QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 4000);
        unsigned Confirmations = 0; QTimer Confirm; Confirm.setInterval(10);
        connect(&Confirm, &QTimer::timeout, this, [&] {
            if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                if (auto* Yes = Box->button(QMessageBox::Yes)) { ++Confirmations; Yes->click(); }
        }); Confirm.start();
        for (const auto Selection : {QtViewerSurface::PluginViewerSelectionMode::Automatic, QtViewerSurface::PluginViewerSelectionMode::Manual})
            for (const auto Purpose : {hc::viewer::ViewerPurpose::QuickPreview, hc::viewer::ViewerPurpose::FullViewer})
            {
                Mode = Selection;
                qInfo() << "Surface selection" << int(Selection) << "purpose" << int(Purpose);
                if (Purpose == hc::viewer::ViewerPurpose::QuickPreview)
                    Surface->OpenPreview(Content("preview.native"), {{"dopus", "DOpus", Options}});
                else Surface->Open(Content("full.native"), Purpose);
                if (Selection == QtViewerSurface::PluginViewerSelectionMode::Manual)
                {
                    auto* Menu = Surface->findChild<QMenu*>("hc.viewer.plugins"); QVERIFY(Menu); Menu->aboutToShow();
                    auto* Action = Menu->findChild<QAction*>("hc.viewer.plugin.dopus"); QVERIFY(Action); Action->trigger();
                }
                QTRY_VERIFY2_WITH_TIMEOUT(Surface->IsReady(), qPrintable(Failure()), 5000); QVERIFY(Surface->Snapshot().provider_.starts_with("dopus-viewer."));
                Surface->Execute("cancel"); QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 4000);
            }
        QVERIFY(Confirmations > 0); // The focused native preview requires an explicit close decision.
    }
    void crashAndTimeout_data()
    { QTest::addColumn<QString>("Name"); QTest::newRow("crash") << "crash.native"; QTest::newRow("timeout") << "hang.native"; }
    void crashAndTimeout()
    {
        QFETCH(QString, Name); QTemporaryDir Package; PluginViewerOptions Options; QVERIFY(prepare(Package, Options));
        QWidget Parent; Parent.show();
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_); }); QVERIFY(Started.HasValue());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Input = file(Package, Name); QElapsedTimer Timer; Timer.start();
        auto Result = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, {{"parent", QString::number(Parent.winId(), 16)}, {"file", Input}, {"width", 100}, {"height", 100}, {"flags", 0}}); });
        QVERIFY(!Result.HasValue()); QVERIFY(Timer.elapsed() < 4000); QTRY_VERIFY_WITH_TIMEOUT(!Host->Snapshot().running_, 2000);
    }
};
QTEST_MAIN(DopusViewerTests)
#include "tstDopusViewer.moc"
