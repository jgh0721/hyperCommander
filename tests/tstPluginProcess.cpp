#include <QtTest>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QProcess>
#include "pluginProcess.hpp"
#include "pipeChannel.hpp"
#include "archiveCodec.hpp"
#include "viewerHostRuntime.hpp"
#include "commonLibs/win/restrictedEnvironment.hpp"
#include <TlHelp32.h>
#include <thread>
#include <future>
#include <QScopeGuard>
#include <QUuid>

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
    void runtimeNegotiation_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto* Mode : {"profile", "identity", "control-epoch", "control-sequence", "replay", "wrong-channel", "generation", "control-budget", "partial-control", "stop"})
            QTest::newRow(Mode) << QString::fromLatin1(Mode);
    }
    void runtimeNegotiation()
    {
        QFETCH(QString, Mode);
        const auto Base = "\\\\.\\pipe\\hyperCommander-" + QUuid::createUuid().toString(QUuid::Id128);
        const auto ControlName = Base + "-control";
        const auto Server = [](const QString& Name) {
            return WinHandle(CreateNamedPipeW(reinterpret_cast<LPCWSTR>(Name.utf16()), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
        };
        auto Command = Server(Base), Control = Server(ControlName); QVERIFY(Command.Valid()); QVERIFY(Control.Valid());
        QProcess Child; Child.start(host_, {Base, QString::number(GetCurrentProcessId())}); QVERIFY(Child.waitForStarted(1500));
        const auto Cleanup = qScopeGuard([&] { if (Child.state() != QProcess::NotRunning) { Child.kill(); Child.waitForFinished(2000); } });
        WinHandle Peer(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(Child.processId()))); QVERIFY(Peer.Valid());
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        const auto Connect = [&](HANDLE Pipe) -> Result<Unit> {
            WinHandle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!Event.Valid()) return HostError("test-event");
            OVERLAPPED Pending{}; Pending.hEvent = Event.Get();
            if (!ConnectNamedPipe(Pipe, &Pending)) {
                const auto Code = GetLastError(); if (Code == ERROR_PIPE_CONNECTED) return Unit{};
                if (Code != ERROR_IO_PENDING) return HostError("test-connect", Code);
                auto Wait = WaitPipe(Pipe, Pending, Peer.Get(), Context); if (!Wait.HasValue()) return Wait.Failure();
                DWORD Count = 0; if (!GetOverlappedResult(Pipe, &Pending, &Count, FALSE)) return HostError("test-connect-result", GetLastError());
            }
            return Unit{};
        };
        QVERIFY(Connect(Command.Get()).HasValue());
        const auto Epoch = QUuid::createUuid().toString(QUuid::Id128), Nonce = QUuid::createUuid().toString(QUuid::Id128);
        const QJsonObject Hello{{"profile", Mode == "profile" ? "viewer-ui/999" : "viewer-ui/1"}, {"control", ControlName},
            {"nonce", Mode == "identity" ? QString(32, 'z') : Nonce}, {"epoch", Epoch}, {"heartbeatMs", 200}};
        QVERIFY(WriteFrame(Command.Get(), Peer.Get(), WireKind::RuntimeHello, 1, RuntimeJson(Hello), Context).HasValue());
        if (Mode == "profile" || Mode == "identity") {
            QVERIFY(Child.waitForFinished(1500)); QCOMPARE(Child.exitCode(), 7); return;
        }
        QVERIFY(Connect(Control.Get()).HasValue());
        auto Greeting = ReadFrame(Control.Get(), Peer.Get(), Context, false, 16384, 500); QVERIFY(Greeting.HasValue());
        QCOMPARE(Greeting.Value().header_.kind_, WireKind::ControlHello); QCOMPARE(Greeting.Value().header_.sequence_, std::uint64_t(1));
        const auto Declaration = RuntimeObject(Greeting.Value().payload_); QVERIFY(Declaration.HasValue());
        QCOMPARE(Declaration.Value()["epoch"].toString(), Epoch); QCOMPARE(Declaration.Value()["nonce"].toString(), Nonce);
        QCOMPARE(Declaration.Value()["processId"].toInteger(), Child.processId());
        QVERIFY(WriteFrame(Control.Get(), Peer.Get(), WireKind::Response, Mode == "control-sequence" ? 2 : 1,
            RuntimeJson({{"epoch", Mode == "control-epoch" ? QString(32, 'f') : Epoch}}), Context).HasValue());
        if (Mode == "control-epoch" || Mode == "control-sequence") {
            QVERIFY(Child.waitForFinished(1500)); QCOMPARE(Child.exitCode(), 6); return;
        }
        auto Negotiated = ReadFrame(Command.Get(), Peer.Get(), Context); QVERIFY(Negotiated.HasValue());
        QCOMPARE(Negotiated.Value().header_.kind_, WireKind::Response); QCOMPARE(Negotiated.Value().header_.sequence_, std::uint64_t(1));
        if (Mode == "wrong-channel" || Mode == "generation") {
            QVERIFY(WriteFrame(Command.Get(), Peer.Get(), Mode == "wrong-channel" ? WireKind::ControlQuery : WireKind::Hello, 2,
                RuntimeJson({{"epoch", Epoch}, {"generation", Mode == "generation" ? "1" : "0"}, {"request", QJsonObject{}}}), Context).HasValue());
        } else if (Mode == "control-budget" || Mode == "partial-control") {
            auto Header = EncodeHeader({{1, 0}, WireKind::ControlPing, 2, Mode == "control-budget" ? 16385u : 4u}); QVERIFY(Header.HasValue());
            QByteArray Bytes(reinterpret_cast<const char*>(Header.Value().data()), Header.Value().size()); if (Mode == "partial-control") Bytes += '{';
            WinHandle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); QVERIFY(Event.Valid()); OVERLAPPED Pending{}; Pending.hEvent = Event.Get(); DWORD Count = 0;
            if (!WriteFile(Control.Get(), Bytes.constData(), DWORD(Bytes.size()), &Count, &Pending)) {
                QCOMPARE(GetLastError(), DWORD(ERROR_IO_PENDING)); QVERIFY(WaitPipe(Control.Get(), Pending, Peer.Get(), Context).HasValue());
            }
            QVERIFY(GetOverlappedResult(Control.Get(), &Pending, &Count, FALSE)); QCOMPARE(Count, DWORD(Bytes.size()));
        } else {
            const auto Kind = Mode == "stop" ? WireKind::ControlStop : WireKind::ControlPing;
            QVERIFY(WriteFrame(Control.Get(), Peer.Get(), Kind, 2, RuntimeJson({{"epoch", Epoch}}), Context).HasValue());
            auto Reply = ReadFrame(Control.Get(), Peer.Get(), Context); QVERIFY(Reply.HasValue()); QCOMPARE(Reply.Value().header_.sequence_, std::uint64_t(2));
            if (Mode == "replay") QVERIFY(WriteFrame(Control.Get(), Peer.Get(), WireKind::ControlPing, 2, RuntimeJson({{"epoch", Epoch}}), Context).HasValue());
        }
        QVERIFY(Child.waitForFinished(2000)); QCOMPARE(Child.exitCode(), Mode == "stop" ? 0 : 6);
    }
    void channelFrameBounds_data()
    {
        QTest::addColumn<bool>("Oversize");
        QTest::newRow("partial-header-deadline") << false;
        QTest::newRow("control-payload-before-body") << true;
    }
    void channelFrameBounds()
    {
        QFETCH(bool, Oversize);
        const auto Name = "\\\\.\\pipe\\hyperCommander-frame-test-" + QUuid::createUuid().toString(QUuid::Id128);
        WinHandle Server(CreateNamedPipeW(reinterpret_cast<LPCWSTR>(Name.utf16()), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr)); QVERIFY(Server.Valid());
        std::promise<void> Retire; auto Released = Retire.get_future();
        auto Writer = std::async(std::launch::async, [&] {
            WinHandle Client(CreateFileW(reinterpret_cast<LPCWSTR>(Name.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
            if (!Client.Valid()) return false;
            auto Header = EncodeHeader({{1, 0}, WireKind::ControlPing, 1, Oversize ? 16385u : 10u});
            DWORD Written = 0; const auto Count = Oversize ? DWORD(WireHeaderBytes) : 2u;
            const bool Sent = Header.HasValue() && WriteFile(Client.Get(), Header.Value().data(), Count, &Written, nullptr) && Written == Count;
            Released.wait(); return Sent;
        });
        const auto Cleanup = qScopeGuard([&] { Retire.set_value(); Writer.wait(); });
        WinHandle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); QVERIFY(Event.Valid()); OVERLAPPED Pending{}; Pending.hEvent = Event.Get();
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        if (!ConnectNamedPipe(Server.Get(), &Pending)) {
            const auto Code = GetLastError();
            if (Code == ERROR_IO_PENDING) QVERIFY(WaitPipe(Server.Get(), Pending, nullptr, Context).HasValue());
            else QCOMPARE(Code, DWORD(ERROR_PIPE_CONNECTED));
        }
        QElapsedTimer Timer; Timer.start();
        auto Frame = ReadFrame(Server.Get(), nullptr, Context, false, 16384, 120);
        QVERIFY(!Frame.HasValue());
        QCOMPARE(Frame.Failure().code_, std::string(Oversize ? "host-wire-channel-budget" : "deadline-exceeded"));
        QVERIFY(Timer.elapsed() < 500);
    }
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
        QVERIFY(!EncodeHeader({{1, 0}, WireKind::SessionPause, 1, 1}).HasValue());
        auto Pause = EncodeHeader({{2, 0}, WireKind::SessionPause, 7, 1}); QVERIFY(Pause.HasValue());
        auto Paused = DecodeHeader(Pause.Value()); QVERIFY(Paused.HasValue());
        QCOMPARE(Paused.Value().kind_, WireKind::SessionPause); QCOMPARE(Paused.Value().sequence_, std::uint64_t(7));
        auto V2 = EncodeHeader({{2, 0}, WireKind::SessionInvoke, UINT64_MAX, 0}); QVERIFY(V2.HasValue());
        QCOMPARE(DecodeHeader(V2.Value()).Value().sequence_, UINT64_MAX);
        for (const auto Kind : {WireKind::RuntimeHello, WireKind::ControlHello, WireKind::ControlPing, WireKind::ControlQuery, WireKind::ControlCancel, WireKind::ControlStop}) {
            auto Runtime = EncodeHeader({{1, 0}, Kind, 1, 16 * 1024}); QVERIFY(Runtime.HasValue());
            QCOMPARE(DecodeHeader(Runtime.Value()).Value().kind_, Kind);
            QVERIFY(!EncodeHeader({{2, 0}, Kind, 1, 0}).HasValue());
        }
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
            qInfo() << "Configuration start" << Dopus << Started.Value()->Snapshot().runtime_;
            QElapsedTimer ConfigureTimer; ConfigureTimer.start();
            auto Value = Started.Value()->Configure(uint64_t(Owner), Context);
            qInfo() << "Configuration result before close" << Dopus << ConfigureTimer.elapsed() << Value.HasValue() << Started.Value()->Snapshot().runtime_;
            Started.Value()->Close();
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
    void parentDeathReapsHost_data()
    { QTest::addColumn<bool>("Configuration"); QTest::newRow("archive-data-v2") << false; QTest::newRow("viewer-ui-runtime") << true; }
    void parentDeathReapsHost()
    {
        QFETCH(bool, Configuration);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_wcx_fixture.wcx64", Approval));
        if (Configuration) {
            Approval.reviewed_.permissions_.insert(Permission::UserInterface); Approval.granted_ = Approval.reviewed_.permissions_;
            QFile Manifest(Directory.filePath("plugin.json")); QVERIFY(Manifest.open(QIODevice::WriteOnly)); QVERIFY(Manifest.write(EncodeManifest(Approval.reviewed_)) > 0);
        }
        QStringList Arguments{host_, Directory.filePath("plugin.json")}; if (Configuration) Arguments << "--configuration";
        QProcess Owner; Owner.start(QCoreApplication::applicationDirPath() + "/hc_plugin_owner_fixture.exe", Arguments);
        QVERIFY(Owner.waitForStarted(5000)); QVERIFY2(Owner.waitForReadyRead(5000), Owner.readAllStandardError().constData());
        bool Parsed = false; const auto ChildId = Owner.readLine().trimmed().toUInt(&Parsed); QVERIFY(Parsed && ChildId);
        WinHandle Child(OpenProcess(SYNCHRONIZE, FALSE, ChildId)); QVERIFY(Child.Valid());
        Owner.kill(); QVERIFY(Owner.waitForFinished(5000)); QCOMPARE(WaitForSingleObject(Child.Get(), 3000), DWORD(WAIT_OBJECT_0));
    }
    void isolatedIoThreadStalls_data()
    {
        QTest::addColumn<QString>("Role"); QTest::newRow("command-only") << "command"; QTest::newRow("control-only") << "control";
    }
    void isolatedIoThreadStalls()
    {
        QFETCH(QString, Role); QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_wcx_fixture.wcx64", Approval));
        Approval.reviewed_.permissions_.insert(Permission::UserInterface); Approval.granted_ = Approval.reviewed_.permissions_;
        { QFile Manifest(Directory.filePath("plugin.json")); QVERIFY(Manifest.open(QIODevice::WriteOnly)); QVERIFY(Manifest.write(EncodeManifest(Approval.reviewed_)) > 0); }
        HostLimits Limits; Limits.purpose_ = HostPurpose::Configuration; Limits.requestMs_ = 500; Limits.heartbeatMs_ = 200;
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits); QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value());
        const auto Pid = Host->Snapshot().processId_;
        WinHandle Threads(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)); QVERIFY(Threads.Valid());
        THREADENTRY32 Entry{sizeof(Entry)}; QVERIFY(Thread32First(Threads.Get(), &Entry)); WinHandle Suspended;
        do {
            if (Entry.th32OwnerProcessID != Pid) continue;
            WinHandle Thread(OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME, FALSE, Entry.th32ThreadID)); QVERIFY(Thread.Valid());
            PWSTR Name = nullptr; if (FAILED(GetThreadDescription(Thread.Get(), &Name))) continue;
            const auto Match = QString::fromWCharArray(Name) == "HC viewer " + Role + " I/O"; LocalFree(Name);
            if (Match) { QVERIFY(SuspendThread(Thread.Get()) != DWORD(-1)); Suspended = std::move(Thread); break; }
        } while (Thread32Next(Threads.Get(), &Entry));
        QVERIFY(Suspended.Valid()); const auto Resume = qScopeGuard([&] { ResumeThread(Suspended.Get()); });
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        QElapsedTimer Timer; Timer.start(); const auto Reply = Host->Probe(Context);
        if (Role == "command") QVERIFY(!Reply.HasValue());
        else QVERIFY(Reply.HasValue()); // Command I/O and SDK completion remain independent of the stopped control thread.
        QVERIFY(Timer.elapsed() < 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!Host->Snapshot().running_, 2000); const auto Snapshot = Host->Snapshot(); QVERIFY(Snapshot.failure_);
        qInfo() << "Isolated I/O stall" << Role << "reaped ms" << Timer.elapsed() << Snapshot.runtime_;
        if (Role == "command") {
            QCOMPARE(Snapshot.failure_->code_, std::string("host-command-acceptance-stalled"));
            QCOMPARE(Snapshot.runtime_["probeFailures"].toInt(), 0);
        }
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
    void busyHostObservationAndQueuedDeadline()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_plugin_fault_hang.dll", Approval));
        HostLimits Limits; Limits.requestMs_ = 1500; Limits.heartbeatMs_ = 5000;
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits);
        QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value());
        std::atomic<bool> Calling = false;
        auto Work = std::async(std::launch::async, [&] { Calling = true; return Host->Probe(); });
        QTRY_VERIFY(Calling.load()); QTest::qWait(80);
        QElapsedTimer Timer; Timer.start(); const auto Busy = Host->Snapshot();
        QVERIFY2(Timer.elapsed() < 150, "A status query must not wait for the stalled plugin's IPC mutex");
        QVERIFY(Busy.busy_); QVERIFY(Busy.running_);
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(70);
        Timer.restart(); auto Queued = Host->Ping(Context);
        QVERIFY(!Queued.HasValue()); QCOMPARE(Queued.Failure().code_, std::string("deadline-exceeded"));
        QVERIFY2(Timer.elapsed() < 300, "A queued request must honour its own deadline before acquiring the pipe");
        QCOMPARE(Work.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
        QVERIFY(Work.wait_for(std::chrono::seconds(4)) == std::future_status::ready); QVERIFY(!Work.get().HasValue());
        const auto Failed = Host->Snapshot(); QVERIFY(!Failed.busy_); QVERIFY(Failed.failure_);
        Timer.restart(); Host->Close(); QVERIFY(Timer.elapsed() < 3000);
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
    void configurationFaults_data()
    {
        QTest::addColumn<bool>("Attach");
        QTest::newRow("dll-process-attach") << true;
        QTest::newRow("dll-process-detach") << false;
    }
    void configurationFaults()
    {
        QFETCH(bool, Attach);
        QTemporaryDir Directory; PluginApproval Approval;
        QVERIFY(prepare(Directory, Attach ? "hc_plugin_fault_attach.dll" : "hc_plugin_fault_detach.dll", Approval));
        Approval.reviewed_.permissions_.insert(Permission::UserInterface); Approval.granted_ = Approval.reviewed_.permissions_;
        { QFile Manifest(Directory.filePath("plugin.json")); QVERIFY(Manifest.open(QIODevice::WriteOnly)); QVERIFY(Manifest.write(EncodeManifest(Approval.reviewed_)) > 0); }
        HostLimits Limits; Limits.purpose_ = HostPurpose::Configuration; Limits.startupMs_ = 1000; Limits.requestMs_ = 500; Limits.heartbeatMs_ = 200;
        QElapsedTimer Timer; Timer.start();
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits);
        if (Attach) QVERIFY(!Started.HasValue());
        else {
            QVERIFY2(Started.HasValue(), Started.HasValue() ? "" : Started.Failure().code_.c_str());
            Started.Value()->Close(); QVERIFY(!Started.Value()->Snapshot().running_); QVERIFY(Started.Value()->Snapshot().forcedTermination_);
        }
        QVERIFY(Timer.elapsed() < 3000);
        QFile Binary(Directory.filePath("test.wcx64")); QVERIFY(Binary.open(QIODevice::ReadWrite));
    }
    void pinnedThreadDetachIsReaped()
    {
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_plugin_fault_threaddetach.dll", Approval));
        Approval.reviewed_.permissions_.insert(Permission::UserInterface); Approval.granted_ = Approval.reviewed_.permissions_;
        { QFile Manifest(Directory.filePath("plugin.json")); QVERIFY(Manifest.open(QIODevice::WriteOnly)); QVERIFY(Manifest.write(EncodeManifest(Approval.reviewed_)) > 0); }
        HostLimits Limits; Limits.purpose_ = HostPurpose::Configuration; Limits.heartbeatMs_ = 200;
        auto Started = PluginProcess::Start(host_, Directory.filePath("plugin.json"), Approval, Limits);
        QVERIFY(Started.HasValue()); QVERIFY(Started.Value()->Probe().HasValue());
        QElapsedTimer Timer; Timer.start(); Started.Value()->Close();
        QVERIFY(Timer.elapsed() < 3000); QVERIFY(!Started.Value()->Snapshot().running_); QVERIFY(Started.Value()->Snapshot().forcedTermination_);
    }
    void startupUsesOwnDeadline_data()
    {
        QTest::addColumn<bool>("Configuration"); QTest::newRow("archive-data-v2") << false;
        QTest::newRow("configuration-runtime-750ms-attach") << true;
    }
    void startupUsesOwnDeadline()
    {
        QFETCH(bool, Configuration);
        QTemporaryDir Directory; PluginApproval Approval; QVERIFY(prepare(Directory, "hc_plugin_fault_slowstart.dll", Approval));
        HostLimits Limits; Limits.startupMs_ = 5000; Limits.requestMs_ = 100;
        if (Configuration) {
            Approval.reviewed_.permissions_.insert(Permission::UserInterface); Approval.granted_ = Approval.reviewed_.permissions_;
            QFile Manifest(Directory.filePath("plugin.json")); QVERIFY(Manifest.open(QIODevice::WriteOnly)); QVERIFY(Manifest.write(EncodeManifest(Approval.reviewed_)) > 0); Manifest.close();
            Limits.purpose_ = HostPurpose::Configuration; Limits.startupMs_ = 1000; Limits.heartbeatMs_ = 200;
        }
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
