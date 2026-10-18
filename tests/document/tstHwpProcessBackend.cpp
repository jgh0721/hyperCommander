#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "UniqueLibs/document/hwpProcessBackend.hpp"
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QProcess>
#include <QApplication>
using namespace hc::document;
class ProcessLease final : public hc::storage::IMaterializedFileLease
{
public:
    std::string LocalPath() const override { return "fixture.hwp"; }
    hc::storage::EntrySnapshot Describe() const override { return {}; }
    std::size_t Depth() const override { return 0; }
};
class HwpProcessTests final : public QObject
{
    Q_OBJECT
private:
    HWND findProxy()
    {
        HWND Found = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND Window, LPARAM Context) -> BOOL {
            EnumChildWindows(Window, [](HWND Child, LPARAM Out) -> BOOL {
                if(GetPropW(Child, L"HyperCommander.RhwpProxyOwner")) {
                    *reinterpret_cast<HWND*>(Out) = Child; return FALSE;
                }
                return TRUE;
            }, Context);
            return *reinterpret_cast<HWND*>(Context) ? FALSE : TRUE;
        }, reinterpret_cast<LPARAM>(&Found));
        return Found;
    }
    WebProcessRuntime prepare(QTemporaryDir& Directory, const QByteArray& Mode = {})
    {
        WebProcessRuntime Runtime; Runtime.root_ = Directory.path();
        if(!QFile::copy(QCoreApplication::applicationDirPath()+"/hc_web_process_fixture.exe",
            Directory.filePath("HyperEditorWeb.exe"))) return {};
        QFile Config(Directory.filePath("mode.txt"));
        if(!Config.open(QIODevice::WriteOnly) || Config.write(Mode) != Mode.size()) return {};
        Config.close();
        for(const auto Name : {"HyperEditorWeb.exe", "mode.txt"}) {
            QFile File(Directory.filePath(Name)); if(!File.open(QIODevice::ReadOnly)) return {};
            Runtime.sha256_[Name] = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex();
        }
        return Runtime;
    }
    bool locked(const QTemporaryDir& Directory)
    {
        QFile File(Directory.filePath("mode.txt"));
        return !File.open(QIODevice::ReadWrite);
    }
    WebProcessLimits limits() { return {4, 512ULL*1024*1024, 1500}; }
