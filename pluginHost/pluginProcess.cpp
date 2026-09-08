#include "pluginProcess.hpp"
#include "pipeChannel.hpp"
#include <sddl.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>
#include <thread>
#include <condition_variable>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        OperationContext Bound(OperationContext Context, std::uint32_t Milliseconds)
        { Context.deadline_ = std::min(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::milliseconds(Milliseconds)); return Context; }
        std::wstring Quote(const QString& Value)
        {
            std::wstring Out = L"\""; unsigned Slashes = 0;
            for (auto Ch : Value.toStdWString())
            {
                if (Ch == L'\\') { ++Slashes; continue; }
                Out.append(Ch == L'"' ? Slashes * 2 + 1 : Slashes, L'\\'); Slashes = 0; Out += Ch;
            }
            Out.append(Slashes * 2, L'\\'); return Out + L'"';
        }
        Result<WinHandle> RestrictedToken()
        {
            HANDLE Raw = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, &Raw)) return HostError("host-token-open", GetLastError());
            WinHandle Original(Raw);
            BYTE Admin[SECURITY_MAX_SID_SIZE]; DWORD Size = sizeof(Admin);
            if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, Admin, &Size)) return HostError("host-admin-sid", GetLastError());
            SID_AND_ATTRIBUTES Denied{Admin, 0}; Raw = nullptr;
            if (!CreateRestrictedToken(Original.Get(), DISABLE_MAX_PRIVILEGE, 1, &Denied, 0, nullptr, 0, nullptr, &Raw)) return HostError("host-token-restrict", GetLastError());
            return WinHandle(Raw);
        }
        Result<WinHandle> ServerPipe(const QString& Name)
        {
            HANDLE Raw = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw)) return HostError("host-token-open", GetLastError());
            WinHandle Token(Raw); DWORD Size = 0; GetTokenInformation(Token.Get(), TokenUser, nullptr, 0, &Size);
            std::vector<std::byte> Data(Size); if (!GetTokenInformation(Token.Get(), TokenUser, Data.data(), Size, &Size)) return HostError("host-token-user", GetLastError());
            LPWSTR Sid = nullptr;
            if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(Data.data())->User.Sid, &Sid)) return HostError("host-token-sid", GetLastError());
            const auto Sddl = L"D:P(A;;GA;;;" + std::wstring(Sid) + L")"; LocalFree(Sid);
            PSECURITY_DESCRIPTOR Descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl.c_str(), SDDL_REVISION_1, &Descriptor, nullptr)) return HostError("host-pipe-acl", GetLastError());
            SECURITY_ATTRIBUTES Security{sizeof(Security), Descriptor, FALSE};
            Raw = CreateNamedPipeW(reinterpret_cast<LPCWSTR>(Name.utf16()), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &Security);
            const auto Error = GetLastError(); LocalFree(Descriptor); WinHandle Pipe(Raw);
            if (!Pipe.Valid()) return HostError("host-pipe-create", Error); return std::move(Pipe);
        }
    }
    struct PluginProcess::State
    {
        mutable std::mutex mutex_;
        WinHandle pipe_, job_, process_;
        HostLimits limits_;
        DWORD pid_ = 0;
        bool viewer_ = false;
        HWND viewerParent_ = nullptr;
        std::uint64_t sequence_ = 0;
        std::optional<Error> failure_;
        bool closed_ = false;
        bool ready_ = false, forcedTermination_ = false;
        std::chrono::steady_clock::time_point lastCall_ = std::chrono::steady_clock::now();
        std::jthread heartbeat_;
        std::mutex sleepMutex_;
        std::condition_variable_any sleep_;
        void fail(Error Error)
        {
            if (!failure_) failure_ = std::move(Error);
            // Preserve an OS crash exit code when the broken pipe wins the race
            // with process termination. Hung/live jobs still get bounded teardown.
            if (job_.Valid() && (!process_.Valid() || WaitForSingleObject(process_.Get(), 100) == WAIT_TIMEOUT))
            { forcedTermination_ = true; TerminateJobObject(job_.Get(), 0xe0000001); }
        }
        Result<QJsonObject> call(WireKind Kind, const QByteArray& Payload, OperationContext Context)
        {
            if (failure_) return *failure_;
            if (closed_) return HostError("host-closed");
            Context = Bound(Context, Kind == WireKind::Hello ? limits_.startupMs_ : limits_.requestMs_);
            const auto Sequence = ++sequence_;
            auto Sent = WriteFrame(pipe_.Get(), process_.Get(), Kind, Sequence, Payload, Context);
            if (!Sent.HasValue()) { fail(Sent.Failure()); return *failure_; }
            auto Reply = ReadFrame(pipe_.Get(), process_.Get(), Context);
            if (!Reply.HasValue()) { fail(Reply.Failure()); return *failure_; }
            if (Reply.Value().header_.sequence_ != Sequence || (Reply.Value().header_.kind_ != WireKind::Response && Reply.Value().header_.kind_ != WireKind::Failure))
            { fail(HostError("host-response-mismatch")); return *failure_; }
            QJsonParseError Parse; const auto Document = QJsonDocument::fromJson(Reply.Value().payload_, &Parse);
            if (Parse.error != QJsonParseError::NoError || !Document.isObject()) { fail(HostError("host-response-json")); return *failure_; }
            lastCall_ = std::chrono::steady_clock::now();
            if (Reply.Value().header_.kind_ == WireKind::Failure)
            {
                const auto Code = Document.object()["code"].toString();
                if (Code.isEmpty() || Code.size() > 256) { fail(HostError("host-error-envelope")); return *failure_; }
                auto Failure = HostError(Code.toUtf8().constData());
                const auto Category = Document.object()["category"].toInt(-1);
                if (Category < 0 || Category > static_cast<int>(ErrorCategory::Internal)) { fail(HostError("host-error-category")); return *failure_; }
                Failure.category_ = static_cast<ErrorCategory>(Category); Failure.provider_.value_ = Document.object()["provider"].toString().toStdString();
                Failure.retryable_ = Document.object()["retryable"].toBool(); Failure.userAction_ = Document.object()["userAction"].toString().toStdString();
                return Failure;
            }
            return Document.object();
        }
    };
    PluginProcess::PluginProcess(std::unique_ptr<State> Value) : state_(std::move(Value)) {}
    PluginProcess::~PluginProcess() { Close(); }
    Result<std::unique_ptr<PluginProcess>> PluginProcess::Start(const QString& Executable, const QString& ManifestPath,
        const PluginApproval& Approval, HostLimits Limits, OperationContext Context, const QString& DefaultIniPath)
    {
        if (Limits.startupMs_ < 100 || Limits.startupMs_ > 60000 || Limits.requestMs_ < 100 || Limits.requestMs_ > 60000
            || Limits.heartbeatMs_ < 100 || Limits.heartbeatMs_ > 60000 || Limits.memoryBytes_ < 64ULL * 1024 * 1024 || Limits.memoryBytes_ > 4ULL * 1024 * 1024 * 1024)
            return HostError("host-invalid-limits");
        Context = Bound(Context, Limits.startupMs_);
        auto Inspected = InspectManifestFile(ManifestPath, Context); if (!Inspected.HasValue()) return Inspected.Failure();
        HostProfile Profile; Profile.formats_ = {PluginFormat::Wcx, PluginFormat::Wlx};
        Profile.extensions_ = {Extension::ArchiveRead, Extension::ArchiveWrite, Extension::Viewer}; Profile.supportsIsolation_ = true;
        auto Admission = Admit(Inspected.Value().manifest_, Approval, Profile); if (!Admission.HasValue()) return Admission.Failure();
        const auto HostPath = QFileInfo(Executable).canonicalFilePath();
        if (HostPath.isEmpty() || !QFileInfo(HostPath).isFile()) return HostError("host-executable-missing");
        auto Token = RestrictedToken(); if (!Token.HasValue()) return Token.Failure();
        const auto PipeName = "\\\\.\\pipe\\hyperCommander-" + QUuid::createUuid().toString(QUuid::Id128);
        auto Pipe = ServerPipe(PipeName); if (!Pipe.HasValue()) return Pipe.Failure();
        auto Owner = std::unique_ptr<PluginProcess>(new PluginProcess(std::make_unique<State>())); auto& S = *Owner->state_;
        S.limits_ = Limits; S.pipe_ = std::move(Pipe.Value()); S.job_.Reset(CreateJobObjectW(nullptr, nullptr));
        S.viewer_ = Inspected.Value().manifest_.format_ == PluginFormat::Wlx;
        if (!S.job_.Valid()) return HostError("host-job-create", GetLastError());
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Job{};
        Job.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        Job.BasicLimitInformation.ActiveProcessLimit = 1; Job.ProcessMemoryLimit = static_cast<SIZE_T>(Limits.memoryBytes_);
        if (!SetInformationJobObject(S.job_.Get(), JobObjectExtendedLimitInformation, &Job, sizeof(Job))) return HostError("host-job-limits", GetLastError());
        JOBOBJECT_BASIC_UI_RESTRICTIONS Ui{JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD
            | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_EXITWINDOWS};
        if (!SetInformationJobObject(S.job_.Get(), JobObjectBasicUIRestrictions, &Ui, sizeof(Ui))) return HostError("host-job-ui", GetLastError());
        auto Command = Quote(HostPath) + L" " + Quote(PipeName) + L" " + std::to_wstring(GetCurrentProcessId());
        // Native nonvisual host receives no application secrets or Qt/plugin path
        // overrides. Its DLL search policy is configured before loading a plugin.
        const auto SystemRoot = qEnvironmentVariable("SystemRoot");
        QStringList Variables{"PATH=" + SystemRoot + "/System32", "SystemRoot=" + SystemRoot, "TEMP=" + QDir::tempPath(), "TMP=" + QDir::tempPath()};
        Variables.sort(Qt::CaseInsensitive); std::wstring Environment;
        for (const auto& Variable : Variables) { Environment += Variable.toStdWString(); Environment += L'\0'; } Environment += L'\0';
        STARTUPINFOW Startup{}; Startup.cb = sizeof(Startup); Startup.dwFlags = STARTF_USESHOWWINDOW; Startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION Process{};
        if (!CreateProcessAsUserW(Token.Value().Get(), reinterpret_cast<LPCWSTR>(HostPath.utf16()), Command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, Environment.data(), reinterpret_cast<LPCWSTR>(QFileInfo(HostPath).absolutePath().utf16()), &Startup, &Process))
            return HostError("host-process-create", GetLastError());
        S.process_.Reset(Process.hProcess); S.pid_ = Process.dwProcessId; WinHandle Thread(Process.hThread);
        if (!AssignProcessToJobObject(S.job_.Get(), S.process_.Get())) { const auto Error = GetLastError(); TerminateProcess(S.process_.Get(), 0xe0000002); return HostError("host-job-assign", Error); }
        if (ResumeThread(Thread.Get()) == DWORD(-1)) return HostError("host-process-resume", GetLastError());
        WinHandle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!Event.Valid()) return HostError("host-pipe-event", GetLastError());
        OVERLAPPED Pending{}; Pending.hEvent = Event.Get();
        if (!ConnectNamedPipe(S.pipe_.Get(), &Pending))
        {
            const auto Error = GetLastError();
            if (Error == ERROR_IO_PENDING) { auto Wait = WaitPipe(S.pipe_.Get(), Pending, S.process_.Get(), Context); if (!Wait.HasValue()) return Wait.Failure(); }
            else if (Error != ERROR_PIPE_CONNECTED) return HostError("host-pipe-connect", Error);
        }
        ULONG Peer = 0; if (!GetNamedPipeClientProcessId(S.pipe_.Get(), &Peer) || Peer != S.pid_) return HostError("host-pipe-peer");
        QJsonObject Declaration{{"manifestPath", QFileInfo(ManifestPath).absoluteFilePath()},
            {"manifest", QJsonDocument::fromJson(EncodeManifest(Inspected.Value().manifest_)).object()}};
        if (S.viewer_)
        {
            const auto Ini = DefaultIniPath.isEmpty()
                ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("lsplugin.ini") : DefaultIniPath;
            if (!QDir::isAbsolutePath(Ini) || Ini.contains(QChar::Null)) return HostError("host-wlx-config-path");
            Declaration.insert("defaultIni", QDir::cleanPath(Ini));
        }
        const auto Hello = QJsonDocument(Declaration).toJson(QJsonDocument::Compact);
        auto Ready = S.call(WireKind::Hello, Hello, Context); if (!Ready.HasValue()) return Ready.Failure();
        if (!Ready.Value()["inJob"].toBool() || !Ready.Value()["adminDenied"].toBool() || !Ready.Value()["privilegesRemoved"].toBool()) return HostError("host-restriction-verification");
        S.ready_ = true;
        S.heartbeat_ = std::jthread([State = &S](std::stop_token Stop) {
            while (!Stop.stop_requested())
            {
                { std::unique_lock SleepLock(State->sleepMutex_); State->sleep_.wait_for(SleepLock, Stop, std::chrono::milliseconds(State->limits_.heartbeatMs_), [] { return false; }); }
                if (Stop.stop_requested()) break;
                std::unique_lock Lock(State->mutex_, std::try_to_lock); if (!Lock.owns_lock()) continue;
                if (State->closed_ || State->failure_) break;
                if (std::chrono::steady_clock::now() - State->lastCall_ < std::chrono::milliseconds(State->limits_.heartbeatMs_)) continue;
                // A normal Close lets a small in-flight ping complete so it can
                // subsequently deliver Close and run DLL detach. Cancelling its
                // partially read frame would instead require killing the host.
                auto Reply = State->call(WireKind::Ping, {}, Bound({}, std::min(State->limits_.heartbeatMs_, 500u)));
                if (!Reply.HasValue()) { State->fail(Reply.Failure()); break; }
            }
        });
        return Owner;
    }
    Result<QJsonObject> PluginProcess::Probe(OperationContext Context)
    { std::lock_guard Lock(state_->mutex_); return state_->call(WireKind::Probe, {}, Context); }
    Result<QJsonObject> PluginProcess::Ping(OperationContext Context)
    { std::lock_guard Lock(state_->mutex_); return state_->call(WireKind::Ping, {}, Context); }
    Result<QJsonObject> PluginProcess::ArchiveCall(WireKind Kind, QJsonObject Request, OperationContext Context)
    {
        if (Kind < WireKind::ArchiveOpen || Kind > WireKind::ArchiveUpdateApply) return HostError("host-archive-command");
        std::lock_guard Lock(state_->mutex_); return state_->call(Kind, QJsonDocument(Request).toJson(QJsonDocument::Compact), Context);
    }
    Result<QJsonObject> PluginProcess::ViewerCall(WireKind Kind, QJsonObject Request, OperationContext Context)
    {
        if (Kind < WireKind::ViewerOpen || Kind > WireKind::ViewerClose) return HostError("host-viewer-command");
        std::lock_guard Lock(state_->mutex_); auto& S = *state_;
        if (!S.viewer_ || S.closed_ || S.failure_) return HostError("host-viewer-unavailable");
        if (Kind == WireKind::ViewerOpen)
        {
            if (S.viewerParent_) return HostError("host-viewer-already-open");
            bool Parsed = false; const auto Value = Request["parent"].toString().toULongLong(&Parsed, 16);
            const auto Parent = reinterpret_cast<HWND>(static_cast<quintptr>(Value)); DWORD Owner = 0;
            if (!Parsed || !Parent || !GetWindowThreadProcessId(Parent, &Owner) || Owner != GetCurrentProcessId()) return HostError("host-viewer-parent");
            // Keep HANDLES restrictions and grant only the caller-owned surface.
            // Do not remove the restriction for all windows on the desktop.
            if (!UserHandleGrantAccess(Parent, S.job_.Get(), TRUE)) return HostError("host-viewer-parent-grant", GetLastError());
            S.viewerParent_ = Parent;
        }
        auto Result = S.call(Kind, QJsonDocument(Request).toJson(QJsonDocument::Compact), Context);
        if ((Kind == WireKind::ViewerOpen && !Result.HasValue()) || (Kind == WireKind::ViewerClose && Result.HasValue()))
        {
            if (S.viewerParent_) UserHandleGrantAccess(S.viewerParent_, S.job_.Get(), FALSE);
            S.viewerParent_ = nullptr;
        }
        return Result;
    }
    HostSnapshot PluginProcess::Snapshot() const
    {
        std::lock_guard Lock(state_->mutex_); HostSnapshot Snapshot; Snapshot.processId_ = state_->pid_; Snapshot.failure_ = state_->failure_; Snapshot.forcedTermination_ = state_->forcedTermination_;
        if (state_->process_.Valid()) { DWORD Exit = 0; if (GetExitCodeProcess(state_->process_.Get(), &Exit)) { Snapshot.running_ = WaitForSingleObject(state_->process_.Get(), 0) == WAIT_TIMEOUT; if (!Snapshot.running_) Snapshot.exitCode_ = Exit; } }
        return Snapshot;
    }
    void PluginProcess::Close()
    {
        auto& S = *state_;
        S.heartbeat_.request_stop(); if (S.heartbeat_.joinable()) S.heartbeat_.join();
        std::lock_guard Lock(S.mutex_); if (S.closed_) return;
        if (S.ready_ && !S.failure_ && S.process_.Valid() && WaitForSingleObject(S.process_.Get(), 0) == WAIT_TIMEOUT)
        {
            // Let DllMain/thread cleanup run, but never wait indefinitely in an
            // uncooperative plugin's detach handler.
            S.call(WireKind::Close, {}, Bound({}, 200));
            WaitForSingleObject(S.process_.Get(), 500);
        }
        // Job closure is the final authority, including suspended startup failures.
        if (S.job_.Valid() && S.process_.Valid() && WaitForSingleObject(S.process_.Get(), 0) == WAIT_TIMEOUT)
        { S.forcedTermination_ = true; TerminateJobObject(S.job_.Get(), 0); }
        if (S.process_.Valid()) WaitForSingleObject(S.process_.Get(), 2000);
        if (S.viewerParent_) UserHandleGrantAccess(S.viewerParent_, S.job_.Get(), FALSE);
        S.viewerParent_ = nullptr;
        S.closed_ = true; S.pipe_.Reset(); S.job_.Reset();
    }
}
