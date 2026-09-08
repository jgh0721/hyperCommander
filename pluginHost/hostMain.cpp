#include "pipeChannel.hpp"
#include "manifestFile.hpp"
#include "wcxHostService.hpp"
#include "wlxHostService.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <thread>

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
        return {{"inJob", bool(InJob)}, {"adminDenied", !bool(AdminEnabled)}, {"privilegesRemoved", PrivilegesRemoved}, {"processId", qint64(GetCurrentProcessId())}};
    }
    struct LoadedPlugin
    {
        std::vector<WinHandle> locks_;
        std::unique_ptr<WcxHostService> archives_;
        std::unique_ptr<WlxHostService> viewer_;
        HMODULE module_ = nullptr;
        ~LoadedPlugin() { viewer_.reset(); archives_.reset(); if (module_) FreeLibrary(module_); }
        Result<Unit> Open(const QJsonObject& Hello, DWORD ParentProcess)
        {
            if (Hello.size() != 2 || !Hello["manifestPath"].isString() || !Hello["manifest"].isObject()) return HostError("host-hello-fields");
            const auto Path = Hello["manifestPath"].toString();
            auto Expected = ParseManifest(QJsonDocument(Hello["manifest"].toObject()).toJson()); if (!Expected.HasValue()) return Expected.Failure();
            if ((Expected.Value().format_ != PluginFormat::Wcx && Expected.Value().format_ != PluginFormat::Wlx) || Expected.Value().architecture_ != Architecture::X64
                || Expected.Value().execution_ != ExecutionMode::Isolated
                || (Expected.Value().format_ == PluginFormat::Wcx && !Expected.Value().extensions_.contains(Extension::ArchiveRead))
                || Expected.Value().protocol_.major_ != 1 || Expected.Value().protocol_.minimumMinor_ != 0) return HostError("host-unsupported-manifest");
            auto Inspected = InspectManifestFile(Path, Deadline(5000)); if (!Inspected.HasValue()) return Inspected.Failure();
            if (Inspected.Value().manifest_ != Expected.Value()) return HostError("host-manifest-changed");
            const auto Binary = Inspected.Value().binaryPath_;
            // Deny replacement of the file and its ancestor directory names until
            // unload. Recheck while these handles are held, before any DllMain.
            auto Directory = QFileInfo(Binary).absolutePath();
            for (;;)
            {
                WinHandle Lock(CreateFileW(reinterpret_cast<LPCWSTR>(Directory.utf16()), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (!Lock.Valid()) return HostError("host-package-lock", GetLastError()); locks_.push_back(std::move(Lock));
                const auto Parent = QFileInfo(Directory).absolutePath(); if (Parent == Directory) break; Directory = Parent;
            }
            WinHandle File(CreateFileW(reinterpret_cast<LPCWSTR>(Binary.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!File.Valid()) return HostError("host-binary-lock", GetLastError()); locks_.push_back(std::move(File));
            Inspected = InspectManifestFile(Path, Deadline(5000)); if (!Inspected.HasValue()) return Inspected.Failure();
            if (Inspected.Value().manifest_ != Expected.Value() || Inspected.Value().binaryPath_ != Binary) return HostError("host-manifest-changed");
            module_ = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Binary.utf16()), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module_) return HostError("host-plugin-load", GetLastError());
            if (Expected.Value().format_ == PluginFormat::Wlx) viewer_ = std::make_unique<WlxHostService>(module_, ParentProcess);
            else archives_ = std::make_unique<WcxHostService>(Binary.toStdString(), Expected.Value().extensions_.contains(Extension::ArchiveWrite));
            return Unit{};
        }
        Result<QJsonObject> Probe()
        {
            if (!module_) return HostError("host-plugin-not-loaded");
            if (viewer_) return viewer_->Probe();
            for (const auto Name : {"OpenArchiveW", "ReadHeaderExW", "ProcessFileW", "CloseArchive"})
                if (!GetProcAddress(module_, Name)) return HostError("host-wcx-unicode-exports");
            const auto Caps = reinterpret_cast<int(__stdcall*)()>(GetProcAddress(module_, "GetPackerCaps"));
            // Deliberately invoke the plugin only in this job-owned process.
            return QJsonObject{{"format", "wcx"}, {"unicode", true}, {"caps", Caps ? Caps() : 0}, {"processId", qint64(GetCurrentProcessId())}};
        }
    };
}
int main(int Argc, char** Argv)
{
    static_assert(sizeof(void*) == 8, "hc-plugin-host-x64 requires an x64 build");
    using namespace hc::plugins;
    using namespace hc::storage;
    QCoreApplication App(Argc, Argv);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR)) return 2;
    if (App.arguments().size() != 3 || !App.arguments()[1].startsWith("\\\\.\\pipe\\hyperCommander-")) return 2;
    bool Parsed = false; const auto ParentId = App.arguments()[2].toUInt(&Parsed); if (!Parsed || !ParentId) return 2;
    WinHandle Parent(OpenProcess(SYNCHRONIZE, FALSE, ParentId)); if (!Parent.Valid()) return 3;
    const auto PipeName = App.arguments()[1];
    WinHandle Pipe(CreateFileW(reinterpret_cast<LPCWSTR>(PipeName.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!Pipe.Valid()) return 4;
    ULONG Server = 0; if (!GetNamedPipeServerProcessId(Pipe.Get(), &Server) || Server != ParentId) return 5;
    LoadedPlugin Plugin; bool Ready = false; std::uint64_t LastSequence = 0;
    for (;;)
    {
        auto Frame = ReadFrame(Pipe.Get(), Parent.Get(), Deadline(120000), Plugin.viewer_ != nullptr); if (!Frame.HasValue()) return 6;
        const auto& Header = Frame.Value().header_;
        if (Header.sequence_ != LastSequence + 1) return 7; LastSequence = Header.sequence_;
        Result<QJsonObject> Result = HostError("host-command-unavailable");
        if (!Ready && Header.kind_ == WireKind::Hello)
        {
            QJsonParseError Error; const auto Request = QJsonDocument::fromJson(Frame.Value().payload_, &Error);
            if (Error.error != QJsonParseError::NoError || !Request.isObject()) return 8;
            const auto Policy = Restrictions();
            if (!Policy["inJob"].toBool() || !Policy["adminDenied"].toBool() || !Policy["privilegesRemoved"].toBool()) return 9;
            auto Opened = Plugin.Open(Request.object(), ParentId); if (Opened.HasValue()) { Ready = true; Result = Policy; } else Result = Opened.Failure();
        }
        else if (Ready && Frame.Value().payload_.isEmpty())
        {
            if (Header.kind_ == WireKind::Ping) Result = Restrictions();
            else if (Header.kind_ == WireKind::Probe) Result = Plugin.Probe();
            else if (Header.kind_ == WireKind::Close) Result = QJsonObject{{"closed", true}};
        }
        else if (Ready && Plugin.archives_ && Header.kind_ >= WireKind::ArchiveOpen && Header.kind_ <= WireKind::ArchiveUpdateApply)
        {
            QJsonParseError Error; const auto Request = QJsonDocument::fromJson(Frame.Value().payload_, &Error);
            if (Error.error != QJsonParseError::NoError || !Request.isObject()) return 8;
            Result = Plugin.archives_->Handle(Header.kind_, Request.object());
        }
        else if (Ready && Plugin.viewer_ && Header.kind_ >= WireKind::ViewerOpen && Header.kind_ <= WireKind::ViewerClose)
        {
            QJsonParseError Error; const auto Request = QJsonDocument::fromJson(Frame.Value().payload_, &Error);
            if (Error.error != QJsonParseError::NoError || !Request.isObject()) return 8;
            Result = Plugin.viewer_->Handle(Header.kind_, Request.object());
        }
        const auto Reply = Result.HasValue() ? Result.Value() : QJsonObject{{"code", QString::fromStdString(Result.Failure().code_)},
            {"category", static_cast<int>(Result.Failure().category_)}, {"provider", QString::fromStdString(Result.Failure().provider_.value_)},
            {"retryable", Result.Failure().retryable_}, {"userAction", QString::fromStdString(Result.Failure().userAction_)}};
        if (!WriteFrame(Pipe.Get(), Parent.Get(), Result.HasValue() ? WireKind::Response : WireKind::Failure,
            Header.sequence_, QJsonDocument(Reply).toJson(QJsonDocument::Compact), Deadline(3000)).HasValue()) return 10;
        if (!Ready || Header.kind_ == WireKind::Close) return Result.HasValue() ? 0 : 11;
    }
}
