#include "pluginProcess.hpp"
#include "pipeChannel.hpp"
#include <QtTest>
#include <QApplication>
#include <QWidget>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QScreen>
#include <QScopeGuard>
#include <QTimer>
#include <future>
#include <iostream>

using namespace hc::plugins;
using namespace hc::storage;
namespace
{
    template<class F> auto Worker(F&& Function)
    {
        auto Future = std::async(std::launch::async, std::forward<F>(Function));
        while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) QTest::qWait(10);
        return Future.get();
    }
    HWND Window(const QJsonObject& Reply, const char* Key)
    { return reinterpret_cast<HWND>(static_cast<quintptr>(Reply[Key].toString().toULongLong(nullptr, 16))); }
    QString Title(HWND Window)
    { wchar_t Buffer[256]{}; GetWindowTextW(Window, Buffer, 256); return QString::fromWCharArray(Buffer); }
}
class HostedWlxTests : public QObject
{
    Q_OBJECT
private:
    QString host_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
    bool prepare(QTemporaryDir& Directory, PluginApproval& Approval)
    {
        if (!Directory.isValid() || !QFile::copy(QCoreApplication::applicationDirPath() + "/hc_wlx_fixture.wlx64", Directory.filePath("test.wlx64"))) return false;
        QFile Binary(Directory.filePath("test.wlx64")); if (!Binary.open(QIODevice::ReadOnly)) return false;
        PluginManifest Value; Value.id_ = "hc.test.wlx"; Value.version_ = "1.0"; Value.binary_ = "test.wlx64";
        Value.format_ = PluginFormat::Wlx; Value.sha256_ = QCryptographicHash::hash(Binary.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
        Value.extensions_ = {Extension::Viewer}; Value.permissions_ = {Permission::ReadContent, Permission::UserInterface};
        Approval.reviewed_ = Value; Approval.approved_ = true; Approval.granted_ = Value.permissions_;
        QFile Manifest(Directory.filePath("plugin.json")); return Manifest.open(QIODevice::WriteOnly) && Manifest.write(EncodeManifest(Value)) > 0;
    }
    QJsonObject request(QWidget& Parent, const QString& Path)
    {
        QFile File(Path); if (File.open(QIODevice::WriteOnly)) File.write("Unicode WLX content\n");
        return {{"parent", QString::number(Parent.winId(), 16)}, {"file", Path},
            {"width", qRound(Parent.width() * Parent.devicePixelRatioF())}, {"height", qRound(Parent.height() * Parent.devicePixelRatioF())}, {"flags", 0}};
    }
private slots:
    void admission()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        Approval.granted_.erase(Permission::UserInterface);
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval); });
        QVERIFY(!Started.HasValue()); QCOMPARE(Started.Failure().code_, std::string("plugin-permission-denied"));
    }
    void nativeLifecycle()
    {
        QTemporaryDir Directory(QDir::tempPath() + "/WLX 한글 XXXXXX"); PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(640, 360); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        const auto Expected = qEnvironmentVariable("HC_VIEWER_EXPECT_DPR");
        if (!Expected.isEmpty()) QCOMPARE(Parent.devicePixelRatioF(), Expected.toDouble());
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval); });
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        auto Probe = Worker([&] { return Host->Probe(); }); QVERIFY(Probe.HasValue()); QCOMPARE(Probe.Value()["format"].toString(), QString("wlx"));
        const auto Request = request(Parent, Directory.filePath("문서.txt"));
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : Opened.Failure().code_.c_str());
        const auto Container = Window(Opened.Value(), "window"), Viewer = Window(Opened.Value(), "viewer");
        QCOMPARE(GetParent(Container), reinterpret_cast<HWND>(Parent.winId())); QVERIFY(IsChild(Container, Viewer));
        DWORD Pid = 0; GetWindowThreadProcessId(Viewer, &Pid); QCOMPARE(Pid, Host->Snapshot().processId_); QVERIFY(Pid != GetCurrentProcessId());
        QTRY_VERIFY_WITH_TIMEOUT(Title(Viewer).startsWith("WLX fixture ready ") && Title(Viewer).section(' ', -1).toUInt() >= 3, 3000);
        RECT Rect{}; QVERIFY(GetClientRect(Viewer, &Rect)); QCOMPARE(Rect.right, Request["width"].toInt()); QCOMPARE(Rect.bottom, Request["height"].toInt());
        auto Resized = Worker([&] { return Host->ViewerCall(WireKind::ViewerResize, {{"width", 333}, {"height", 177}}); });
        QVERIFY(Resized.HasValue()); QVERIFY(GetClientRect(Viewer, &Rect)); QCOMPARE(Rect.right, 333L); QCOMPARE(Rect.bottom, 177L);
        Parent.activateWindow(); Parent.setFocus();
        auto Focused = Worker([&] { return Host->ViewerCall(WireKind::ViewerFocus, {}); }); QVERIFY(Focused.HasValue()); QVERIFY(Focused.Value()["focused"].toBool());
        auto Command = Worker([&] { return Host->ViewerCall(WireKind::ViewerCommand, {{"command", 3}, {"parameter", 0}}); });
        QVERIFY(Command.HasValue()); QCOMPARE(Command.Value()["result"].toInt(-1), 0);
        auto Invalid = Worker([&] { return Host->ViewerCall(WireKind::ViewerResize, {{"width", -1}, {"height", 1}}); }); QVERIFY(!Invalid.HasValue());
        QVERIFY(Worker([&] { return Host->Ping(); }).HasValue());
        QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue()); QVERIFY(!IsWindow(Viewer)); QVERIFY(!IsWindow(Container));
        auto Again = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); }); QVERIFY(Again.HasValue());
        Worker([&] { Host->Close(); }); QVERIFY(!IsWindow(Window(Again.Value(), "viewer")));
        QCOMPARE(Host->Snapshot().exitCode_.value(), 0u); QVERIFY(!Host->Snapshot().forcedTermination_);
    }
    void foreignParentDenied()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval); });
        QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value());
        const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        auto Denied = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen,
            {{"parent", QString::number(reinterpret_cast<quintptr>(GetDesktopWindow()), 16)}}); });
        QVERIFY(!Denied.HasValue()); QCOMPARE(Denied.Failure().code_, std::string("host-viewer-parent"));
        QVERIFY(Worker([&] { return Host->Ping(); }).HasValue());
    }
    void cancelOpen()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval); });
        QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value());
        const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Request = request(Parent, Directory.filePath("hang-load.txt"));
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        QTimer Canceller; Canceller.setSingleShot(true); connect(&Canceller, &QTimer::timeout, this, [&] { Stop.request_stop(); }); Canceller.start(100);
        QElapsedTimer Elapsed; Elapsed.start();
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request, Context); });
        QVERIFY(!Opened.HasValue()); QCOMPARE(Opened.Failure().category_, ErrorCategory::Cancelled); QVERIFY(Elapsed.elapsed() < 1500);
        Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().running_);
    }
    void faults_data()
    {
        QTest::addColumn<QString>("Name");
        for (const auto Name : {"reject", "hang-load", "hang-message", "crash-command", "hang-close"}) QTest::newRow(Name) << QString(Name);
    }
    void faults()
    {
        QFETCH(QString, Name);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        HostLimits Limits; Limits.requestMs_ = 500; Limits.heartbeatMs_ = 200;
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits); });
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        QElapsedTimer Clock; Clock.start(); qint64 LastTick = 0, MaximumGap = 0; unsigned Ticks = 0;
        QTimer Timer; Timer.setInterval(10);
        connect(&Timer, &QTimer::timeout, this, [&] { const auto Now = Clock.elapsed(); MaximumGap = std::max(MaximumGap, Now - LastTick); LastTick = Now; ++Ticks; });
        Timer.start();
        const auto Request = request(Parent, Directory.filePath(Name + ".txt"));
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        if (Name == "reject")
        {
            QVERIFY(!Opened.HasValue()); QVERIFY(Worker([&] { return Host->Ping(); }).HasValue());
            auto Retry = Request; Retry["file"] = Directory.filePath("ok.txt"); request(Parent, Retry["file"].toString());
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Retry); }).HasValue()); return;
        }
        if (Name == "hang-load") QVERIFY(!Opened.HasValue());
        else
        {
            QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : Opened.Failure().code_.c_str());
            if (Name == "crash-command") QVERIFY(!Worker([&] { return Host->ViewerCall(WireKind::ViewerCommand, {{"command", 3}, {"parameter", 0}}); }).HasValue());
            if (Name == "hang-close") QVERIFY(!Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue());
        }
        QTRY_VERIFY_WITH_TIMEOUT(!Worker([&] { return Host->Snapshot(); }).running_, 4000);
        if (Opened.HasValue()) QVERIFY(!IsWindow(Window(Opened.Value(), "viewer")));
        QVERIFY(Host->Snapshot().failure_.has_value());
        qInfo() << "GUI heartbeat during" << Name << "ticks" << Ticks << "maximum gap ms" << MaximumGap;
        QVERIFY(Ticks > 0); QVERIFY2(MaximumGap < 200, "Plugin failure stalled the caller GUI event loop");
    }
};
int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv);
    if (App.arguments().contains("--probe-dpr")) { std::cout << App.primaryScreen()->devicePixelRatio() << std::endl; return 0; }
    HostedWlxTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstHostedWlx.moc"
