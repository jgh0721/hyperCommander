#include "pluginProcess.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include "commonLibs/win/restrictedEnvironment.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "pipeChannel.hpp"
#include "sessionClient.hpp"
#include "archiveCodec.hpp"
#include <cstring>
#include <sddl.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>
#include <thread>
#include <QTemporaryDir>
#include <QXmlStreamWriter>
#include <QScopeGuard>
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
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &Raw)) return HostError("host-token-open", GetLastError());
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
        std::unique_ptr<QTemporaryDir> helperConfig_;
        bool helpers_ = false;
        QString helperPath_;
        DWORD pid_ = 0;
        bool viewer_ = false;
        HWND viewerParent_ = nullptr, viewerRoot_ = nullptr;
        bool documentViewer_ = false;
        std::uint64_t sequence_ = 0;
        std::optional<Error> failure_;
        bool closed_ = false;
        bool ready_ = false, forcedTermination_ = false;
        unsigned archiveOptionsVersion_ = 0;
        std::chrono::steady_clock::time_point lastCall_ = std::chrono::steady_clock::now();
        std::jthread heartbeat_;
        std::mutex sleepMutex_;
        std::condition_variable_any sleep_;
        QString epoch_;
        std::atomic<ProviderSessionState> sessionState_{ProviderSessionState::Closed};
        ReadChunk streamReply_;
        void fail(Error Error)
        {
            if (!failure_) failure_ = std::move(Error);
            sessionState_ = ProviderSessionState::Faulted;
            // Preserve an OS crash exit code when the broken pipe wins the race
            // with process termination. Hung/live jobs still get bounded teardown.
            if (job_.Valid() && (helpers_ || !process_.Valid() || WaitForSingleObject(process_.Get(), 100) == WAIT_TIMEOUT))
            { forcedTermination_ = true; TerminateJobObject(job_.Get(), 0xe0000001); }
        }
        Result<QJsonObject> call(WireKind Kind, const QByteArray& Payload, OperationContext Context, const QByteArray& Binary = {})
        {
            if (failure_) return *failure_;
            if (closed_) return HostError("host-closed");
            // In-process WCX writers also stream and verify multi-GiB images.
            // The write budget is independent of permission to spawn helpers.
            const auto RequestBudget = Context.passwordPrompt_ ? std::max(limits_.requestMs_, 180000u) : limits_.requestMs_;
            const bool Session = !epoch_.isEmpty() && (IsArchiveCommand(Kind) || IsDataCommand(Kind) || IsSessionCommand(Kind));
            if (!Session && Kind != WireKind::PluginConfigure) Context = Bound(Context, Kind == WireKind::Hello ? limits_.startupMs_ : (Kind == WireKind::ArchiveUpdateApply ? limits_.archiveWriteMs_ : RequestBudget));
            const auto Sequence = ++sequence_;
            Result<Unit> Sent = Unit{};
            Result<WireFrame> Reply = HostError("host-no-response");
            if (Session)
            {
                sessionState_ = ProviderSessionState::Busy;
                Reply = CallSession(pipe_.Get(), process_.Get(), epoch_, Sequence, Kind, QJsonDocument::fromJson(Payload).object(), Binary, Context,
                    limits_.requestMs_, limits_.noProgressMs_, limits_.cancelGraceMs_,
                    [this](bool Waiting) { sessionState_ = Waiting ? ProviderSessionState::AwaitingInteraction : ProviderSessionState::Busy; });
                sessionState_ = ProviderSessionState::Ready;
            }
            else
            {
                Sent = WriteFrame(pipe_.Get(), process_.Get(), Kind, Sequence, Payload, Context);
                if (!Sent.HasValue()) { fail(Sent.Failure()); return *failure_; }
                Reply = ReadFrame(pipe_.Get(), process_.Get(), Context);
            }
            unsigned Prompts = 0;
            while (Reply.HasValue() && Reply.Value().header_.kind_ == WireKind::ArchivePassword)
            {
                if (!IsArchiveCommand(Kind) || Reply.Value().header_.sequence_ != Sequence
                    || ++Prompts > (Kind == WireKind::ArchiveNext ? 384u : Kind == WireKind::ArchiveUpdateApply ? 6u : 3u))
                { fail(HostError("host-password-envelope")); return *failure_; }
                const auto Query = QJsonDocument::fromJson(Reply.Value().payload_).object();
                if (Query.size() != 2 || !Query["archive"].isString() || Query["archive"].toString().size() > 32768
                    || Query["archive"].toString().contains(QChar::Null) || Query["attempt"].toInt() < 1 || Query["attempt"].toInt() > 3)
                { fail(HostError("host-password-payload")); return *failure_; }
                Result<Password> Answer = Error{ErrorCategory::AccessDenied, "archive-password-required", {"wcx"}, {}, true};
                if (Context.passwordPrompt_) {
                    try { Answer = Context.passwordPrompt_(Query["archive"].toString().toStdString(), unsigned(Query["attempt"].toInt()), Context); }
                    catch (...) { Answer = Error{ErrorCategory::Internal, "archive-password-callback-failed", {"wcx"}}; }
                }
                if (!Answer.HasValue()) { fail(Answer.Failure()); return *failure_; }
                auto Text = QString::fromStdString(Answer.Value().Value());
                if (Text.size() > 4096 || Text.contains(QChar::Null)) { Text.fill(QChar::Null); fail(HostError("host-password-size")); return *failure_; }
                auto Bytes = QJsonDocument(QJsonObject{{"accepted", true}, {"password", Text}}).toJson(QJsonDocument::Compact); Text.fill(QChar::Null);
                Sent = WriteFrame(pipe_.Get(), process_.Get(), WireKind::ArchivePassword, Sequence, Bytes, Context); Bytes.fill(0);
                if (!Sent.HasValue()) { fail(Sent.Failure()); return *failure_; }
                Reply = ReadFrame(pipe_.Get(), process_.Get(), Context);
            }
            if (!Reply.HasValue()) { fail(Reply.Failure()); return *failure_; }
            if (Session && Reply.Value().header_.kind_ == WireKind::StreamData)
            {
                if (Kind != WireKind::StreamRead || Reply.Value().payload_.isEmpty() || (Reply.Value().payload_[0] != 0 && Reply.Value().payload_[0] != 1))
                { fail(HostError("host-stream-response")); return *failure_; }
                streamReply_.eof_ = Reply.Value().payload_[0] == 1;
                streamReply_.bytes_.resize(Reply.Value().payload_.size() - 1);
                std::memcpy(streamReply_.bytes_.data(), Reply.Value().payload_.constData() + 1, streamReply_.bytes_.size());
                lastCall_ = std::chrono::steady_clock::now(); return QJsonObject{};
            }
            if (Reply.Value().header_.sequence_ != Sequence || (Reply.Value().header_.kind_ != WireKind::Response && Reply.Value().header_.kind_ != WireKind::Failure))
            { fail(HostError("host-response-mismatch")); return *failure_; }
            QJsonParseError Parse; const auto Document = QJsonDocument::fromJson(Reply.Value().payload_, &Parse);
            if (Parse.error != QJsonParseError::NoError || !Document.isObject()) { fail(HostError("host-response-json")); return *failure_; }
            if (Kind == WireKind::ArchiveUpdateApply && helpers_)
            {
                // A WCX return alone is insufficient for a plugin that delegates
                // updates. Keep staging inputs alive until every helper exits.
                auto HelperProgressAt = std::chrono::steady_clock::now();
                std::map<DWORD, std::uint64_t> HelperTicks;
                for (;;)
                {
                    if (const auto Stop = Context.Check({"plugin-host"})) { fail(*Stop); return *failure_; }
                    if (Session && limits_.noProgressMs_ && std::chrono::steady_clock::now() - HelperProgressAt > std::chrono::milliseconds(limits_.noProgressMs_))
                    { fail(HostError("host-helper-no-progress")); return *failure_; }
                    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
                    if (!QueryInformationJobObject(job_.Get(), JobObjectBasicAccountingInformation, &Accounting, sizeof(Accounting), nullptr))
                    { fail(HostError("host-helper-accounting", GetLastError())); return *failure_; }
                    if (Accounting.ActiveProcesses <= 1) break;
                    alignas(JOBOBJECT_BASIC_PROCESS_ID_LIST) std::byte Buffer[sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + sizeof(ULONG_PTR) * 4]{};
                    auto* Processes = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(Buffer);
                    if (!QueryInformationJobObject(job_.Get(), JobObjectBasicProcessIdList, Processes, sizeof(Buffer), nullptr))
                    { fail(HostError("host-helper-process-list", GetLastError())); return *failure_; }
                    bool Busy = false;
                    wchar_t System[MAX_PATH]{}; GetSystemDirectoryW(System, MAX_PATH);
                    const auto Console = QDir::cleanPath(QString::fromWCharArray(System) + "/conhost.exe");
                    for (DWORD Index = 0; Index < Processes->NumberOfProcessIdsInList; ++Index)
                    {
                        const auto Pid = DWORD(Processes->ProcessIdList[Index]); if (Pid == pid_) continue;
                        WinHandle Child(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, Pid));
                        if (!Child.Valid()) { if (GetLastError() == ERROR_INVALID_PARAMETER) continue; fail(HostError("host-helper-process-open", GetLastError())); return *failure_; }
                        if (WaitForSingleObject(Child.Get(), 0) == WAIT_OBJECT_0) continue;
                        wchar_t Path[32768]{}; DWORD Size = DWORD(std::size(Path));
                        if (!QueryFullProcessImageNameW(Child.Get(), 0, Path, &Size))
                        { fail(HostError("host-helper-process-path", GetLastError())); return *failure_; }
                        const auto Image = QDir::fromNativeSeparators(QString::fromWCharArray(Path, Size));
                        // Total7zip allocates a hidden console that lasts until
                        // host shutdown. It is infrastructure, not a write task.
                        if (Image.compare(Console, Qt::CaseInsensitive) == 0) continue;
                        if (Image.compare(helperPath_, Qt::CaseInsensitive) != 0)
                        { fail(HostError("host-unexpected-child")); return *failure_; }
                        FILETIME Created{}, Exited{}, Kernel{}, User{};
                        if (GetProcessTimes(Child.Get(), &Created, &Exited, &Kernel, &User)) {
                            const auto Ticks = ((std::uint64_t(Kernel.dwHighDateTime) << 32) | Kernel.dwLowDateTime)
                                + ((std::uint64_t(User.dwHighDateTime) << 32) | User.dwLowDateTime);
                            if (!HelperTicks.contains(Pid) || Ticks > HelperTicks[Pid]) HelperProgressAt = std::chrono::steady_clock::now();
                            HelperTicks[Pid] = Ticks;
                        }
                        Busy = true;
                    }
                    if (!Busy) break;
                    // The WCX call has returned, so the host can answer pings.
                    // Keep its idle pipe deadline alive during a long helper
                    // wait and still detect a host that stops responding.
                    if (std::chrono::steady_clock::now() - lastCall_ >= std::chrono::milliseconds(limits_.heartbeatMs_))
                    {
                        auto Alive = call(WireKind::Ping, {}, Bound(Context, limits_.heartbeatMs_));
                        if (!Alive.HasValue()) return Alive;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
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
            || Limits.archiveWriteMs_ < 100 || Limits.archiveWriteMs_ > 3600000
            || Limits.cancelGraceMs_ < 100 || Limits.cancelGraceMs_ > 60000
            || Limits.heartbeatMs_ < 100 || Limits.heartbeatMs_ > 60000)
            return HostError("host-invalid-limits");
        Context = Bound(Context, Limits.startupMs_);
        auto Inspected = InspectManifestFile(ManifestPath, Context); if (!Inspected.HasValue()) return Inspected.Failure();
        const auto Format = Inspected.Value().manifest_.format_;
        const bool ArchiveHost = Format == PluginFormat::Wcx || Format == PluginFormat::SevenZip
            || Format == PluginFormat::UnEgg || Format == PluginFormat::DopusVfs;
        if (!ArchiveHost && Limits.memoryBytes_ < 64ULL * 1024 * 1024) return HostError("host-invalid-limits");
        HostProfile Profile; Profile.formats_ = {PluginFormat::Wcx, PluginFormat::Wlx, PluginFormat::Wfx, PluginFormat::Wdx, PluginFormat::DopusVfs, PluginFormat::DopusViewer, PluginFormat::SevenZip, PluginFormat::UnEgg};
        Profile.extensions_ = {Extension::ArchiveRead, Extension::ArchiveWrite, Extension::Viewer, Extension::StorageRead, Extension::StorageWrite, Extension::Metadata, Extension::ContentConvert}; Profile.supportsIsolation_ = true;
        auto Admission = Admit(Inspected.Value().manifest_, Approval, Profile); if (!Admission.HasValue()) return Admission.Failure();
        if (Limits.purpose_ == HostPurpose::Configuration && !Admission.Value().permissions_.contains(Permission::UserInterface))
            return HostError("host-configuration-ui-not-approved");
        const auto HostPath = QFileInfo(Executable).canonicalFilePath();
        if (HostPath.isEmpty() || !QFileInfo(HostPath).isFile()) return HostError("host-executable-missing");
        auto Token = RestrictedToken(); if (!Token.HasValue()) return Token.Failure();
        const auto PipeName = "\\\\.\\pipe\\hyperCommander-" + QUuid::createUuid().toString(QUuid::Id128);
        auto Pipe = ServerPipe(PipeName); if (!Pipe.HasValue()) return Pipe.Failure();
        auto Owner = std::unique_ptr<PluginProcess>(new PluginProcess(std::make_unique<State>())); auto& S = *Owner->state_;
        S.documentViewer_ = Inspected.Value().manifest_.id_ == "builtin.hyperviewer" && Inspected.Value().manifest_.compatibility_ == "hyperviewer-v1"
            && QFileInfo(Inspected.Value().binaryPath_).canonicalFilePath().compare(
                QFileInfo(QCoreApplication::applicationDirPath() + '/' + hc::layout::HyperViewerPackage + "/HyperViewer.dll").canonicalFilePath(), Qt::CaseInsensitive) == 0;
        S.helpers_ = !Inspected.Value().manifest_.helpers_.empty() || S.documentViewer_;
        if (!Inspected.Value().helperPaths_.empty()) S.helperPath_ = Inspected.Value().helperPaths_.front();
        // The reviewed built-in viewer lets Qt and the OS decide whether image
        // allocations fit. Third-party plugins retain their fixed Job quota.
        if (Inspected.Value().manifest_.id_ == "builtin.hyperviewer" && Inspected.Value().manifest_.compatibility_ == "hyperviewer-v1")
            {
            const HostLimits Default;
            if (Limits.requestMs_ == Default.requestMs_) Limits.requestMs_ = document::HostExchangeTimeoutMs + 5000;
            if (Limits.startupMs_ == Default.startupMs_) Limits.startupMs_ = 15000;
        }
        S.limits_ = Limits; S.pipe_ = std::move(Pipe.Value()); S.job_.Reset(CreateJobObjectW(nullptr, nullptr));
        S.viewer_ = Inspected.Value().manifest_.format_ == PluginFormat::Wlx || Inspected.Value().manifest_.format_ == PluginFormat::DopusViewer;
        if (!S.job_.Valid()) return HostError("host-job-create", GetLastError());
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Job{};
        Job.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        // The qualified RHWP renderer uses the same bounded process tree as
        // the viewer DLL. A default viewer requires only its plugin host.
#ifdef HC_RHWP_HOST_RUNTIME
        Job.BasicLimitInformation.ActiveProcessLimit = S.documentViewer_ ? 9 : S.helpers_ ? 3 : 1;
#else
        Job.BasicLimitInformation.ActiveProcessLimit = S.documentViewer_ ? 1 : S.helpers_ ? 3 : 1;
#endif
        if (!S.documentViewer_ && !ArchiveHost) {
            Job.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            Job.ProcessMemoryLimit = static_cast<SIZE_T>(Limits.memoryBytes_);
            if (S.helpers_) { Job.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY; Job.JobMemoryLimit = static_cast<SIZE_T>(Limits.memoryBytes_); }
        }
        if (!SetInformationJobObject(S.job_.Get(), JobObjectExtendedLimitInformation, &Job, sizeof(Job))) return HostError("host-job-limits", GetLastError());
        JOBOBJECT_BASIC_UI_RESTRICTIONS Ui{JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD
            | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_EXITWINDOWS};
        if (Limits.purpose_ == HostPurpose::Configuration) Ui.UIRestrictionsClass &= ~JOB_OBJECT_UILIMIT_HANDLES;
        if (!SetInformationJobObject(S.job_.Get(), JobObjectBasicUIRestrictions, &Ui, sizeof(Ui))) return HostError("host-job-ui", GetLastError());
        // An unprivileged host cannot OpenProcess on an elevated parent. Give
        // it only a lifetime/identity handle, through an explicit inheritance
        // list; never grant access to the parent's memory or termination rights.
        HANDLE ParentRaw = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &ParentRaw,
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) return HostError("host-parent-handle", GetLastError());
        WinHandle ParentMonitor(ParentRaw);
        auto Command = Quote(HostPath) + L" " + Quote(PipeName) + L" " + std::to_wstring(GetCurrentProcessId())
            + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(ParentMonitor.Get()));
        // Native nonvisual host receives no application secrets or Qt/plugin path
        // overrides. Its DLL search policy is configured before loading a plugin.
        // Children such as the HyperViewer workspace inherit this block.
        auto Variables = hc::win::RestrictedEnvironment(qEnvironmentVariable("SystemRoot") + "/System32");
        if (S.helpers_ && Inspected.Value().manifest_.compatibility_ == "hyperviewer-v1")
            Variables << "HC_DOCUMENT_CONFIG_DIR=" + hc::settings::DocumentPaths().directory_;
        QString HelperIni;
        if (S.helpers_ && Inspected.Value().manifest_.compatibility_ == "total7zip")
        {
            QString Directory;
            const bool Persistent = !DefaultIniPath.isEmpty();
            if (Persistent) {
                if (!QDir::isAbsolutePath(DefaultIniPath) || DefaultIniPath.contains(QChar::Null)) return HostError("host-helper-config-path");
                HelperIni = DefaultIniPath; Directory = QFileInfo(HelperIni).absolutePath();
                if (!QDir().mkpath(Directory)) return HostError("host-helper-config-directory");
            } else {
                S.helperConfig_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/hc-total7zip-XXXXXX");
                if (!S.helperConfig_->isValid()) return HostError("host-helper-config-directory");
                Directory = S.helperConfig_->path(); HelperIni = S.helperConfig_->filePath("wincmd.ini");
            }
            if (!QFileInfo::exists(HelperIni)) {
                QFile Ini(HelperIni); if (!Ini.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return HostError("host-helper-config-write");
                if (Ini.write("[Configuration]\r\nUseIniInProgramDir=0\r\n") < 0) return HostError("host-helper-config-write");
            }
            const auto ConfigPath = QDir(Directory).filePath("Total7zip.xml");
            const auto ExistingConfig = QDir(QFileInfo(Inspected.Value().binaryPath_).absolutePath()).filePath("Total7zip.xml");
            // An existing plugin configuration is opaque. Copy it once without
            // interpreting it; subsequent configuration dialogs own this file.
            if (Persistent && !QFileInfo::exists(ConfigPath) && QFileInfo::exists(ExistingConfig)
                && !QFile::copy(ExistingConfig, ConfigPath)) return HostError("host-helper-config-copy");
            if (!QFileInfo::exists(ConfigPath)) {
            QFile Config(ConfigPath);
            if (!Config.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return HostError("host-helper-config-write");
            QXmlStreamWriter Xml(&Config); Xml.writeStartDocument(); Xml.writeStartElement("settings");
            Xml.writeEmptyElement("path64bit");
            Xml.writeAttribute("path_7zG_exe", QDir::toNativeSeparators(Inspected.Value().helperPaths_.front()));
            Xml.writeAttribute("path_7z_dll", QDir::toNativeSeparators(QDir(QFileInfo(ManifestPath).absolutePath()).filePath("64/7z.dll")));
            Xml.writeEmptyElement("compression"); Xml.writeAttribute("save", Persistent ? "1" : "0"); Xml.writeAttribute("alwaysWait7zip", "1");
            Xml.writeAttribute("deleteToRecycleBin", "0"); Xml.writeAttribute("askByContent", "0");
            Xml.writeEmptyElement("passwords"); Xml.writeAttribute("save", "0");
            Xml.writeEndElement(); Xml.writeEndDocument();
            if (Xml.hasError() || !Config.flush()) return HostError("host-helper-config-write");
            }
            Variables << "COMMANDER_INI=" + QDir::toNativeSeparators(HelperIni)
                << "COMMANDER_PATH=" + QDir::toNativeSeparators(Directory);
        }
        Variables.sort(Qt::CaseInsensitive); std::wstring Environment;
        for (const auto& Variable : Variables) { Environment += Variable.toStdWString(); Environment += L'\0'; } Environment += L'\0';
        STARTUPINFOEXW Startup{}; Startup.StartupInfo.cb = sizeof(Startup);
        Startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW; Startup.StartupInfo.wShowWindow = SW_HIDE;
        SIZE_T AttributeBytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeBytes);
        std::vector<std::byte> AttributeStorage(AttributeBytes);
        Startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.data());
        if (!InitializeProcThreadAttributeList(Startup.lpAttributeList, 1, 0, &AttributeBytes)) return HostError("host-startup-attributes", GetLastError());
        const auto DeleteAttributes = qScopeGuard([&] { DeleteProcThreadAttributeList(Startup.lpAttributeList); });
        if (!UpdateProcThreadAttribute(Startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &ParentRaw, sizeof(ParentRaw), nullptr, nullptr))
            return HostError("host-inherit-parent", GetLastError());
        // Windows applies STARTF_USESHOWWINDOW to the first native dialog too.
        // An approved interactive WCX must not wait on an invisible password
        // prompt. Keep the console hidden and preserve all Job UI restrictions.
        if ((Inspected.Value().manifest_.format_ == PluginFormat::Wcx || Limits.purpose_ == HostPurpose::Configuration)
            && Inspected.Value().manifest_.permissions_.contains(Permission::UserInterface)) Startup.StartupInfo.wShowWindow = SW_SHOWNOACTIVATE;
        PROCESS_INFORMATION Process{};
        // An elevated token's default DACL can grant only Administrators and
        // SYSTEM. Administrators is deny-only in the host token: its loader
        // must still be able to open its own process/thread during DLL startup.
        // Grant those two new objects to the actual user, without restoring any
        // disabled administrator group or removed privilege.
        DWORD UserSize = 0; GetTokenInformation(Token.Value().Get(), TokenUser, nullptr, 0, &UserSize);
        std::vector<std::byte> UserData(UserSize);
        if (!GetTokenInformation(Token.Value().Get(), TokenUser, UserData.data(), UserSize, &UserSize)) return HostError("host-process-user", GetLastError());
        LPWSTR UserSid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(UserData.data())->User.Sid, &UserSid)) return HostError("host-process-sid", GetLastError());
        const auto ProcessSddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(UserSid) + L")"; LocalFree(UserSid);
        PSECURITY_DESCRIPTOR ProcessDescriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(ProcessSddl.c_str(), SDDL_REVISION_1, &ProcessDescriptor, nullptr)) return HostError("host-process-security", GetLastError());
        const auto FreeDescriptor = qScopeGuard([&] { LocalFree(ProcessDescriptor); });
        // Loader-created objects must use the same user-accessible DACL too;
        // changing only the initial process/thread does not cover DLL startup.
        BOOL DaclPresent = FALSE, DaclDefaulted = FALSE; PACL DefaultAcl = nullptr;
        if (!GetSecurityDescriptorDacl(ProcessDescriptor, &DaclPresent, &DefaultAcl, &DaclDefaulted) || !DaclPresent)
            return HostError("host-token-default-acl", GetLastError());
        TOKEN_DEFAULT_DACL DefaultDacl{DefaultAcl};
        if (!SetTokenInformation(Token.Value().Get(), TokenDefaultDacl, &DefaultDacl, sizeof(DefaultDacl)))
            return HostError("host-token-default-acl", GetLastError());
        SECURITY_ATTRIBUTES ProcessSecurity{sizeof(ProcessSecurity), ProcessDescriptor, FALSE};
        if (!CreateProcessAsUserW(Token.Value().Get(), reinterpret_cast<LPCWSTR>(HostPath.utf16()), Command.data(), &ProcessSecurity, &ProcessSecurity, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, Environment.data(), reinterpret_cast<LPCWSTR>(QFileInfo(HostPath).absolutePath().utf16()), &Startup.StartupInfo, &Process))
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
        if (Limits.purpose_ == HostPurpose::Configuration) Declaration["configuration"] = true;
        {
            const auto Ini = !HelperIni.isEmpty() ? HelperIni : DefaultIniPath.isEmpty()
                ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath(S.viewer_ ? "lsplugin.ini" : Inspected.Value().manifest_.format_ == PluginFormat::Wcx ? "pkplugin.ini" : "tc-data.ini") : DefaultIniPath;
            if (!QDir::isAbsolutePath(Ini) || Ini.contains(QChar::Null)) return HostError("host-wlx-config-path");
            Declaration.insert("defaultIni", QDir::cleanPath(Ini));
        }
        const auto Hello = QJsonDocument(Declaration).toJson(QJsonDocument::Compact);
        auto Ready = S.call(WireKind::Hello, Hello, Context); if (!Ready.HasValue()) return Ready.Failure();
        if (!Ready.Value()["inJob"].toBool() || !Ready.Value()["adminDenied"].toBool() || !Ready.Value()["privilegesRemoved"].toBool()) return HostError("host-restriction-verification");
        S.ready_ = true;
        S.archiveOptionsVersion_ = unsigned(Ready.Value()["archiveOptionsVersion"].toInt(0));
        if (Limits.enableSessionV2_ && !S.viewer_ && Ready.Value()["sessionProtocol"].toInt() == 2)
        {
            S.epoch_ = Ready.Value()["sessionEpoch"].toString();
            if (S.epoch_.size() != 32) return HostError("host-session-negotiation");
            S.sessionState_ = ProviderSessionState::Ready;
        }
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
        if (!IsArchiveCommand(Kind)) return HostError("host-archive-command");
        std::lock_guard Lock(state_->mutex_); return state_->call(Kind, QJsonDocument(Request).toJson(QJsonDocument::Compact), Context);
    }
    Result<QJsonObject> PluginProcess::ViewerCall(WireKind Kind, QJsonObject Request, OperationContext Context)
    {
        if (!IsViewerCommand(Kind)) return HostError("host-viewer-command");
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
            if (S.documentViewer_) {
                // The embedded Qt window needs its native activation root. The
                // user-authorized exception grants only this caller-owned root;
                // every other desktop window remains behind HANDLES restrictions.
                const auto Root = GetAncestor(Parent, GA_ROOT); DWORD RootOwner = 0;
                if (Root && Root != Parent && GetWindowThreadProcessId(Root, &RootOwner) && RootOwner == GetCurrentProcessId()) {
                    if (!UserHandleGrantAccess(Root, S.job_.Get(), TRUE)) {
                        const auto Failure = HostError("host-viewer-root-grant", GetLastError());
                        UserHandleGrantAccess(Parent, S.job_.Get(), FALSE); S.viewerParent_ = nullptr; return Failure;
                    }
                    S.viewerRoot_ = Root;
                }
            }
        }
        auto Result = S.call(Kind, QJsonDocument(Request).toJson(QJsonDocument::Compact), Context);
        if ((Kind == WireKind::ViewerOpen && !Result.HasValue()) || (Kind == WireKind::ViewerClose && Result.HasValue()))
        {
            if (S.viewerRoot_) UserHandleGrantAccess(S.viewerRoot_, S.job_.Get(), FALSE);
            S.viewerRoot_ = nullptr;
            if (S.viewerParent_) UserHandleGrantAccess(S.viewerParent_, S.job_.Get(), FALSE);
            S.viewerParent_ = nullptr;
        }
        return Result;
    }
    Result<QJsonObject> PluginProcess::DataCall(WireKind Kind, QJsonObject Request, OperationContext Context)
    {
        if (!IsDataCommand(Kind)) return HostError("host-data-command");
        if (const auto Stop = Context.Check({"hosted-data"})) return *Stop;
        std::lock_guard Lock(state_->mutex_); return state_->call(Kind, QJsonDocument(Request).toJson(QJsonDocument::Compact), Context);
    }
    Result<QJsonObject> PluginProcess::Configure(std::uint64_t Owner, OperationContext Context)
    {
        if (state_->limits_.purpose_ != HostPurpose::Configuration) return HostError("host-configuration-session-required");
        std::lock_guard Lock(state_->mutex_);
        return state_->call(WireKind::PluginConfigure, QJsonDocument(QJsonObject{{"owner", QString::number(Owner)}}).toJson(QJsonDocument::Compact), Context);
    }
    unsigned PluginProcess::ArchiveOptionsVersion() const { return state_->archiveOptionsVersion_; }
    HostSnapshot PluginProcess::Snapshot() const
    {
        std::lock_guard Lock(state_->mutex_); HostSnapshot Snapshot; Snapshot.processId_ = state_->pid_; Snapshot.failure_ = state_->failure_; Snapshot.forcedTermination_ = state_->forcedTermination_;
        if (state_->process_.Valid()) { DWORD Exit = 0; if (GetExitCodeProcess(state_->process_.Get(), &Exit)) { Snapshot.running_ = WaitForSingleObject(state_->process_.Get(), 0) == WAIT_TIMEOUT; if (!Snapshot.running_) Snapshot.exitCode_ = Exit; } }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
        if (state_->job_.Valid() && QueryInformationJobObject(state_->job_.Get(), JobObjectBasicAccountingInformation, &Accounting, sizeof(Accounting), nullptr))
        { Snapshot.activeProcesses_ = Accounting.ActiveProcesses; Snapshot.totalProcesses_ = Accounting.TotalProcesses; }
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
        if (S.helpers_ && S.job_.Valid())
        {
            TerminateJobObject(S.job_.Get(), 0);
            const auto Until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            for (;;)
            {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
                if (!QueryInformationJobObject(S.job_.Get(), JobObjectBasicAccountingInformation, &Accounting, sizeof(Accounting), nullptr)
                    || !Accounting.ActiveProcesses || std::chrono::steady_clock::now() >= Until) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (S.viewerRoot_) UserHandleGrantAccess(S.viewerRoot_, S.job_.Get(), FALSE);
        S.viewerRoot_ = nullptr;
        if (S.viewerParent_) UserHandleGrantAccess(S.viewerParent_, S.job_.Get(), FALSE);
        S.viewerParent_ = nullptr;
        S.closed_ = true; S.sessionState_ = ProviderSessionState::Closed; S.pipe_.Reset(); S.job_.Reset();
    }
    ProviderSessionSnapshot PluginProcess::Session() const
    { return {state_->epoch_.toStdString(), state_->sessionState_.load()}; }
    Result<QJsonObject> PluginProcess::SessionCall(WireKind Kind, QJsonObject Request, OperationContext Context, QByteArray Binary)
    {
        if (Kind != WireKind::StreamOpen && Kind != WireKind::StreamSeek && Kind != WireKind::StreamClose && Kind != WireKind::ArchiveUpdateBinary)
            return HostError("host-session-command");
        std::lock_guard Lock(state_->mutex_);
        if (state_->epoch_.isEmpty()) return Error{ErrorCategory::Unsupported, "session-v2-required"};
        return state_->call(Kind, QJsonDocument(Request).toJson(QJsonDocument::Compact), Context, Binary);
    }
    Result<ReadChunk> PluginProcess::ReadStream(const QString& Token, std::size_t Count, OperationContext Context)
    {
        if (!Count || !Context.maxReadBytes_) return HostError("host-stream-read-size");
        std::lock_guard Lock(state_->mutex_);
        if (state_->epoch_.isEmpty()) return Error{ErrorCategory::Unsupported, "session-v2-required"};
        const auto Wanted = std::min({Count, Context.maxReadBytes_, std::size_t(WirePayloadLimit / 2)});
        auto Read = state_->call(WireKind::StreamRead, QJsonDocument(QJsonObject{{"stream", Token}, {"count", QString::number(Wanted)}}).toJson(QJsonDocument::Compact), Context);
        if (!Read.HasValue()) return Read.Failure();
        if (state_->streamReply_.bytes_.size() > Wanted) { state_->fail(HostError("host-stream-read-size")); return *state_->failure_; }
        return std::exchange(state_->streamReply_, {});
    }
    Result<ReadChunk> PluginProcess::ReadArchiveStream(std::uint64_t Stream, std::uint32_t Ordinal, std::size_t Count, OperationContext Context)
    {
        if (!Count || !Context.maxReadBytes_) return HostError("host-stream-read-size");
        std::lock_guard Lock(state_->mutex_);
        if (state_->epoch_.isEmpty()) return Error{ErrorCategory::Unsupported,"session-v2-required"};
        const auto Wanted=std::min({Count,Context.maxReadBytes_,std::size_t(WirePayloadLimit-1)});
        auto Read=state_->call(WireKind::StreamRead,QJsonDocument(QJsonObject{{"archiveStream",QString::number(Stream)},
            {"ordinal",QString::number(Ordinal)},{"count",QString::number(Wanted)}}).toJson(QJsonDocument::Compact),Context);
        if (!Read.HasValue()) return Read.Failure();
        if (state_->streamReply_.bytes_.size()>Wanted || (state_->streamReply_.bytes_.empty() && !state_->streamReply_.eof_)) {
            state_->fail(HostError("host-stream-read-size")); return *state_->failure_;
        }
        return std::exchange(state_->streamReply_,{});
    }
    Result<QJsonObject> PluginProcess::ArchiveData(QJsonObject Request, const QByteArray& Bytes, OperationContext Context)
    {
        if (!Session().epoch_.empty()) return SessionCall(WireKind::ArchiveUpdateBinary, std::move(Request), Context, Bytes);
        Request["data"] = QString::fromLatin1(Bytes.toBase64()); Request["context"] = EncodeContext(Context);
        return ArchiveCall(WireKind::ArchiveUpdateData, std::move(Request), Context);
    }
}
