#include "pipeChannel.hpp"
#include "manifestFile.hpp"
#include "wcxHostService.hpp"
#include "HyperCommanderBuiltInPlugin/SevenZip/sevenZipBackend.hpp"
#include "HyperCommanderBuiltInPlugin/AlzEgg/unEggBackend.hpp"
#include "nativeViewerHostService.hpp"
#include "tcDataHostService.hpp"
#include "dopusVfsHostService.hpp"
#include "hostSession.hpp"
#include <QUuid>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <thread>
#include <QWidget>
#include <QEventLoop>

namespace
{
    using namespace hc::plugins;
    using namespace hc::storage;
    OperationContext Deadline(unsigned Milliseconds)
    { OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(Milliseconds); return Context; }
    QJsonObject Restrictions()
    {
        BOOL InJob = FALSE; IsProcessInJob(GetCurrentProcess(), nullptr, &InJob);
        BYTE Admin[SECURITY_MAX_SID_SIZE]; DWORD Size = sizeof(Admin); BOOL AdminEnabled = TRUE;
        if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, Admin, &Size)) CheckTokenMembership(nullptr, Admin, &AdminEnabled);
        HANDLE Raw = nullptr; bool PrivilegesRemoved = false;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Raw))
        {
            WinHandle Token(Raw); Size = 0; GetTokenInformation(Token.Get(), TokenPrivileges, nullptr, 0, &Size);
            std::vector<std::byte> Buffer(Size); LUID Notify{};
            if (LookupPrivilegeValueW(nullptr, SE_CHANGE_NOTIFY_NAME, &Notify) && GetTokenInformation(Token.Get(), TokenPrivileges, Buffer.data(), Size, &Size))
            {
                PrivilegesRemoved = true; const auto* Privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(Buffer.data());
                for (DWORD Index = 0; Index < Privileges->PrivilegeCount; ++Index)
                { const auto Id = Privileges->Privileges[Index].Luid; if (Id.LowPart != Notify.LowPart || Id.HighPart != Notify.HighPart) PrivilegesRemoved = false; }
            }
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
        const bool Known = QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits), nullptr);
        return {{"inJob", bool(InJob)}, {"adminDenied", !bool(AdminEnabled)}, {"privilegesRemoved", PrivilegesRemoved}, {"processId", qint64(GetCurrentProcessId())},
            {"jobLimitsKnown", Known}, {"jobLimitFlags", qint64(Limits.BasicLimitInformation.LimitFlags)}};
    }
    class ConfigureNotify final : public QWidget {
    public:
        bool reinitialize_ = false;
    protected:
        bool nativeEvent(const QByteArray&, void* Message, qintptr*) override {
            if (static_cast<MSG*>(Message)->message == WM_APP + 0xfff) reinitialize_ = true;
            return false;
        }
    };
    struct LoadedPlugin
    {
        std::vector<WinHandle> locks_;
        std::unique_ptr<WcxHostService> archives_;
        std::unique_ptr<NativeViewerHostService> viewer_;
        std::unique_ptr<TcDataHostService> data_;
        std::unique_ptr<DopusVfsHostService> dopusVfs_;
        std::vector<HMODULE> dependencies_;
        HMODULE module_ = nullptr;
        std::map<QString, std::shared_ptr<IReadStream>> streams_;
        bool configuration_ = false;
        DWORD parentProcess_ = 0;
        PluginManifest manifest_;
        ~LoadedPlugin() { Close(); }
        void Close()
        {
            streams_.clear(); dopusVfs_.reset(); data_.reset(); viewer_.reset(); archives_.reset(); if (module_) FreeLibrary(module_);
            for (auto At = dependencies_.rbegin(); At != dependencies_.rend(); ++At) FreeLibrary(*At);
            module_ = nullptr; dependencies_.clear(); locks_.clear();
        }
        Result<Unit> Open(const QJsonObject& Hello, DWORD ParentProcess)
        {
            const bool HasIni = Hello.contains("defaultIni");
            const bool Configuration = Hello.contains("configuration");
            if ((Configuration && Hello["configuration"] != QJsonValue(true)) || Hello.size() != (HasIni ? 3 : 2) + Configuration || !Hello["manifestPath"].isString() || !Hello["manifest"].isObject()
                || (HasIni && !Hello["defaultIni"].isString())) return HostError("host-hello-fields");
            const auto Path = Hello["manifestPath"].toString();
            auto Expected = ParseManifest(QJsonDocument(Hello["manifest"].toObject()).toJson()); if (!Expected.HasValue()) return Expected.Failure();
            configuration_ = Configuration; parentProcess_ = ParentProcess; manifest_ = Expected.Value();
            if (configuration_ && !manifest_.permissions_.contains(Permission::UserInterface)) return HostError("host-configuration-ui-not-approved");
            if (Expected.Value().architecture_ != Architecture::X64
                || Expected.Value().execution_ != ExecutionMode::Isolated
                || (Expected.Value().format_ == PluginFormat::Wcx && !Expected.Value().extensions_.contains(Extension::ArchiveRead))
                || Expected.Value().protocol_.major_ != 1 || Expected.Value().protocol_.minimumMinor_ != 0) return HostError("host-unsupported-manifest");
            auto Inspected = InspectManifestFile(Path, Deadline(5000)); if (!Inspected.HasValue()) return Inspected.Failure();
            if (Inspected.Value().manifest_ != Expected.Value()) return HostError("host-manifest-changed");
            const auto Binary = Inspected.Value().binaryPath_;
            // Deny replacement of the file and its ancestor directory names until
            // unload. Recheck while these handles are held, before any DllMain.
            auto PackageFiles = Inspected.Value().dependencyPaths_; PackageFiles.append(Inspected.Value().helperPaths_); PackageFiles.prepend(Binary);
            for (const auto& PackageFile : PackageFiles)
            {
            auto Directory = QFileInfo(PackageFile).absolutePath();
            for (;;)
            {
                WinHandle Lock(CreateFileW(reinterpret_cast<LPCWSTR>(Directory.utf16()), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (!Lock.Valid()) return HostError("host-package-lock", GetLastError()); locks_.push_back(std::move(Lock));
                const auto Parent = QFileInfo(Directory).absolutePath(); if (Parent == Directory) break; Directory = Parent;
            }
            WinHandle File(CreateFileW(reinterpret_cast<LPCWSTR>(PackageFile.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!File.Valid()) return HostError("host-binary-lock", GetLastError()); locks_.push_back(std::move(File));
            }
            Inspected = InspectManifestFile(Path, Deadline(5000)); if (!Inspected.HasValue()) return Inspected.Failure();
            if (Inspected.Value().manifest_ != Expected.Value() || Inspected.Value().binaryPath_ != Binary) return HostError("host-manifest-changed");
            auto RecheckedFiles = Inspected.Value().dependencyPaths_; RecheckedFiles.append(Inspected.Value().helperPaths_); RecheckedFiles.prepend(Binary);
            if (RecheckedFiles != PackageFiles) return HostError("host-dependency-changed");
            // Resolve package imports only through pinned, already loaded DLLs.
            // Never search the writable package directory during loader lookup.
            // Repeated passes permit an unordered acyclic dependency declaration.
            auto Pending = Inspected.Value().dependencyPaths_;
            if (Expected.Value().compatibility_ == "total7zip") {
                const auto Companion = QDir(QFileInfo(Binary).absolutePath()).filePath("7z.dll");
                for (qsizetype I = Pending.size(); I-- > 0;) if (Pending[I].compare(Companion, Qt::CaseInsensitive) == 0) {
                    QFile File(Pending[I]); if (!File.open(QIODevice::ReadOnly)) return HostError("host-dependency-open"); const auto Header = File.read(1024 * 1024);
                    const auto Arch = InspectPe(std::span(reinterpret_cast<const std::byte*>(Header.constData()), size_t(Header.size())));
                    // The original dual-architecture package also ships an x86
                    // codec. Keep its reviewed file lease, but never load it.
                    if (Arch.HasValue() && Arch.Value() == Architecture::X86) Pending.removeAt(I);
                }
            }
            // HyperViewer now owns its Qt workspace in this process. Load the
            // reviewed dependencies before loading the SDK DLL.
            while (!Pending.isEmpty())
            {
                const auto Before = Pending.size();
                for (qsizetype Index = 0; Index < Pending.size();)
                {
                    const auto Dependency = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Pending[Index].utf16()), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                    if (!Dependency) { ++Index; continue; }
                    dependencies_.push_back(Dependency); Pending.removeAt(Index);
                }
                if (Pending.size() == Before) return HostError("host-dependency-load", GetLastError());
            }
            module_ = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Binary.utf16()), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module_) return HostError("host-plugin-load", GetLastError());
            if (Expected.Value().format_ == PluginFormat::Wlx || Expected.Value().format_ == PluginFormat::DopusViewer)
            {
                viewer_ = std::make_unique<NativeViewerHostService>(module_, ParentProcess, Expected.Value());
                auto Initialized = viewer_->Initialize(Hello["defaultIni"].toString());
                if (!Initialized.HasValue()) return Initialized.Failure();
            }
            else if (Expected.Value().format_ == PluginFormat::UnEgg)
            {
                archives_ = std::make_unique<WcxHostService>(MakeUnEggBackend(module_));
            }
            else if (Expected.Value().format_ == PluginFormat::SevenZip)
            {
                archives_ = std::make_unique<WcxHostService>(MakeSevenZipBackend(module_), ArchiveWritePolicy(Expected.Value()).HasValue());
            }
            else if (Expected.Value().format_ == PluginFormat::Wcx)
            {
                struct Defaults { int size_; DWORD low_, high_; char ini_[MAX_PATH]; };
                const auto Set = reinterpret_cast<void(__stdcall*)(Defaults*)>(GetProcAddress(module_, "PackSetDefaultParams"));
                if (Set)
                {
                    const auto Ini = QDir::toNativeSeparators(Hello["defaultIni"].toString()); const auto Bytes = Ini.toLocal8Bit();
                    if (!QDir::isAbsolutePath(Ini) || Bytes.size() >= MAX_PATH || QString::fromLocal8Bit(Bytes) != Ini) return HostError("host-wcx-config-encoding");
                    if (!QDir().mkpath(QFileInfo(Ini).absolutePath())) return HostError("host-wcx-config-directory");
                    Defaults Parameters{}; Parameters.size_ = sizeof(Parameters); Parameters.high_ = 2; Parameters.low_ = 21;
                    std::copy(Bytes.begin(), Bytes.end(), Parameters.ini_); Set(&Parameters);
                }
                auto WritePolicy = ArchiveWritePolicy(Expected.Value());
                archives_ = std::make_unique<WcxHostService>(Binary.toStdString(), WritePolicy.HasValue(), WritePolicy.HasValue() ? "" : WritePolicy.Failure().code_);
            }
            else if (Expected.Value().format_ == PluginFormat::DopusVfs)
            {
                dopusVfs_ = std::make_unique<DopusVfsHostService>(module_, Expected.Value());
                auto Initialized = dopusVfs_->Initialize(); if (!Initialized.HasValue()) return Initialized.Failure();
            }
            else
            {
                data_ = std::make_unique<TcDataHostService>(module_, Expected.Value());
                auto Initialized = data_->Initialize(Hello["defaultIni"].toString()); if (!Initialized.HasValue()) return Initialized.Failure();
            }
            return Unit{};
        }
        Result<QJsonObject> Probe()
        {
            if (!module_) return HostError("host-plugin-not-loaded");
            if (viewer_) return viewer_->Probe();
            if (data_) return data_->Probe();
            if (dopusVfs_) return dopusVfs_->Probe();
            if (GetProcAddress(module_, "EGG_CreateEgg"))
            {
                for (const auto Name : {"EGG_CreateEgg", "EGG_DestroyEgg", "EGG_IsValidArchive", "EGG_OpenArchive", "EGG_CloseArchive",
                    "EGG_Extract", "EGG_Test", "EGG_GetFileCount", "EGG_GetFileHeader"})
                    if (!GetProcAddress(module_, Name)) return HostError("host-unegg-exports");
                return QJsonObject{{"format", "unegg"}, {"unicode", true}, {"caps", 0}, {"processId", qint64(GetCurrentProcessId())}};
            }
            if (GetProcAddress(module_, "GetNumberOfFormats") && GetProcAddress(module_, "CreateObject"))
                return QJsonObject{{"format", "7zip"}, {"unicode", true}, {"caps", 0}, {"processId", qint64(GetCurrentProcessId())}};
            for (const auto Name : {"OpenArchiveW", "ReadHeaderExW", "ProcessFileW", "CloseArchive"})
                if (!GetProcAddress(module_, Name)) return HostError("host-wcx-unicode-exports");
            const auto Caps = reinterpret_cast<int(__stdcall*)()>(GetProcAddress(module_, "GetPackerCaps"));
            // Deliberately invoke the plugin only in this job-owned process.
            return QJsonObject{{"format", "wcx"}, {"unicode", true}, {"caps", Caps ? Caps() : 0}, {"configure", bool(GetProcAddress(module_, "ConfigurePacker"))}, {"processId", qint64(GetCurrentProcessId())}};
        }
        Result<QJsonObject> Configure(const QJsonObject& Request)
        {
            if (!configuration_ || !manifest_.permissions_.contains(Permission::UserInterface)) return HostError("host-configuration-session-required");
            auto Value = WireUnsigned(Request["owner"]); if (Request.size() != 1 || !Value.HasValue()) return HostError("host-configuration-owner");
            const auto Owner = reinterpret_cast<HWND>(std::uintptr_t(Value.Value())); DWORD Process = 0;
            if (!IsWindow(Owner) || !GetWindowThreadProcessId(Owner, &Process) || Process != parentProcess_) return HostError("host-configuration-owner");
            ConfigureNotify Notify; HWND Window = nullptr;
            if (manifest_.format_ == PluginFormat::Wcx) {
                const auto Call = reinterpret_cast<void(__stdcall*)(HWND, HINSTANCE)>(GetProcAddress(module_, "ConfigurePacker"));
                if (!Call) return HostError("host-configuration-unavailable"); Call(Owner, module_);
            } else if (manifest_.format_ == PluginFormat::DopusVfs && dopusVfs_ && dopusVfs_->CanConfigure()) {
                const auto Call = reinterpret_cast<HWND(*)(HWND, HWND, DWORD)>(GetProcAddress(module_, "VFS_Configure"));
                Window = Call(Owner, reinterpret_cast<HWND>(Notify.winId()), 1);
            } else return HostError("host-configuration-unavailable");
            if (Window && IsWindow(Window)) {
                DWORD Process = 0; if (!GetWindowThreadProcessId(Window, &Process) || Process != GetCurrentProcessId()) return HostError("host-configuration-window");
                ShowWindow(Window, SW_SHOWNORMAL); SetForegroundWindow(Window);
                while (IsWindow(Window)) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    if (!IsWindow(Owner)) { PostMessageW(Window, WM_CLOSE, 0, 0); break; }
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
                }
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            return QJsonObject{{"configured", true}, {"reinitialize", Notify.reinitialize_}};
        }
        Result<QByteArray> Dispatch(WireKind Kind, const QJsonObject& Request, const QByteArray& Binary, OperationContext Context)
        {
            const auto Json = [](const Result<QJsonObject>& Value) -> Result<QByteArray> {
                if (!Value.HasValue()) return Value.Failure(); return QJsonDocument(Value.Value()).toJson(QJsonDocument::Compact);
            };
            if (const auto Stop = Context.Check({"plugin-host"})) return *Stop;
            if (Kind == WireKind::ArchiveUpdateBinary && archives_) return Json(archives_->AppendBinary(Request, Binary, Context));
            if (!Binary.isEmpty()) return HostError("session-unexpected-binary");
            if (IsArchiveCommand(Kind) && archives_) return Json(archives_->Handle(Kind, Request, {}, Context));
            if (IsDataCommand(Kind) && (data_ || dopusVfs_)) return Json(dopusVfs_ ? dopusVfs_->Handle(Kind, Request, Context) : data_->Handle(Kind, Request));
            if (Kind==WireKind::StreamRead && Request.contains("archiveStream") && archives_) {
                auto Chunk=archives_->ReadStream(Request,Context); if (!Chunk.HasValue()) return Chunk.Failure();
                if (Chunk.Value().bytes_.size()>WirePayloadLimit-1 || (Chunk.Value().bytes_.empty() && !Chunk.Value().eof_)) return HostError("host-archive-stream-size");
                QByteArray Bytes(1,Chunk.Value().eof_?'\1':'\0'); Bytes.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()),Chunk.Value().bytes_.size()); return Bytes;
            }
            if (Kind == WireKind::StreamOpen)
            {
                if (!dopusVfs_) return Error{ErrorCategory::Unsupported, "host-stream-unavailable"};
                if (streams_.size() >= 64) return Error{ErrorCategory::BudgetExceeded, "host-active-streams", {}, {}, true, "close-streams"};
                auto Stream = dopusVfs_->OpenStream(Request, Context); if (!Stream.HasValue()) return Stream.Failure();
                const auto Token = QUuid::createUuid().toString(QUuid::Id128); streams_.emplace(Token, Stream.Value());
                return Json(QJsonObject{{"stream", Token}, {"seek", Stream.Value()->CanSeek()}, {"access", int(Stream.Value()->Access())}});
            }
            const auto Token = Request["stream"].toString(); const auto It = streams_.find(Token);
            if (It == streams_.end()) return Error{ErrorCategory::StaleRevision, "host-stream-token"};
            if (Kind == WireKind::StreamClose && Request.size() == 1) { streams_.erase(It); return Json(QJsonObject{{"closed", true}}); }
            if (Kind == WireKind::StreamRead && Request.size() == 2)
            {
                auto Count = WireUnsigned(Request["count"]); if (!Count.HasValue() || !Count.Value() || Count.Value() > WirePayloadLimit / 2) return HostError("host-stream-read-size");
                auto Chunk = It->second->Read(std::size_t(Count.Value()), Context).get(); if (!Chunk.HasValue()) return Chunk.Failure();
                if (Chunk.Value().bytes_.size() > Count.Value() || (Chunk.Value().bytes_.empty() && !Chunk.Value().eof_)) return HostError("host-stream-no-progress");
                QByteArray Bytes(1, Chunk.Value().eof_ ? '\1' : '\0'); Bytes.append(reinterpret_cast<const char*>(Chunk.Value().bytes_.data()), Chunk.Value().bytes_.size()); return Bytes;
            }
            if (Kind == WireKind::StreamSeek && Request.size() == 2)
            {
                auto Offset = WireUnsigned(Request["offset"]); if (!Offset.HasValue()) return Offset.Failure();
                auto Moved = It->second->Seek(Offset.Value(), Context).get(); if (!Moved.HasValue()) return Moved.Failure(); return Json(QJsonObject{});
            }
            return Error{ErrorCategory::Unsupported, "host-session-command"};
        }
    };
}
int main(int Argc, char** Argv)
{
    static_assert(sizeof(void*) == 8, "hc-plugin-host-x64 requires an x64 build");
    using namespace hc::plugins;
    using namespace hc::storage;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR)) return 2;
    QApplication App(Argc, Argv);
    App.setQuitOnLastWindowClosed(false);
    if ((App.arguments().size() != 3 && App.arguments().size() != 4) || !App.arguments()[1].startsWith("\\\\.\\pipe\\hyperCommander-")) return 2;
    bool Parsed = false; const auto ParentId = App.arguments()[2].toUInt(&Parsed); if (!Parsed || !ParentId) return 2;
    WinHandle Parent;
    if (App.arguments().size() == 4) {
        const auto Value = App.arguments()[3].toULongLong(&Parsed); if (!Parsed || !Value) return 3;
        Parent.Reset(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(Value)));
        if (!Parent.Valid() || GetProcessId(Parent.Get()) != ParentId) return 3;
        if (!SetHandleInformation(Parent.Get(), HANDLE_FLAG_INHERIT, 0)) return 3;
    } else Parent.Reset(OpenProcess(SYNCHRONIZE, FALSE, ParentId));
    if (!Parent.Valid()) return 3;
    const auto PipeName = App.arguments()[1];
    WinHandle Pipe(CreateFileW(reinterpret_cast<LPCWSTR>(PipeName.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!Pipe.Valid()) return 4;
    ULONG Server = 0; if (!GetNamedPipeServerProcessId(Pipe.Get(), &Server) || Server != ParentId) return 5;
    LoadedPlugin Plugin; std::unique_ptr<HostSession> Session; bool Ready = false, LastWasSession = false; std::uint64_t LastSequence = 0;
    const auto Execute = [&](std::function<Result<QJsonObject>()> Call) -> Result<QJsonObject> {
        if (!Session) return Call();
        auto Reply = Session->Invoke([&]() -> Result<QByteArray> {
            auto Value = Call(); if (!Value.HasValue()) return Value.Failure();
            return QJsonDocument(Value.Value()).toJson(QJsonDocument::Compact);
        });
        if (!Reply.HasValue()) return Reply.Failure();
        return QJsonDocument::fromJson(Reply.Value()).object();
    };
    // SDK thread-local settings and native handles share one lifetime for both
    // wire versions. Windowed viewers retain their main-thread message loop.
    const auto Cleanup = qScopeGuard([&] {
        if (Session) Session->Invoke([&]() -> Result<QByteArray> { Plugin.Close(); return QByteArray{}; });
    });
    for (;;)
    {
        // A viewer can spend arbitrarily long in a modal interaction. The
        // parent's liveness watchdog and peer handle govern its lifetime;
        // an idle receive deadline must not expire when that dialog returns.
        auto Frame = ReadFrame(Pipe.Get(), Parent.Get(), Plugin.viewer_ ? OperationContext{} : Deadline(120000), Plugin.viewer_ != nullptr); if (!Frame.HasValue()) return 6;
        const auto& Header = Frame.Value().header_;
        // Completion can cross an already-sent cancel/prompt reply on the
        // duplex pipe. Retire that control for the completed invocation; it
        // must neither cancel the next invocation nor poison its sequence.
        if (LastWasSession && Header.version_ == ProtocolVersion{2, 0} && Header.sequence_ == LastSequence
            && (Header.kind_ == WireKind::InteractionReply || (Header.kind_ == WireKind::SessionCancel && Frame.Value().payload_.isEmpty())))
        { Frame.Value().payload_.fill(0); continue; }
        if (Header.sequence_ != LastSequence + 1) return 7; LastSequence = Header.sequence_;
        LastWasSession = false;
        if (Header.version_ == ProtocolVersion{2, 0})
        {
            if (!Ready || !Session || Header.kind_ != WireKind::SessionInvoke) return 7;
            if (!Session->Run(Frame.Value(), Pipe.Get(), Parent.Get()).HasValue()) return 10;
            LastWasSession = true;
            continue;
        }
        Result<QJsonObject> Result = HostError("host-command-unavailable");
        if (!Ready && Header.kind_ == WireKind::Hello)
        {
            QJsonParseError Error; const auto Request = QJsonDocument::fromJson(Frame.Value().payload_, &Error);
            if (Error.error != QJsonParseError::NoError || !Request.isObject()) return 8;
            const auto Policy = Restrictions();
            if (!Policy["inJob"].toBool() || !Policy["adminDenied"].toBool() || !Policy["privilegesRemoved"].toBool()) return 9;
            auto Expected = ParseManifest(QJsonDocument(Request.object()["manifest"].toObject()).toJson());
            if (!Expected.HasValue()) return 8;
            if (!Request.object()["configuration"].toBool() && Expected.Value().format_ != PluginFormat::Wlx && Expected.Value().format_ != PluginFormat::DopusViewer)
                Session = std::make_unique<HostSession>([&](WireKind Kind, const QJsonObject& Request, const QByteArray& Binary, OperationContext Context) { return Plugin.Dispatch(Kind, Request, Binary, Context); });
            auto Opened = Execute([&]() -> hc::storage::Result<QJsonObject> {
                auto Value = Plugin.Open(Request.object(), ParentId); if (!Value.HasValue()) return Value.Failure(); return QJsonObject{};
            });
            if (Opened.HasValue()) {
                Ready = true; auto Reply = Policy;
                Reply["archiveOptionsVersion"] = 1;
                if (!Plugin.viewer_ && Session) {
                    Reply["sessionProtocol"] = 2; Reply["sessionEpoch"] = Session->Epoch();
                }
                Result = Reply;
            } else Result = Opened.Failure();
        }
        else if (Ready && Frame.Value().payload_.isEmpty())
        {
            if (Header.kind_ == WireKind::Ping) Result = Restrictions();
            else if (Header.kind_ == WireKind::Probe) Result = Execute([&] { return Plugin.Probe(); });
            else if (Header.kind_ == WireKind::Close) Result = QJsonObject{{"closed", true}};
        }
        else if (Ready && Header.kind_ == WireKind::PluginConfigure) {
            auto Request = QJsonDocument::fromJson(Frame.Value().payload_); if (!Request.isObject()) return 8;
            Result = Plugin.Configure(Request.object());
        }
        else if (Ready && Plugin.archives_ && IsArchiveCommand(Header.kind_))
        {
            QJsonParseError Error; const auto Request = QJsonDocument::fromJson(Frame.Value().payload_, &Error);
            if (Error.error != QJsonParseError::NoError || !Request.isObject()) return 8;
            Result = Execute([&] { return Plugin.archives_->Handle(Header.kind_, Request.object(), [&](const std::string& Archive, unsigned Attempt, const OperationContext& Context) -> hc::storage::Result<Password> {
                const auto Query = QJsonDocument(QJsonObject{{"archive", QString::fromStdString(Archive)}, {"attempt", int(Attempt)}}).toJson(QJsonDocument::Compact);
                auto Sent = WriteFrame(Pipe.Get(), Parent.Get(), WireKind::ArchivePassword, Header.sequence_, Query, Context);
                if (!Sent.HasValue()) return Sent.Failure();
                auto Reply = ReadFrame(Pipe.Get(), Parent.Get(), Context); if (!Reply.HasValue()) return Reply.Failure();
                if (Reply.Value().header_.sequence_ != Header.sequence_ || Reply.Value().header_.kind_ != WireKind::ArchivePassword)
                    return HostError("host-password-envelope");
                auto Document = QJsonDocument::fromJson(Reply.Value().payload_); Reply.Value().payload_.fill(0);
                auto Object = Document.object(); Document = {};
                if (Object.size() != 2 || !Object["accepted"].isBool() || !Object["password"].isString()) return HostError("host-password-payload");
                auto Text = Object["password"].toString(); const bool Accepted = Object["accepted"].toBool(); Object = {};
                if (!Accepted) { Text.fill(QChar::Null); return hc::storage::Error{ErrorCategory::Cancelled, "archive-password-cancelled", {"wcx"}}; }
                if (Text.size() > 4096 || Text.contains(QChar::Null)) { Text.fill(QChar::Null); return HostError("host-password-size"); }
                Password Value(Text.toStdString()); Text.fill(QChar::Null); return Value;
            }); });
        }
        else if (Ready && Plugin.viewer_ && (IsViewerCommand(Header.kind_) || Header.kind_ == WireKind::MetadataQuery || Header.kind_ == WireKind::ContentConvert))
        {
            QJsonParseError Error; const auto Request = QJsonDocument::fromJson(Frame.Value().payload_, &Error);
            if (Error.error != QJsonParseError::NoError || !Request.isObject()) return 8;
            Result = Plugin.viewer_->Handle(Header.kind_, Request.object());
        }
        else if (Ready && (Plugin.data_ || Plugin.dopusVfs_) && IsDataCommand(Header.kind_))
        {
            QJsonParseError Error; const auto Request = QJsonDocument::fromJson(Frame.Value().payload_, &Error);
            if (Error.error != QJsonParseError::NoError || !Request.isObject()) return 8;
            Result = Execute([&] { return Plugin.dopusVfs_ ? Plugin.dopusVfs_->Handle(Header.kind_, Request.object()) : Plugin.data_->Handle(Header.kind_, Request.object()); });
        }
        const auto Reply = Result.HasValue() ? Result.Value() : QJsonObject{{"code", QString::fromStdString(Result.Failure().code_)},
            {"category", static_cast<int>(Result.Failure().category_)}, {"provider", QString::fromStdString(Result.Failure().provider_.value_)},
            {"retryable", Result.Failure().retryable_}, {"userAction", QString::fromStdString(Result.Failure().userAction_)}};
        if (!WriteFrame(Pipe.Get(), Parent.Get(), Result.HasValue() ? WireKind::Response : WireKind::Failure,
            Header.sequence_, QJsonDocument(Reply).toJson(QJsonDocument::Compact), Deadline(3000)).HasValue()) return 10;
        if (!Ready || Header.kind_ == WireKind::Close) return Result.HasValue() ? 0 : 11;
    }
}
