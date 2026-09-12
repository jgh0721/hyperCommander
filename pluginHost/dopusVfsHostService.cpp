#include "dopusVfsHostService.hpp"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <array>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        template<class T> T Export(HMODULE Module, const char* Name) { return reinterpret_cast<T>(GetProcAddress(Module, Name)); }
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"dopus-vfs"}}; }
        Error Unsupported(const char* Code) { return {ErrorCategory::Unsupported, Code, {"dopus-vfs"}}; }
        bool Integer(QJsonValue Value, int Low, int High)
        { return Value.isDouble() && Value.toDouble() == Value.toInt(Low - 1) && Value.toInt() >= Low && Value.toInt() <= High; }
        bool Local(QString Value) { return !Value.isEmpty() && Value.size() <= 1023 && !Value.contains(QChar::Null) && QDir::isAbsolutePath(Value); }
        quint64 Ticks(FILETIME Value) { return (quint64(Value.dwHighDateTime) << 32) | Value.dwLowDateTime; }
    }
    DopusVfsHostService::DopusVfsHostService(HMODULE Module, PluginManifest Manifest) : module_(Module), manifest_(std::move(Manifest)) {}
    DopusVfsHostService::~DopusVfsHostService()
    {
        if (abort_.Valid()) SetEvent(abort_.Get());
        if (instance_) Export<dopus::VfsDestroy>(module_, "VFS_Destroy")(instance_);
        if (initialized_) if (const auto Uninit = Export<dopus::Uninit>(module_, "VFS_Uninit")) Uninit();
    }
    Error DopusVfsHostService::failure(const char* Operation)
    {
        const auto Get = Export<dopus::VfsError>(module_, "VFS_GetLastError");
        const long Code = Get ? Get(instance_) : long(GetLastError());
        auto Category = Code == ERROR_FILE_NOT_FOUND || Code == ERROR_PATH_NOT_FOUND ? ErrorCategory::NotFound
            : Code == ERROR_ACCESS_DENIED || Code == -14 || Code == -15 ? ErrorCategory::AccessDenied
            : Code == ERROR_CANCELLED || Code == ERROR_OPERATION_ABORTED ? ErrorCategory::Cancelled
            : Code == -3 ? ErrorCategory::Unsupported : ErrorCategory::Unavailable;
        return {Category, std::string("dopus-vfs-") + Operation + ":" + std::to_string(Code), {"dopus-vfs"}};
    }
    Result<Unit> DopusVfsHostService::Initialize()
    {
        for (const auto Name : {"VFS_IdentifyW", "VFS_Create", "VFS_Destroy", "VFS_GetLastError", "VFS_FindFirstFileW", "VFS_FindNextFileW", "VFS_FindClose"})
            if (!GetProcAddress(module_, Name)) return Unsupported("dopus-vfs-required-exports");
        abort_ = WinHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr)); if (!abort_.Valid()) return HostError("dopus-vfs-abort-event");
        wchar_t Language[] = L"english"; dopus::InitData Init{sizeof(Init), nullptr, 9, 0, Language};
        if (const auto Call = Export<dopus::Init>(module_, "VFS_Init")) if (!Call(&Init)) return Unsupported("dopus-vfs-init");
        initialized_ = true;
        std::array<std::array<wchar_t, 1024>, 6> Text{};
        dopus::VfsInfo<wchar_t> Info{}; Info.size_ = sizeof(Info); Info.opusMajor_ = 9;
        Info.prefix_ = Text[0].data(); Info.extensions_ = Text[1].data(); Info.name_ = Text[2].data();
        Info.description_ = Text[3].data(); Info.copyright_ = Text[4].data(); Info.url_ = Text[5].data();
        Info.prefixCapacity_ = Info.extensionsCapacity_ = Info.nameCapacity_ = Info.descriptionCapacity_ = Info.copyrightCapacity_ = Info.urlCapacity_ = 1024;
        const auto Identified = Export<dopus::VfsIdentify>(module_, "VFS_IdentifyW")(&Info);
        if (Info.smallIcon_) DestroyIcon(Info.smallIcon_); if (Info.largeIcon_) DestroyIcon(Info.largeIcon_);
        if (!Identified) return Unsupported("dopus-vfs-identify");
        for (const auto& Value : Text) if (std::find(Value.begin(), Value.end(), L'\0') == Value.end()) return Invalid("dopus-vfs-info-termination");
        if (Info.flags_ & (1u << 2)) return Unsupported("dopus-vfs-multiple-formats");
        name_ = QString::fromWCharArray(Text[2].data()); extensions_ = QString::fromWCharArray(Text[1].data()); capabilities_ = Info.capabilities_;
        instance_ = Export<dopus::VfsCreate>(module_, "VFS_Create")(&Info.id_, nullptr);
        if (!instance_ || instance_ == INVALID_HANDLE_VALUE) { instance_ = nullptr; return Unsupported("dopus-vfs-create"); }
        if (const auto Get = Export<dopus::VfsCapabilities>(module_, "VFS_GetCapabilities")) capabilities_ = Get(instance_);
        return Unit{};
    }
    Result<QJsonObject> DopusVfsHostService::Probe()
    {
        if (!instance_) return Invalid("dopus-vfs-not-initialized");
        QJsonArray Exports;
        for (auto Name : {"VFS_CreateFileW", "VFS_ReadFile", "VFS_WriteFile", "VFS_CloseFile", "VFS_CreateDirectoryW", "VFS_DeleteFileW",
            "VFS_RemoveDirectoryW", "VFS_MoveFileW", "VFS_SetFileAttrW", "VFS_SetFileTimeW", "VFS_BatchOperationW"})
            if (GetProcAddress(module_, Name)) Exports.append(Name);
        return QJsonObject{{"format", "dopus-vfs"}, {"sdkVersion", 2}, {"unicode", true}, {"name", name_}, {"extensions", extensions_},
            {"readOnly", bool(capabilities_ & dopus::VfsReadOnly)}, {"caps", qint64(capabilities_)}, {"exports", Exports}};
    }
    Result<std::wstring> DopusVfsHostService::path(const QJsonValue& Remote)
    {
        const auto Name = Remote.toString();
        if (root_.isEmpty() || !Name.startsWith('\\') || Name.startsWith("\\\\") || Name.contains('/') || Name.contains(':') || Name.contains(QChar::Null)) return Invalid("dopus-vfs-path");
        for (const auto& Part : Name.split('\\')) if (Part == "." || Part == "..") return Invalid("dopus-vfs-traversal");
        const auto Full = root_ + Name; if (Full.size() > 1023) return Invalid("dopus-vfs-path-length"); return Full.toStdWString();
    }
    Result<QJsonObject> DopusVfsHostService::enumerate(const QJsonObject& Request)
    {
        const auto Action = Request["action"].toString();
        if (Action == "mount" && Request.size() == 2 && root_.isEmpty())
        {
            auto Root = Request["root"].toString();
            if (!Local(Root)) return Invalid("dopus-vfs-mount-root");
            Root = QDir::toNativeSeparators(QDir::cleanPath(Root));
            while (Root.endsWith('\\')) Root.chop(1);
            if (Root.isEmpty()) return Invalid("dopus-vfs-mount-root"); root_ = Root; return QJsonObject{};
        }
        if (Action == "open" && Request.size() == 2)
        {
            auto Name = path(Request["path"]); if (!Name.HasValue()) return Name.Failure();
            if (cursors_.size() >= 64 || nextCursor_ == INT_MAX) return Error{ErrorCategory::BudgetExceeded, "dopus-vfs-cursor-budget"};
            if (!Name.Value().ends_with(L'\\')) Name.Value() += L'\\'; Name.Value() += L'*'; Cursor Value{}; WIN32_FIND_DATAW Data{};
            const auto Find = Export<dopus::VfsFindFirst>(module_, "VFS_FindFirstFileW")(instance_, nullptr, Name.Value().data(), &Data, abort_.Get());
            if (!Find || Find == INVALID_HANDLE_VALUE)
            {
                const auto Code = Export<dopus::VfsError>(module_, "VFS_GetLastError")(instance_);
                if (Code && Code != ERROR_NO_MORE_FILES && Code != ERROR_FILE_NOT_FOUND) return failure("enumerate");
                return QJsonObject{{"cursor", 0}, {"end", true}};
            }
            struct Guard { HANDLE instance_, find_; dopus::VfsFindClose close_; ~Guard() { close_(instance_, find_); } } Cleanup{
                instance_, Find, Export<dopus::VfsFindClose>(module_, "VFS_FindClose")};
            // The official RAR sample holds an archive mutex until FindClose.
            // Its reader thread deadlocks if a caller retains an enumeration
            // while opening a file. Snapshot a bounded list before returning
            // the first page; the native find handle never crosses an IPC call.
            for (;;)
            {
                if (retainedEntries_ + Value.entries_.size() >= 65536) return Error{ErrorCategory::BudgetExceeded, "dopus-vfs-list-budget"};
                Value.entries_.push_back(Data);
                if (!Export<dopus::VfsFindNext>(module_, "VFS_FindNextFileW")(instance_, nullptr, Find, &Data))
                {
                    const auto Code = Export<dopus::VfsError>(module_, "VFS_GetLastError")(instance_);
                    if (Code && Code != ERROR_NO_MORE_FILES) return failure("next"); break;
                }
            }
            retainedEntries_ += Value.entries_.size();
            const auto Id = nextCursor_++; cursors_.emplace(Id, std::move(Value)); return QJsonObject{{"cursor", Id}, {"end", false}};
        }
        if (!Integer(Request["cursor"], 1, INT_MAX)) return Invalid("dopus-vfs-cursor");
        const auto At = cursors_.find(Request["cursor"].toInt()); if (At == cursors_.end()) return Invalid("dopus-vfs-cursor");
        if (Action == "close" && Request.size() == 2)
        { retainedEntries_ -= At->second.entries_.size(); cursors_.erase(At); return QJsonObject{}; }
        if (Action != "next" || Request.size() != 3 || !Integer(Request["count"], 1, 256)) return Invalid("dopus-vfs-page");
        auto& Cursor = At->second; QJsonArray Entries; int Examined = 0;
        while (Cursor.next_ < Cursor.entries_.size() && Entries.size() < Request["count"].toInt() && ++Examined <= 1024)
        {
            const auto& Data = Cursor.entries_[Cursor.next_++]; const auto End = std::find(std::begin(Data.cFileName), std::end(Data.cFileName), L'\0');
            if (End == std::end(Data.cFileName)) return Invalid("dopus-vfs-name-termination");
            const auto Name = QString::fromWCharArray(Data.cFileName);
            if (Name.isEmpty() || Name.contains('/') || Name.contains('\\') || Name.contains(':')) return Invalid("dopus-vfs-entry-name");
            if (Name != "." && Name != "..") Entries.append(QJsonObject{{"name", Name}, {"attributes", qint64(Data.dwFileAttributes)},
                {"size", QString::number((quint64(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow)},
                {"modified", QString::number(Ticks(Data.ftLastWriteTime))}, {"created", QString::number(Ticks(Data.ftCreationTime))}});
        }
        return QJsonObject{{"entries", Entries}, {"end", Cursor.next_ == Cursor.entries_.size()}};
    }
    Result<QJsonObject> DopusVfsHostService::transfer(const QJsonObject& Request, bool Write)
    {
        auto Name = path(Request["path"]); if (!Name.HasValue()) return Name.Failure();
        if (!Local(Request["local"].toString())) return Invalid("dopus-vfs-local-path");
        const auto Open = Export<dopus::VfsOpenFile>(module_, "VFS_CreateFileW");
        const auto Close = Export<dopus::VfsCloseFile>(module_, "VFS_CloseFile");
        const auto Read = Export<dopus::VfsReadFile>(module_, "VFS_ReadFile"); const auto Put = Export<dopus::VfsWriteFile>(module_, "VFS_WriteFile");
        if (!Open || !Close || (Write ? !Put : !Read)) return Unsupported("dopus-vfs-transfer-exports");
        QFile LocalFile(Request["local"].toString());
        // The content store reserves this output file before invoking its
        // producer, keeping the lease directory and ownership record pinned.
        if (!LocalFile.open(Write ? QIODevice::ReadOnly : QIODevice::WriteOnly | QIODevice::Truncate)) return HostError("dopus-vfs-local-open");
        const auto Budget = 256ULL * 1024 * 1024;
        if (Write && quint64(LocalFile.size()) > Budget) return Error{ErrorCategory::BudgetExceeded, "dopus-vfs-transfer-budget"};
        auto File = Open(instance_, nullptr, Name.Value().data(), Write ? GENERIC_WRITE : GENERIC_READ, FILE_ATTRIBUTE_NORMAL,
            Write ? Request["replace"].toBool() ? CREATE_ALWAYS : CREATE_NEW : OPEN_EXISTING, nullptr);
        if (!File || File == INVALID_HANDLE_VALUE) { if (!Write) LocalFile.remove(); return failure("open-file"); }
        struct Guard { HANDLE instance_, file_; dopus::VfsCloseFile close_; ~Guard() { close_(instance_, nullptr, file_); } } Cleanup{instance_, File, Close};
        std::array<char, 256 * 1024> Buffer{}; quint64 Total = 0;
        for (;;)
        {
            DWORD Count = 0;
            if (Write)
            {
                const auto Size = LocalFile.read(Buffer.data(), Buffer.size()); if (Size < 0) return HostError("dopus-vfs-local-read");
                DWORD Done = 0;
                do {
                    if (!Put(instance_, nullptr, File, Buffer.data() + Done, DWORD(Size) - Done, LocalFile.atEnd(), &Count)) return failure("write");
                    if (Count > DWORD(Size) - Done || (!Count && DWORD(Size) != Done)) return Invalid("dopus-vfs-write-count"); Done += Count;
                } while (Done < DWORD(Size));
                Total += Size; if (LocalFile.atEnd()) break;
            }
            else
            {
                if (!Read(instance_, nullptr, File, Buffer.data(), DWORD(Buffer.size()), &Count)) { LocalFile.remove(); return failure("read"); }
                if (Count > Buffer.size() || Total + Count > Budget) { LocalFile.remove(); return Error{ErrorCategory::BudgetExceeded, "dopus-vfs-transfer-budget"}; }
                if (!Count) break;
                if (LocalFile.write(Buffer.data(), Count) != Count) { LocalFile.remove(); return HostError("dopus-vfs-local-write"); }
                Total += Count;
            }
        }
        if (!Write && !LocalFile.flush()) { LocalFile.remove(); return HostError("dopus-vfs-local-flush"); }
        return QJsonObject{};
    }
    Result<QJsonObject> DopusVfsHostService::Handle(WireKind Kind, const QJsonObject& Request)
    {
        if (!instance_) return Invalid("dopus-vfs-not-initialized");
        if (!manifest_.extensions_.contains(Extension::StorageRead)) return Unsupported("dopus-vfs-storage-extension");
        if (Kind == WireKind::StorageEnumerate) return enumerate(Request);
        if (Kind == WireKind::StorageGet && Request.size() == 3 && Request["info"].isObject()) return transfer(Request, false);
        if (Kind != WireKind::StorageMutate) return Invalid("dopus-vfs-command");
        if (!manifest_.extensions_.contains(Extension::StorageWrite) || !manifest_.permissions_.contains(Permission::WriteContent)) return Error{ErrorCategory::AccessDenied, "dopus-vfs-write-denied"};
        if (capabilities_ & dopus::VfsReadOnly) return Unsupported("dopus-vfs-read-only");
        auto Name = path(Request["path"]); if (!Name.HasValue()) return Name.Failure();
        if (Request["path"] == "\\") return Invalid("dopus-vfs-mutate-root");
        const auto Action = Request["action"].toString();
        if (Action == "put" && Request.size() == 4 && Request["replace"].isBool()) return transfer(Request, true);
        BOOL Result = FALSE;
        if ((Action == "mkdir" || Action == "rmdir") && Request.size() == 2)
        {
            const auto Call = Export<dopus::VfsDirectory>(module_, Action == "mkdir" ? "VFS_CreateDirectoryW" : "VFS_RemoveDirectoryW");
            if (!Call) return Unsupported("dopus-vfs-directory"); Result = Call(instance_, nullptr, Name.Value().data(), 0);
        }
        else if (Action == "delete" && Request.size() == 2)
        {
            const auto Call = Export<dopus::VfsDeleteFile>(module_, "VFS_DeleteFileW"); if (!Call) return Unsupported("dopus-vfs-delete");
            Result = Call(instance_, nullptr, Name.Value().data(), 0, 0);
        }
        else if (Action == "rename" && Request.size() == 3)
        {
            auto To = path(Request["to"]); if (!To.HasValue()) return To.Failure();
            const auto Call = Export<dopus::VfsMoveFile>(module_, "VFS_MoveFileW"); if (!Call) return Unsupported("dopus-vfs-rename");
            Result = Call(instance_, nullptr, Name.Value().data(), To.Value().data());
        }
        else if (Action == "attributes" && Request.size() == 3 && Integer(Request["value"], 0, 65535))
        {
            const auto Call = Export<dopus::VfsSetAttributes>(module_, "VFS_SetFileAttrW"); if (!Call) return Unsupported("dopus-vfs-attributes");
            Result = Call(instance_, nullptr, Name.Value().data(), Request["value"].toInt(), FALSE);
        }
        else if (Action == "modified" && Request.size() == 3 && Request["value"].isString())
        {
            bool Parsed; const auto Ticks = Request["value"].toString().toULongLong(&Parsed); if (!Parsed) return Invalid("dopus-vfs-time");
            const auto Call = Export<dopus::VfsSetTime>(module_, "VFS_SetFileTimeW"); if (!Call) return Unsupported("dopus-vfs-time");
            FILETIME Time{DWORD(Ticks), DWORD(Ticks >> 32)}; Result = Call(instance_, nullptr, Name.Value().data(), nullptr, nullptr, nullptr, &Time);
        }
        else return Invalid("dopus-vfs-mutation-request");
        if (!Result) return failure("mutate"); return QJsonObject{};
    }
}
