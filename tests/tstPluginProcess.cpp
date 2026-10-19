#include <QtTest>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QProcess>
#include "pluginProcess.hpp"
#include "pipeChannel.hpp"
#include "archiveCodec.hpp"
#include "commonLibs/win/restrictedEnvironment.hpp"
#include <TlHelp32.h>
#include <thread>

using namespace hc::plugins;
using namespace hc::storage;
class PluginProcessTests : public QObject
{
    Q_OBJECT
private:
    QString host_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
    bool prepare(QTemporaryDir& Directory, const QString& Fixture, PluginApproval& Approval)
    {
        if (!Directory.isValid()) return false;
        const auto Target = Directory.filePath("test.wcx64");
        if (!QFile::copy(QCoreApplication::applicationDirPath() + "/" + Fixture, Target)) return false;
        QFile Binary(Target); if (!Binary.open(QIODevice::ReadOnly)) return false;
        PluginManifest Value; Value.id_ = "hc.test.host"; Value.version_ = "1.0"; Value.binary_ = "test.wcx64";
        Value.sha256_ = QCryptographicHash::hash(Binary.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
        Value.extensions_ = {Extension::ArchiveRead}; Value.permissions_ = {Permission::ReadContent};
        Approval.reviewed_ = Value; Approval.approved_ = true; Approval.granted_ = Value.permissions_;
        QFile File(Directory.filePath("plugin.json")); return File.open(QIODevice::WriteOnly) && File.write(EncodeManifest(Value)) > 0;
    }
private slots:
    void restrictedEnvironmentKeepsSystemLocations()
    {
        // The host and children that inherit its block (HyperViewer workspace) need SystemDrive,
        // otherwise Shell creates a literal "%SystemDrive%" folder in their working directory.
        qputenv("HC_TEST_SECRET", "must-not-inherit");
        const auto Variables = hc::win::RestrictedEnvironment("C:/Windows/System32");
        qunsetenv("HC_TEST_SECRET");
        QVERIFY(Variables.contains("PATH=C:/Windows/System32"));
        for (const auto* Name : {"SystemRoot", "SystemDrive", "ProgramData"}) {
            const auto Value = qEnvironmentVariable(Name); QVERIFY2(!Value.isEmpty(), Name);
            QVERIFY2(Variables.contains(QString(Name) + '=' + Value), Name);
        }
        for (const auto& Variable : Variables) QVERIFY2(!Variable.startsWith("HC_TEST_SECRET="), qPrintable(Variable));
        QVERIFY(Variables.filter(QRegularExpression("^(USERPROFILE|APPDATA|LOCALAPPDATA)=", QRegularExpression::CaseInsensitiveOption)).isEmpty());
    }
    void sessionPolicyRoundTrip()
    {
        OperationContext Context; Context.maxReadBytes_ = 64ULL * 1024 * 1024;
        auto Decoded = DecodeContextV2(EncodeContextV2(Context)); QVERIFY(Decoded.HasValue());
        QCOMPARE(Decoded.Value().EnumerationLimit(), SIZE_MAX);
        QCOMPARE(Decoded.Value().deadline_, std::chrono::steady_clock::time_point::max());
        QCOMPARE(Decoded.Value().maxReadBytes_, std::size_t(WirePayloadLimit));
        Context.workload_.intent_ = WorkloadIntent::Automatic;
        Context.workload_.automaticEntries_ = 17; Context.workload_.automaticFileBytes_ = 5ULL * 1024 * 1024 * 1024;
        Decoded = DecodeContextV2(EncodeContextV2(Context)); QVERIFY(Decoded.HasValue());
        QCOMPARE(Decoded.Value().EnumerationLimit(), 17u); QCOMPARE(Decoded.Value().workload_.automaticFileBytes_, Context.workload_.automaticFileBytes_);
        auto Invalid = EncodeContextV2(Context); Invalid["milliseconds"] = QString::number(UINT64_MAX);
        QVERIFY(!DecodeContextV2(Invalid).HasValue());
        Invalid = EncodeContextV2(Context); Invalid["entries"] = "-1"; QVERIFY(!DecodeContextV2(Invalid).HasValue());
    }
    void framing()
    {
        const WireHeader Value{{1, 0}, WireKind::Probe, 0x0102030405060708, WirePayloadLimit};
        auto Encoded = EncodeHeader(Value); QVERIFY(Encoded.HasValue()); QCOMPARE(Encoded.Value().size(), WireHeaderBytes);
        QCOMPARE(std::to_integer<int>(Encoded.Value()[12]), 8); QCOMPARE(std::to_integer<int>(Encoded.Value()[19]), 1);
        const auto Decoded = DecodeHeader(Encoded.Value()); QVERIFY(Decoded.HasValue()); QCOMPARE(Decoded.Value().sequence_, Value.sequence_);
        for (unsigned Size = 0; Size < WireHeaderBytes; ++Size) QVERIFY(!DecodeHeader(std::span(Encoded.Value()).first(Size)).HasValue());
        auto Broken = Encoded.Value(); Broken[4] = std::byte{3}; QVERIFY(!DecodeHeader(Broken).HasValue());
        Broken = Encoded.Value(); Broken[10] = std::byte{1}; QVERIFY(!DecodeHeader(Broken).HasValue());
        Broken = Encoded.Value(); Broken[23] = std::byte{0xff}; QVERIFY(!DecodeHeader(Broken).HasValue());
        QVERIFY(!EncodeHeader({{1, 0}, WireKind::Ping, 0, 0}).HasValue()); QVERIFY(!EncodeHeader({{1, 0}, WireKind(99), 1, 0}).HasValue());
        QVERIFY(!EncodeHeader({{1, 0}, WireKind::SessionInvoke, 1, 0}).HasValue());
        auto V2 = EncodeHeader({{2, 0}, WireKind::SessionInvoke, UINT64_MAX, 0}); QVERIFY(V2.HasValue());
        QCOMPARE(DecodeHeader(V2.Value()).Value().sequence_, UINT64_MAX);
    }
    void actualHostAndLease()
    {
        QTemporaryDir Directory(QDir::tempPath() + "/호스트 검증 XXXXXX"); PluginApproval Approval; QVERIFY(prepare(Directory, "hc_wcx_fixture.wcx64", Approval));
        HostLimits Limits; Limits.heartbeatMs_ = 100; Limits.memoryBytes_ = 0;
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits);
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value()); QVERIFY(Host->Snapshot().running_); QVERIFY(Host->Snapshot().processId_ != GetCurrentProcessId());
        auto Ping = Host->Ping(); QVERIFY(Ping.HasValue()); QVERIFY(Ping.Value()["inJob"].toBool()); QVERIFY(Ping.Value()["adminDenied"].toBool()); QVERIFY(Ping.Value()["privilegesRemoved"].toBool());
        QVERIFY(Ping.Value()["jobLimitsKnown"].toBool()); const auto Flags = Ping.Value()["jobLimitFlags"].toInteger();
        QVERIFY(!(Flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY)); QVERIFY(!(Flags & JOB_OBJECT_LIMIT_JOB_MEMORY));
        QVERIFY(Flags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE); QVERIFY(Flags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS);
        auto Probe = Host->Probe(); QVERIFY(Probe.HasValue()); QCOMPARE(Probe.Value()["format"].toString(), QString("wcx")); QVERIFY(Probe.Value()["unicode"].toBool());
        QCOMPARE(Probe.Value()["processId"].toInteger(), qint64(Host->Snapshot().processId_));
        QFile Mutate(Directory.filePath("test.wcx64")); QVERIFY(!Mutate.open(QIODevice::WriteOnly));
        QTest::qWait(350); QVERIFY(Host->Snapshot().running_); QVERIFY(Host->Ping().HasValue());
        Host->Close(); QVERIFY(!Host->Snapshot().running_); QCOMPARE(Host->Snapshot().exitCode_.value(), 0u); QVERIFY(!Host->Snapshot().forcedTermination_);
        QVERIFY(Mutate.open(QIODevice::WriteOnly)); Mutate.close(); QVERIFY(!Host->Ping().HasValue());
    }
    void configurationLifecycle_data()
    {
        QTest::addColumn<bool>("Dopus"); QTest::newRow("wcx-modal") << false; QTest::newRow("dopus-modeless") << true;
    }
    void configurationLifecycle()
    {
        QFETCH(bool, Dopus); QTemporaryDir Directory; PluginApproval Approval;
        QVERIFY(prepare(Directory, Dopus ? "hc_dopus_fixture.dll" : "hc_wcx_fixture.wcx64", Approval));
        auto& M = Approval.reviewed_; M.format_ = Dopus ? PluginFormat::DopusVfs : PluginFormat::Wcx;
        M.extensions_ = Dopus ? std::set<Extension>{Extension::StorageRead} : std::set<Extension>{Extension::ArchiveRead};
        const auto Save = [&] { QFile F(Directory.filePath("plugin.json")); return F.open(QIODevice::WriteOnly | QIODevice::Truncate) && F.write(EncodeManifest(M)) > 0; };
        QVERIFY(Save()); HostLimits Limits; Limits.purpose_ = HostPurpose::Configuration; Limits.requestMs_ = 100;
        auto Denied = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits); QVERIFY(!Denied.HasValue()); QCOMPARE(Denied.Failure().code_, std::string("host-configuration-ui-not-approved"));
        M.permissions_.insert(Permission::UserInterface); Approval.granted_ = M.permissions_; QVERIFY(Save());
        const auto Owner = CreateWindowExW(0, L"STATIC", L"Archive settings owner", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr); QVERIFY(Owner);
        const auto Cleanup = qScopeGuard([&] { DestroyWindow(Owner); });
        QElapsedTimer Timer; Timer.start();
        auto Future = std::async(std::launch::async, [&]() -> Result<QJsonObject> {
            OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits, Context); if (!Started.HasValue()) return Started.Failure();
            auto Value = Started.Value()->Configure(uint64_t(Owner), Context); Started.Value()->Close();
            if (Started.Value()->Snapshot().forcedTermination_) return Error{ErrorCategory::Internal, "configuration-forced-termination"}; return Value;
        });
        while (Future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) { MSG Message; while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&Message); DispatchMessageW(&Message); } QCoreApplication::processEvents(); }
        auto Configured = Future.get(); QVERIFY2(Configured.HasValue(), Configured.HasValue() ? "" : Configured.Failure().code_.c_str());
        QVERIFY(Timer.elapsed() >= 500); QVERIFY(Configured.Value()["configured"].toBool()); QCOMPARE(Configured.Value()["reinitialize"].toBool(), Dopus);
    }
    void noApprovalNoExecution()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_wcx_fixture.wcx64", Approval));
        Approval.approved_ = false;
        QCOMPARE(PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval).Failure().code_, std::string("plugin-review-required"));
    }
    void parentDeathReapsHost()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_wcx_fixture.wcx64", Approval));
        QProcess Owner; Owner.start(QCoreApplication::applicationDirPath() + "/hc_plugin_owner_fixture.exe", {host_, Directory.filePath("plugin.json")});
        QVERIFY(Owner.waitForStarted(5000)); QVERIFY2(Owner.waitForReadyRead(5000), Owner.readAllStandardError().constData());
        bool Parsed = false; const auto ChildId = Owner.readLine().trimmed().toUInt(&Parsed); QVERIFY(Parsed && ChildId);
        WinHandle Child(OpenProcess(SYNCHRONIZE, FALSE, ChildId)); QVERIFY(Child.Valid());
        Owner.kill(); QVERIFY(Owner.waitForFinished(5000)); QCOMPARE(WaitForSingleObject(Child.Get(), 3000), DWORD(WAIT_OBJECT_0));
    }
    void crashAndHang_data()
    {
        QTest::addColumn<QString>("Fixture"); QTest::addColumn<bool>("Cancel");
        QTest::newRow("crash") << "hc_plugin_fault_crash.dll" << false;
        QTest::newRow("timeout") << "hc_plugin_fault_hang.dll" << false;
        QTest::newRow("cancel") << "hc_plugin_fault_hang.dll" << true;
    }
    void crashAndHang()
    {
        QFETCH(QString, Fixture); QFETCH(bool, Cancel);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, Fixture, Approval));
        HostLimits Limits; Limits.requestMs_ = 500; Limits.heartbeatMs_ = 200;
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits);
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str()); auto Host = std::move(Started.Value());
        std::stop_source Stop; OperationContext Context; Context.stopToken_ = Stop.get_token();
        std::jthread Canceller([&](std::stop_token) { if (Cancel) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); Stop.request_stop(); } });
        QElapsedTimer Timer; Timer.start(); auto Probe = Host->Probe(Context); QVERIFY(!Probe.HasValue()); QVERIFY(Timer.elapsed() < 3000);
        if (Cancel) QCOMPARE(Probe.Failure().category_, ErrorCategory::Cancelled);
        QTRY_VERIFY_WITH_TIMEOUT(!Host->Snapshot().running_, 3000); QVERIFY(Host->Snapshot().failure_.has_value()); QVERIFY(Host->Snapshot().exitCode_.has_value());
        if (Fixture.contains("crash")) QCOMPARE(Host->Snapshot().exitCode_.value(), DWORD(EXCEPTION_ACCESS_VIOLATION));
        qInfo() << "Host failure" << QString::fromStdString(Probe.Failure().code_) << "exit" << Host->Snapshot().exitCode_.value();
    }
    void childCreationDenied()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_plugin_fault_child.dll", Approval));
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval);
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Probe = Started.Value()->Probe(); QVERIFY(Probe.HasValue()); QCOMPARE(Probe.Value()["caps"].toInt(-1), 0);
    }
    void hangingDetachIsReaped()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_plugin_fault_detach.dll", Approval));
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval);
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        QElapsedTimer Timer; Timer.start(); Started.Value()->Close(); QVERIFY(Timer.elapsed() < 3000);
        QVERIFY(!Started.Value()->Snapshot().running_); QVERIFY(Started.Value()->Snapshot().forcedTermination_);
    }
    void startupUsesOwnDeadline()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_plugin_fault_slowstart.dll", Approval));
        HostLimits Limits; Limits.startupMs_ = 5000; Limits.requestMs_ = 100;
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits);
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str()); QVERIFY(Started.Value()->Probe().HasValue());
    }
    void hangingStartupIsReaped()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_plugin_fault_attach.dll", Approval));
        HostLimits Limits; Limits.startupMs_ = 500;
        QElapsedTimer Timer; Timer.start(); auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits);
        QVERIFY(!Started.HasValue()); QCOMPARE(Started.Failure().code_, std::string("deadline-exceeded")); QVERIFY(Timer.elapsed() < 3000);
        QFile File(Directory.filePath("test.wcx64")); QVERIFY(File.open(QIODevice::WriteOnly));
    }
    void heartbeatReapsUnresponsiveHost()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_wcx_fixture.wcx64", Approval));
        HostLimits Limits; Limits.heartbeatMs_ = 100;
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits);
        QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
        auto Host = std::move(Started.Value());
        // The OS test suspends only this host's threads. No protocol-only fake
        // simulates loss of a real process's heartbeat.
        const auto Pid = Host->Snapshot().processId_;
        WinHandle Threads(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)); QVERIFY(Threads.Valid());
        THREADENTRY32 Entry{}; Entry.dwSize = sizeof(Entry); QVERIFY(Thread32First(Threads.Get(), &Entry)); unsigned Suspended = 0;
        do {
            if (Entry.th32OwnerProcessID != Pid) continue;
            WinHandle Thread(OpenThread(THREAD_SUSPEND_RESUME, FALSE, Entry.th32ThreadID)); QVERIFY(Thread.Valid());
            QVERIFY(SuspendThread(Thread.Get()) != DWORD(-1)); ++Suspended;
        } while (Thread32Next(Threads.Get(), &Entry));
        QVERIFY(Suspended > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!Host->Snapshot().running_, 3000); QVERIFY(Host->Snapshot().failure_.has_value());
    }
};
QTEST_GUILESS_MAIN(PluginProcessTests)
#include "tstPluginProcess.moc"
