#include "pluginProcess.hpp"
#include "pipeChannel.hpp"
#include "hostedWlxSession.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "memoryStorage.hpp"
#include "UniqueLibs/qtWlxSurface.hpp"
#include "UniqueLibs/qtViewerHost.hpp"
#include <QAction>
#include <QDialog>
#include <QMenu>
#include <QPlainTextEdit>
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
#include <atomic>
#include <cstring>
#include <thread>
#include <QVBoxLayout>

using namespace hc::plugins;
using namespace hc::storage;
namespace
{
    class TimedApplication final : public QApplication
    {
    public:
        using QApplication::QApplication;
        bool notify(QObject* Receiver, QEvent* Event) override
        {
            const auto Type = Event->type(); const QByteArray Class = Receiver->metaObject()->className();
            QElapsedTimer Timer; Timer.start(); const auto Result = QApplication::notify(Receiver, Event);
            if (Timer.elapsed() >= 100) qWarning() << "Slow Qt event" << Class << Type << "ms" << Timer.elapsed();
            return Result;
        }
    };
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
    void Activate(QWidget& Window)
    {
        Window.raise(); Window.activateWindow();
        const auto Foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const auto Thread = GetCurrentThreadId();
        // Match the product UI harness: establish actual foreground ownership
        // before testing focus replay. Qt's cached focus alone is insufficient
        // when another desktop application holds the interactive input queue.
        const bool Attached = Foreground && Foreground != Thread && AttachThreadInput(Thread, Foreground, TRUE);
        SetForegroundWindow(reinterpret_cast<HWND>(Window.winId()));
        if (Attached) AttachThreadInput(Thread, Foreground, FALSE);
    }
    std::shared_ptr<IContentSource> MemoryContent(const QByteArray& Bytes, const std::string& Name = "문서.txt")
    {
        auto Provider = std::make_shared<test::MemoryProvider>();
        std::vector<std::byte> Data(Bytes.size()); if (!Data.empty()) std::memcpy(Data.data(), Bytes.constData(), Bytes.size());
        const auto Id = Provider->Add(Provider->Root(), Name, std::move(Data));
        return std::make_shared<ProviderContentSource>(Provider, Provider->Stat(Id, {}).get().Value());
    }
    QStringList Leases(const QString& Base)
    { return QDir(Base + "/hc-materialized-v1").entryList({"hc-lease-v1-*"}, QDir::Dirs | QDir::NoDotAndDotDot); }
    class WaitingContent final : public IContentSource
    {
    public:
        std::atomic<bool> reading_ = false;
        EntrySnapshot Describe() const override { return source_->Describe(); }
        Task<Unit> Verify(OperationContext Context) override { return source_->Verify(Context); }
        Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context) override
        {
            reading_ = true;
            while (!Context.Check({"wlx-test"})) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return Ready<std::shared_ptr<IReadStream>>(*Context.Check({"wlx-test"}));
        }
    private:
        std::shared_ptr<IContentSource> source_ = MemoryContent("waiting");
    };
}
class HostedWlxTests : public QObject
{
    Q_OBJECT
private:
    QString host_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
    bool prepare(QTemporaryDir& Directory, PluginApproval& Approval, bool Sdk = false)
    {
        if (!Directory.isValid() || !QFile::copy(QCoreApplication::applicationDirPath() + (Sdk ? "/hc_wlx_sdk_fixture.wlx64" : "/hc_wlx_fixture.wlx64"), Directory.filePath("test.wlx64"))) return false;
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
    WlxViewerOptions options(const QTemporaryDir& Directory, PluginApproval Approval)
    {
        WlxViewerOptions Options; Options.executable_ = host_; Options.manifestPath_ = Directory.filePath("plugin.json");
        Options.binaryPath_ = Directory.filePath("test.wlx64"); Options.approval_ = std::move(Approval);
        Options.limits_.requestMs_ = 500; Options.limits_.heartbeatMs_ = 200; return Options;
    }
private slots:
    void sdkInitializationAndDetection_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto* Mode : {"normal", "empty", "maximum", "ansi", "unterminated", "hang-defaults", "crash-defaults", "hang-detect", "crash-detect"})
            QTest::newRow(Mode) << QString::fromLatin1(Mode);
    }
    void sdkInitializationAndDetection()
    {
        QFETCH(QString, Mode); QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval, true));
        QWidget Parent;
        const auto Ini = Directory.filePath("settings/lsplugin.ini"); QVERIFY(QDir().mkpath(QFileInfo(Ini).absolutePath()));
        { QFile File(Ini); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write("[fixture]\r\nvalue=123\r\nmode=" + Mode.toLatin1() + "\r\n") > 0); }
        HostLimits Limits; Limits.startupMs_ = 1000; Limits.requestMs_ = 500; Limits.heartbeatMs_ = 1000;
        QElapsedTimer Clock; Clock.start(); qint64 Last = 0, Gap = 0; QTimer Beat; Beat.setInterval(10);
        connect(&Beat, &QTimer::timeout, this, [&] { const auto Now = Clock.elapsed(); Gap = std::max(Gap, Now - Last); Last = Now; }); Beat.start();
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits, {}, Ini); });
        if (Mode.endsWith("defaults"))
        {
            QVERIFY(!Started.HasValue()); QVERIFY(Clock.elapsed() < 3000); QVERIFY(Gap < 200);
            // The package lock must be released after a failed startup, too.
            QFile Binary(Directory.filePath("test.wlx64")); QVERIFY(Binary.open(QIODevice::ReadWrite)); return;
        }
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        auto Probe = Worker([&] { return Host->Probe(); });
        if (Mode.endsWith("detect"))
        {
            QVERIFY(!Probe.HasValue()); Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().running_);
            QVERIFY(Clock.elapsed() < 3000); QVERIFY(Gap < 200); return;
        }
        if (Mode == "unterminated")
        { QVERIFY(!Probe.HasValue()); QCOMPARE(Probe.Failure().code_, std::string("host-wlx-detect-unterminated")); QVERIFY(Worker([&] { return Host->Ping(); }).HasValue()); return; }
        QVERIFY(Probe.HasValue()); QVERIFY(Probe.Value()["detectAvailable"].toBool());
        const auto Bytes = QByteArray::fromBase64(Probe.Value()["detectStringBytes"].toString().toLatin1());
        if (Mode == "normal") QCOMPARE(Bytes, QByteArray("EXT=\"TXT\""));
        else if (Mode == "maximum") QCOMPARE(Bytes, QByteArray(2047, 'X'));
        else if (Mode == "ansi") QCOMPARE(Bytes, QByteArray::fromHex("5b305d3d22e922"));
        else QVERIFY(Bytes.isEmpty());
        const auto Again = Worker([&] { return Host->Probe(); }); QVERIFY(Again.HasValue()); QCOMPARE(Again.Value(), Probe.Value());
        Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        const auto Request = request(Parent, Directory.filePath("content.txt"));
        auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, Request); });
        QVERIFY(Opened.HasValue()); QCOMPARE(Title(Window(Opened.Value(), "viewer")), QString("WLX SDK initialized"));
    }
    void sdkConfigurationPaths_data()
    {
        QTest::addColumn<QString>("Name"); QTest::addColumn<bool>("Valid");
        QTest::newRow("unicode") << QString::fromUtf8("설정_😀/lsplugin.ini") << true;
        QTest::newRow("overlong-filename") << QString(270, 'a') + ".ini" << false;
    }
    void sdkConfigurationPaths()
    {
        QFETCH(QString, Name); QFETCH(bool, Valid);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval, true));
        const auto Ini = Directory.filePath(Name);
        if (Valid)
        {
            QVERIFY(QDir().mkpath(QFileInfo(Ini).absolutePath()));
            QFile File(Ini); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write("[fixture]\r\nvalue=123\r\n") > 0);
        }
        auto Started = Worker([&] { return PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, {}, {}, Ini); });
        if (!Started.HasValue())
        {
            // A volume without short names may not represent this directory in
            // the active ANSI code page. It must fail explicitly, never redirect.
            QCOMPARE(Started.Failure().code_, std::string("host-wlx-config-encoding")); return;
        }
        QVERIFY(Valid); auto Host = std::move(Started.Value());
        const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        auto Probe = Worker([&] { return Host->Probe(); }); QVERIFY(Probe.HasValue());
        QCOMPARE(QByteArray::fromBase64(Probe.Value()["detectStringBytes"].toString().toLatin1()), QByteArray("EXT=\"TXT\""));
    }
    void sdkSessionConfiguration()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval, true));
        auto Options = options(Directory, Approval); Options.defaultIniPath_ = Directory.filePath("lsplugin.ini");
        { QFile File(Options.defaultIniPath_); QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.write("[fixture]\r\nvalue=123\r\n") > 0); }
        QWidget Main; Main.resize(320, 200); Main.show();
        WlxViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Opened = Service.Open(Options, MemoryContent("sdk"), QString::number(Main.winId(), 16), 320, 200); QVERIFY(Opened.HasValue());
        auto Session = Opened.Value(); QTRY_COMPARE_WITH_TIMEOUT(Session->Snapshot().status_, hc::viewer::ViewerStatus::Ready, 4000);
        Session->Close(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void viewerHostSelectionAndClose()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Main; Main.resize(640, 360); Main.show();
        bool Approved = true;
        QtViewerHost Host(&Main, [&] { return MakeNativeMaterializer(Directory.path().toStdString()); }, [&] {
            auto Options = options(Directory, Approval); Options.approval_.approved_ = Approved;
            return std::vector<QtViewerSurface::WlxChoice>{{"test", "WLX fixture", Options}};
        });
        const auto Drain = qScopeGuard([&] { Host.CloseAll(); while (!Host.IsIdle()) QTest::qWait(10); });
        QPointer<QDialog> Full = Host.OpenFull(MemoryContent("content", "host.txt"));
        auto* Surface = Full->findChild<QtViewerSurface*>(); QVERIFY(Surface);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        auto* Menu = Surface->findChild<QMenu*>("hc.viewer.plugins"); QVERIFY(Menu);
        QVERIFY(QMetaObject::invokeMethod(Menu, "aboutToShow", Qt::DirectConnection));
        auto* Choice = Menu->findChild<QAction*>("hc.viewer.plugin.test"); QVERIFY(Choice); Choice->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QVERIFY(Surface->findChild<QtWlxSurface*>()->IsReady()); QCOMPARE(Surface->Snapshot().source_.name_, std::string("host.txt"));
        QVERIFY(!Surface->findChild<QAction*>("hc.viewer.find")->isEnabled());
        QPointer<QWidget> Anchor = Surface->findChild<QWidget*>("hc.wlx.native-parent"); QVERIFY(Anchor);
        Surface->findChild<QAction*>("hc.viewer.fallback")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QTRY_VERIFY_WITH_TIMEOUT(Anchor.isNull(), 4000);
        QCOMPARE(Surface->findChild<QPlainTextEdit*>()->toPlainText(), QString("content"));
        QSignalSpy Failed(Surface, &QtViewerSurface::Failed);
        // A menu built under approved settings must re-check a later revocation.
        Approved = false; Choice->trigger(); QTRY_COMPARE_WITH_TIMEOUT(Failed.size(), 1, 5000); QVERIFY(!Surface->IsReady());
        QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty());
        Full->close(); QTRY_VERIFY(Full.isNull());
        Approved = true;
        Full = Host.OpenFull(MemoryContent("HC-WLX-FAULT:hang-close", "close.txt"));
        Surface = Full->findChild<QtViewerSurface*>(); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        Menu = Surface->findChild<QMenu*>("hc.viewer.plugins"); QVERIFY(QMetaObject::invokeMethod(Menu, "aboutToShow", Qt::DirectConnection));
        Menu->findChild<QAction*>("hc.viewer.plugin.test")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QVERIFY(Surface->findChild<QtWlxSurface*>()->IsReady());
        QElapsedTimer Closing; Closing.start(); Main.close(); QVERIFY(Closing.elapsed() < 200);
        QVERIFY(Main.isVisible()); QTRY_VERIFY_WITH_TIMEOUT(!Main.isVisible(), 4000); QVERIFY(Host.IsIdle());
        QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void qtSurfaceReplacement()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Main; Main.resize(640, 360); auto* Layout = new QVBoxLayout(&Main);
        QtWlxController Controller(&Main, [&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        auto* Surface = new QtWlxSurface(&Controller, &Main); Layout->addWidget(Surface); Main.show(); QVERIFY(QTest::qWaitForWindowExposed(&Main));
        Activate(Main); QTRY_COMPARE(GetForegroundWindow(), reinterpret_cast<HWND>(Main.winId())); Surface->setFocus();
        const auto Drain = qScopeGuard([&] { Surface->Close(); while (!Controller.IsIdle()) QTest::qWait(10); });
        QSignalSpy Ready(Surface, &QtWlxSurface::Ready); QSignalSpy Failed(Surface, &QtWlxSurface::Failed);
        Surface->Open(options(Directory, Approval), MemoryContent("first", "first.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QCOMPARE(Ready.size(), 1); QCOMPARE(Failed.size(), 0);
        const auto Focused = [&] {
            const auto Container = GetWindow(reinterpret_cast<HWND>(Surface->findChild<QWidget*>("hc.wlx.native-parent")->winId()), GW_CHILD);
            const auto Child = GetWindow(Container, GW_CHILD); GUITHREADINFO Info{sizeof(GUITHREADINFO)};
            return GetGUIThreadInfo(GetWindowThreadProcessId(Child, nullptr), &Info) && (Info.hwndFocus == Child || IsChild(Child, Info.hwndFocus));
        };
        QTRY_VERIFY_WITH_TIMEOUT(Focused(), 3000);
        QPointer<QWidget> Native = Surface->findChild<QWidget*>("hc.wlx.native-parent"); QVERIFY(Native); const auto Before = Native->winId();
        auto Slow = std::make_shared<WaitingContent>(); Surface->Open(options(Directory, Approval), Slow);
        if (Native) QCOMPARE(Native->winId(), Before);
        QTRY_VERIFY_WITH_TIMEOUT(Slow->reading_.load(), 3000);
        Surface->Open(options(Directory, Approval), MemoryContent("latest", "latest.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QCOMPARE(Ready.size(), 2); QCOMPARE(Failed.size(), 0);
        QCOMPARE(Surface->Snapshot().source_.name_, std::string("latest.txt"));
        Main.resize(380, 260); QTest::qWait(100);
        const auto Screenshots = qEnvironmentVariable("HC_VIEWER_SCREENSHOTS");
        if (!Screenshots.isEmpty()) { QDir().mkpath(Screenshots); QVERIFY(Main.screen()->grabWindow(Main.winId()).save(Screenshots + "/wlx-surface.png")); }
        Surface->Close(); QTRY_VERIFY_WITH_TIMEOUT(Controller.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void qtSurfaceDestruction_data()
    {
        QTest::addColumn<QString>("Name");
        for (const auto Name : {"ordinary", "hang-message", "hang-close"}) QTest::newRow(Name) << QString(Name);
    }
    void qtSurfaceDestruction()
    {
        QFETCH(QString, Name); QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Main; Main.resize(640, 360); auto* Layout = new QVBoxLayout(&Main);
        QtWlxController Controller(&Main, [&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        auto* Surface = new QtWlxSurface(&Controller, &Main); Layout->addWidget(Surface); Main.show(); QVERIFY(QTest::qWaitForWindowExposed(&Main));
        const auto Drain = qScopeGuard([&] { Controller.CloseAll(); while (!Controller.IsIdle()) QTest::qWait(10); });
        Surface->Open(options(Directory, Approval), MemoryContent("HC-WLX-FAULT:" + Name.toUtf8()));
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000);
        QPointer<QWidget> Native = Surface->findChild<QWidget*>("hc.wlx.native-parent"); QVERIFY(Native); const auto Before = Native->winId();
        if (Name == "hang-message") QTest::qWait(120);
        QElapsedTimer Deletion; Deletion.start(); delete Surface;
        qInfo() << "Qt surface deletion" << Name << "ms" << Deletion.elapsed(); QVERIFY(Deletion.elapsed() < 200);
        if (Native) { QCOMPARE(Native->winId(), Before); QVERIFY(!Native->isVisible()); }
        QTRY_VERIFY_WITH_TIMEOUT(Controller.IsIdle(), 4000); QTRY_VERIFY_WITH_TIMEOUT(Native.isNull(), 1000);
        QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void sessionMaterializationAndCommands()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(640, 360); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        const auto GuiThread = std::this_thread::get_id(); std::atomic<bool> FactoryOnWorker = false;
        WlxViewerService Service([&] { FactoryOnWorker = std::this_thread::get_id() != GuiThread; return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Source = MemoryContent("materialized Unicode content"); const auto Expected = Source->Describe();
        QElapsedTimer Start; Start.start();
        auto Opened = Service.Open(options(Directory, Approval), Source, QString::number(Parent.winId(), 16), 640, 360);
        QVERIFY(Start.elapsed() < 100); QVERIFY(Opened.HasValue()); auto Session = Opened.Value();
        QTRY_VERIFY_WITH_TIMEOUT(Session->Snapshot().status_ != hc::viewer::ViewerStatus::Loading, 5000);
        QVERIFY2(Session->Snapshot().status_ == hc::viewer::ViewerStatus::Ready,
            Session->Snapshot().error_ ? Session->Snapshot().error_->code_.c_str() : "not ready");
        QVERIFY(FactoryOnWorker); QCOMPARE(Session->Snapshot().source_.id_, Expected.id_); QCOMPARE(Session->Snapshot().bytesRead_, Expected.size_);
        const auto LeaseNames = Leases(Directory.path()); QCOMPARE(LeaseNames.size(), 1);
        const auto ContentPath = Directory.path() + "/hc-materialized-v1/" + LeaseNames.front() + "/content.txt";
        QFile Content(ContentPath); QVERIFY(Content.open(QIODevice::ReadOnly)); QCOMPARE(Content.readAll(), QByteArray("materialized Unicode content")); Content.close();
        const auto Details = Session->Details(); const auto Child = reinterpret_cast<HWND>(static_cast<quintptr>(Details.child_.toULongLong(nullptr, 16)));
        for (unsigned Size = 100; Size <= 600; ++Size) QVERIFY(Session->Resize(Size, Size / 2).HasValue());
        QTRY_VERIFY_WITH_TIMEOUT(([&] { RECT Rect{}; return GetClientRect(Child, &Rect) && Rect.right == 600 && Rect.bottom == 300; })(), 3000);
        QVERIFY(Session->Focus().HasValue()); auto Command = Session->SendCommand(3, 0); QVERIFY(Command.HasValue());
        QTRY_COMPARE_WITH_TIMEOUT(Session->Details().commandSequence_, Command.Value(), 3000); QVERIFY(!Session->Details().commandError_);
        QElapsedTimer Closing; Closing.start(); Session->Close(); QVERIFY(Closing.elapsed() < 100);
        QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000); QVERIFY(Session->Details().idle_); QVERIFY(!Session->Details().host_.running_);
        QVERIFY(!IsWindow(Child)); QVERIFY(Leases(Directory.path()).isEmpty()); QVERIFY(!QFileInfo::exists(ContentPath));
        QCOMPARE(Session->Snapshot().status_, hc::viewer::ViewerStatus::Closed); QVERIFY(!Session->Resize(1, 1).HasValue()); Service.Collect();
    }
    void sessionReadCancellationAndBudget()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval)); QWidget Parent;
        WlxViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); }, 1);
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Source = std::make_shared<WaitingContent>();
        auto Opened = Service.Open(options(Directory, Approval), Source, QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Opened.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Source->reading_.load(), 3000);
        auto Second = Service.Open(options(Directory, Approval), MemoryContent("second"), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(!Second.HasValue()); QCOMPARE(Second.Failure().category_, ErrorCategory::BudgetExceeded);
        Opened.Value()->Cancel(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000);
        QCOMPARE(Opened.Value()->Snapshot().status_, hc::viewer::ViewerStatus::Cancelled);
        QVERIFY(Leases(Directory.path()).isEmpty()); QCOMPARE(Opened.Value()->Details().host_.processId_, 0u);
        auto Third = Service.Open(options(Directory, Approval), MemoryContent("third"), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Third.HasValue()); QTRY_COMPARE_WITH_TIMEOUT(Third.Value()->Snapshot().status_, hc::viewer::ViewerStatus::Ready, 5000);
        Third.Value().reset(); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000); QVERIFY(Leases(Directory.path()).isEmpty());
    }
    void sessionAdmissionBeforeExtraction()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval)); QWidget Parent; std::atomic<unsigned> Factories = 0;
        WlxViewerService Service([&] { ++Factories; return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        auto Options = options(Directory, Approval); Options.binaryPath_ = Directory.filePath("wrong.wlx64");
        auto Opened = Service.Open(Options, MemoryContent("secret"), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Opened.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000);
        QCOMPARE(Opened.Value()->Snapshot().status_, hc::viewer::ViewerStatus::Failed);
        QCOMPARE(Opened.Value()->Snapshot().error_->code_, std::string("plugin-configured-binary-mismatch")); QCOMPARE(Factories.load(), 0u);
        Options = options(Directory, Approval); Options.approval_.granted_.erase(Permission::UserInterface);
        auto Denied = Service.Open(Options, MemoryContent("secret"), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Denied.HasValue()); QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 4000);
        QCOMPARE(Denied.Value()->Snapshot().error_->code_, std::string("plugin-permission-denied")); QCOMPARE(Factories.load(), 0u);
    }
    void sessionFaultCleanup_data()
    {
        QTest::addColumn<QString>("Name");
        for (const auto Name : {"hang-message", "crash-command", "hang-close", "hang-load"}) QTest::newRow(Name) << QString(Name);
    }
    void sessionFaultCleanup()
    {
        QFETCH(QString, Name); QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Approval));
        QWidget Parent; Parent.resize(320, 200); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        WlxViewerService Service([&] { return MakeNativeMaterializer(Directory.path().toStdString()); });
        const auto Drain = qScopeGuard([&] { Service.CancelAll(); while (!Service.IsIdle()) QTest::qWait(10); });
        QElapsedTimer Clock; Clock.start(); qint64 Last = 0, Gap = 0; unsigned Ticks = 0;
        QTimer Beat; Beat.setInterval(10); connect(&Beat, &QTimer::timeout, this, [&] { const auto Now = Clock.elapsed(); Gap = std::max(Gap, Now - Last); Last = Now; ++Ticks; }); Beat.start();
        auto Opened = Service.Open(options(Directory, Approval), MemoryContent("HC-WLX-FAULT:" + Name.toUtf8()), QString::number(Parent.winId(), 16), 320, 200);
        QVERIFY(Opened.HasValue()); auto Session = Opened.Value();
        if (Name == "hang-load")
        { QTRY_VERIFY_WITH_TIMEOUT(Session->Details().host_.processId_ != 0, 3000); Session->Cancel(); }
        else
        {
            QTRY_COMPARE_WITH_TIMEOUT(Session->Snapshot().status_, hc::viewer::ViewerStatus::Ready, 3000);
            if (Name == "crash-command") QVERIFY(Session->SendCommand(3, 0).HasValue());
            if (Name == "hang-close") Session->Close();
        }
        QTRY_VERIFY_WITH_TIMEOUT(Service.IsIdle(), 5000); QVERIFY(Leases(Directory.path()).isEmpty()); QVERIFY(!Session->Details().host_.running_);
        const auto Expected = Name == "hang-load" ? hc::viewer::ViewerStatus::Cancelled : Name == "hang-close" ? hc::viewer::ViewerStatus::Closed : hc::viewer::ViewerStatus::Failed;
        QCOMPARE(Session->Snapshot().status_, Expected); QVERIFY(Ticks > 2);
        qInfo() << "Session GUI maximum gap ms" << Gap; QVERIFY(Gap < 200);
    }
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
        QVERIFY(!Probe.Value()["detectAvailable"].toBool()); QVERIFY(Probe.Value()["detectStringBytes"].toString().isEmpty());
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
    TimedApplication App(Argc, Argv);
    if (App.arguments().contains("--probe-dpr")) { std::cout << App.primaryScreen()->devicePixelRatio() << std::endl; return 0; }
    HostedWlxTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstHostedWlx.moc"