private slots:
    void initTestCase()
    {
        qRegisterMetaType<DocumentOperationTicket>(); qRegisterMetaType<HwpLoadResult>();
    }
    void cleanup()
    {
        QTRY_VERIFY_WITH_TIMEOUT(HwpProcessBackend::IsDrained(), 10000);
    }
    void applicationQuitWaitsForOrphanedOwner()
    {
        QTemporaryDir Directory; const auto Runtime = prepare(Directory, "hang-ready"); QVERIFY(!Runtime.sha256_.isEmpty());
        QProcess Probe;
        Probe.start(QCoreApplication::applicationFilePath(), {"--shutdown-probe", Directory.path()});
        QVERIFY(Probe.waitForStarted(5000)); QVERIFY(Probe.waitForFinished(15000));
        QCOMPARE(Probe.exitStatus(), QProcess::NormalExit); QCOMPARE(Probe.exitCode(), 0);
    }
    void bindingGenerationAndOwnerAreChecked()
    {
        QTemporaryDir Directory; const auto Runtime = prepare(Directory, "hang-ready");
        HwpProcessBackend A(Runtime, limits()), B(Runtime, limits());
        A.Load(30, {}, {}); B.Load(31, {}, {});
        const auto Old = A.NativeBinding(), Other = B.NativeBinding();
        QVERIFY(Old.owner_ && Old.generation_); QVERIFY(A.IsCurrentBinding(Old));
        QVERIFY(B.IsCurrentBinding(Other)); QVERIFY(!A.IsCurrentBinding(Other));
        A.InvalidateBinding(Other); QVERIFY(A.IsCurrentBinding(Old));
        A.InvalidateBinding(Old); QVERIFY(!A.IsCurrentBinding(Old));
        QVERIFY(A.NativeBinding().generation_ > Old.generation_);
        QVERIFY(B.IsCurrentBinding(Other)); B.InvalidateBinding(Other);
        QTRY_VERIFY_WITH_TIMEOUT(HwpProcessBackend::IsDrained(), 10000);
    }
    void nativeProxyParentChangeInvalidatesBinding()
    {
        QTemporaryDir Directory; const auto Runtime = prepare(Directory, "hang-ready");
        HwpProcessBackend Backend(Runtime, limits()); QWidget Other;
        Backend.Load(33, {}, {}); const auto Old = Backend.NativeBinding();
        const auto Proxy = findProxy(); QVERIFY(Proxy); QVERIFY(Backend.IsCurrentBinding(Old));
        QVERIFY(SetParent(Proxy, reinterpret_cast<HWND>(Other.winId())));
        QVERIFY(!Backend.IsCurrentBinding(Old));
        QTRY_VERIFY_WITH_TIMEOUT(HwpProcessBackend::IsDrained(), 10000);
        QVERIFY(!IsWindow(Proxy));
    }
    void widgetReparentInvalidatesBinding()
    {
        QTemporaryDir Directory; const auto Runtime = prepare(Directory, "hang-ready");
        QWidget NewParent; HwpProcessBackend Backend(Runtime, limits());
        Backend.Load(32, {}, {}); const auto Old = Backend.NativeBinding();
        QVERIFY(Backend.IsCurrentBinding(Old)); Backend.setParent(&NewParent);
        QVERIFY(!Backend.IsCurrentBinding(Old));
        QTRY_VERIFY_WITH_TIMEOUT(HwpProcessBackend::IsDrained(), 10000);
        Backend.setParent(nullptr);
    }
    void transportDoesNotCompleteDocument()
    {
        QTemporaryDir Directory; auto Runtime = prepare(Directory); QVERIFY(!Runtime.sha256_.isEmpty());
        auto Backend = std::make_unique<HwpProcessBackend>(Runtime, limits()); auto* Port = Backend.get();
        QSignalSpy Stopped(Port, &HwpSdiBackend::Stopped);
        HwpSdiWindow Window("cold", {}, std::move(Backend));
        QVERIFY(Window.Open()); QVERIFY(Window.IsBusy());
        QTRY_COMPARE_WITH_TIMEOUT(Stopped.count(), 1, 10000);
        QTRY_VERIFY(!Window.IsBusy()); QVERIFY(!Window.Capabilities().view_);
        QVERIFY(Window.Snapshot().error_.has_value());
        QCOMPARE(Window.Snapshot().error_->code_, std::string("web-owner-child-window"));
        QVERIFY(!locked(Directory));
        Window.RequestClose(); QTRY_COMPARE(Window.Snapshot().lifecycle_, DocumentLifecycle::Closed);
    }
    void cancellationWaitsForStartupRetirement()
    {
        QTemporaryDir Directory; auto Runtime = prepare(Directory, "hang-ready"); QVERIFY(!Runtime.sha256_.isEmpty());
        auto Backend = std::make_unique<HwpProcessBackend>(Runtime, limits()); auto* Port = Backend.get();
        QSignalSpy Stopped(Port, &HwpSdiBackend::Stopped);
        HwpSdiWindow Window("cancel", {}, std::move(Backend));
        int Ticks = 0; QTimer Heartbeat; Heartbeat.setInterval(5);
        connect(&Heartbeat, &QTimer::timeout, this, [&] { ++Ticks; }); Heartbeat.start();
        QVERIFY(Window.Open()); QTRY_VERIFY_WITH_TIMEOUT(locked(Directory), 1000);
        QElapsedTimer Elapsed; Elapsed.start(); Window.RequestClose();
        QVERIFY(Elapsed.elapsed() < 200); QVERIFY(Window.IsBusy()); QCOMPARE(Stopped.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(Window.Snapshot().lifecycle_, DocumentLifecycle::Closed, 10000);
        QCOMPARE(Stopped.count(), 1); QVERIFY(Ticks > 10); QVERIFY(!locked(Directory));
    }
    void widgetDeletionRetainsInputUntilRetired()
    {
        QTemporaryDir Directory; auto Runtime = prepare(Directory, "hang-ready"); QVERIFY(!Runtime.sha256_.isEmpty());
        auto Backend = std::make_unique<HwpProcessBackend>(Runtime, limits());
        auto Lease = std::make_shared<ProcessLease>(); std::weak_ptr<ProcessLease> Weak = Lease;
        DocumentOperationTicket Ticket; Ticket.lease_ = Lease;
        SourceIdentity Source; Source.lease_ = Lease;
        Backend->Load(7, Ticket, Source); Ticket.lease_.reset(); Source.lease_.reset(); Lease.reset();
        QTRY_VERIFY_WITH_TIMEOUT(locked(Directory), 1000);
        const auto Proxy = findProxy(); QVERIFY(Proxy);
        QElapsedTimer Elapsed; Elapsed.start(); Backend.reset();
        QVERIFY(Elapsed.elapsed() < 200); QVERIFY(!Weak.expired()); QVERIFY(IsWindow(Proxy));
        QTRY_VERIFY_WITH_TIMEOUT(Weak.expired(), 10000); QVERIFY(!locked(Directory)); QVERIFY(!IsWindow(Proxy));
    }
    void staleCancelAndStopAreIgnored()
    {
        QTemporaryDir Directory; auto Runtime = prepare(Directory); QVERIFY(!Runtime.sha256_.isEmpty());
        HwpProcessBackend Backend(Runtime, limits()); DocumentOperationTicket Ticket; Ticket.documentId_ = "live";
        QSignalSpy Loaded(&Backend, &HwpSdiBackend::Loaded), Cancelled(&Backend, &HwpSdiBackend::Cancelled);
        QSignalSpy Stopped(&Backend, &HwpSdiBackend::Stopped);
        Backend.Load(8, Ticket, {}); Backend.Cancel(9, Ticket); Backend.Stop(9);
        auto Stale = Ticket; Stale.documentId_ = "stale"; Backend.Cancel(8, Stale);
        QTRY_COMPARE_WITH_TIMEOUT(Stopped.count(), 1, 10000);
        QCOMPARE(Loaded.count(), 1); QCOMPARE(Cancelled.count(), 0);
    }
    void stopBeforeLoadIsQueued()
    {
        HwpProcessBackend Backend({}, limits()); QSignalSpy Stopped(&Backend, &HwpSdiBackend::Stopped);
        Backend.Stop(9); QCOMPARE(Stopped.count(), 0); Backend.Load(9, {}, {});
        QTRY_COMPARE(Stopped.count(), 1);
    }
    void directCompletionObserverCanDeleteBackend()
    {
        auto* Backend = new HwpProcessBackend({}, limits()); QPointer<HwpProcessBackend> Guard(Backend);
        connect(Backend, &HwpSdiBackend::Loaded, this, [Backend] { delete Backend; });
        Backend->Load(10, {}, {}); QTRY_VERIFY(Guard.isNull());
    }
};
int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv);
    const auto Args = App.arguments();
    if(Args.size() == 3 && Args[1] == "--shutdown-probe") {
        WebProcessRuntime Runtime; Runtime.root_ = Args[2];
        for(const auto Name : {"HyperEditorWeb.exe", "mode.txt"}) {
            QFile File(Runtime.root_ + '/' + Name); if(!File.open(QIODevice::ReadOnly)) return 2;
            Runtime.sha256_[Name] = QCryptographicHash::hash(File.readAll(), QCryptographicHash::Sha256).toHex();
        }
        auto Backend = std::make_unique<HwpProcessBackend>(Runtime, WebProcessLimits{4, 512ULL*1024*1024, 1500});
        auto Lease = std::make_shared<ProcessLease>(); std::weak_ptr<ProcessLease> Weak = Lease;
        SourceIdentity Source; Source.lease_ = Lease; Lease.reset();
        Backend->Load(99, {}, std::move(Source));
        QElapsedTimer Elapsed; Elapsed.start(); bool TimedOut = false, QuitIssued = false;
        QTimer Poll; Poll.setInterval(10);
        QObject::connect(&Poll, &QTimer::timeout, &App, [&] {
            QFile File(Runtime.root_ + "/mode.txt");
            if(!File.open(QIODevice::ReadWrite)) {
                Poll.stop();
                QTimer::singleShot(50, &App, [&] {
                    Backend.reset(); QuitIssued = true; QCoreApplication::quit();
                });
            }
        }); Poll.start();
        QTimer::singleShot(10000, &App, [&] { TimedOut = true; HwpProcessBackend::RequestShutdown(); App.quit(); });
        App.exec();
        return !TimedOut && QuitIssued && Elapsed.elapsed() >= 1000
            && HwpProcessBackend::IsDrained() && Weak.expired() ? 0 : 3;
    }
    HwpProcessTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstHwpProcessBackend.moc"
