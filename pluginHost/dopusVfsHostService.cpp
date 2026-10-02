#include "dopusVfsHostService.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <array>
#include <QCryptographicHash>
#include <QSet>
#include <QScopeGuard>

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
        for (const auto Name : {"VFS_IdentifyW", "VFS_Create", "VFS_Destroy", "VFS_GetLastError"})
            if (!GetProcAddress(module_, Name)) return Unsupported("dopus-vfs-required-exports");
        if (!GetProcAddress(module_, "VFS_ReadDirectoryW") &&
            (!GetProcAddress(module_, "VFS_FindFirstFileW") || !GetProcAddress(module_, "VFS_FindNextFileW") || !GetProcAddress(module_, "VFS_FindClose"))) return Unsupported("dopus-vfs-enumeration-exports");
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
        if (Info.smallIcon_) DestroyIcon(Info.smallIcon_); if (Info.largeIcon_ && Info.largeIcon_ != Info.smallIcon_) DestroyIcon(Info.largeIcon_);
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
            if (Root.isEmpty()) return Invalid("dopus-vfs-mount-root"); root_ = Root;
            const auto Scope = QString::fromLatin1(QCryptographicHash::hash((QString::fromStdString(manifest_.id_) + "\n" + root_.toCaseFolded()).toUtf8(), QCryptographicHash::Md5).toHex());
            const auto ReadColumns = [&]<class Char>(const char* ExportName) -> Result<Unit> {
                const auto Get = Export<dopus::VfsCustomColumn<Char>*(*)(HANDLE)>(module_, ExportName); if (!Get) return Unit{};
                QSet<const void*> Seen; QSet<int> Ids;
                for (const auto* Column = Get(instance_); Column; Column = Column->next_) {
                    if (Seen.contains(Column) || Seen.size() >= 64 || Column->size_ < sizeof(*Column) || Column->id_ < 1 || Ids.contains(Column->id_)) return Invalid("dopus-vfs-column-schema");
                    Seen.insert(Column); Ids.insert(Column->id_);
                    const auto Text = [](const Char* Value) -> std::optional<QString> {
                        if (!Value) return {}; const auto* End = std::find(Value, Value + 1024, Char(0)); if (End == Value + 1024) return {};
                        if constexpr (std::is_same_v<Char, wchar_t>) return QString::fromWCharArray(Value, int(End - Value));
                        else return QString::fromLocal8Bit(Value, int(End - Value));
                    };
                    const auto Label = Text(Column->label_), Key = Text(Column->key_); if (!Label || !Key || Label->isEmpty() || Key->isEmpty()) return Invalid("dopus-vfs-column-text");
                    columns_.append(QJsonObject{{"id", Column->id_}, {"key", "dopusvfs." + Scope + "." + QString::number(Column->id_)}, {"label", *Label}, {"flags", qint64(Column->flags_)}});
                }
                return Unit{};
            };
            const auto Columns = GetProcAddress(module_, "VFS_GetCustomColumnsW") ? ReadColumns.operator()<wchar_t>("VFS_GetCustomColumnsW") : ReadColumns.operator()<char>("VFS_GetCustomColumnsA");
            if (!Columns.HasValue()) return Columns.Failure();
            return QJsonObject{{"columns", columns_}, {"batchAdd", batchAvailable(root_.toStdWString(), 1)}, {"batchExtract", batchAvailable(root_.toStdWString(), 2)},
                {"batchDelete", batchAvailable(root_.toStdWString(), 3)}};
        }
        if (Action == "open" && Request.size() == 2)
        {
            auto Name = path(Request["path"]); if (!Name.HasValue()) return Name.Failure();
            if (cursors_.size() >= 64 || nextCursor_ == INT_MAX) return Error{ErrorCategory::BudgetExceeded, "dopus-vfs-cursor-budget"};
            if (const auto Read = Export<int(*)(HANDLE, void*, dopus::VfsReadDirectoryData*)>(module_, "VFS_ReadDirectoryW")) {
                const auto Heap = HeapCreate(0, 0, 0); if (!Heap) return HostError("dopus-vfs-column-heap");
                struct HeapGuard { HANDLE heap_; ~HeapGuard() { HeapDestroy(heap_); } } Cleanup{Heap};
                dopus::VfsReadDirectoryData Data{sizeof(Data), nullptr, Name.Value().data(), 1, abort_.Get(), Heap, nullptr};
                if (!Read(instance_, nullptr, &Data)) return failure("read-directory");
                Cursor Value; QSet<const void*> Seen; std::size_t TextBytes = 0;
                for (auto* Header = Data.files_; Header; Header = Header->next_) {
                    if (Seen.contains(Header) || !HeapValidate(Heap, 0, Header)) return Invalid("dopus-vfs-directory-chain"); Seen.insert(Header);
                    const auto Bytes = HeapSize(Heap, 0, Header);
                    if (Header->size_ < sizeof(*Header) || Header->count_ < 0 || Header->entrySize_ < sizeof(dopus::VfsFileData) ||
                        quint64(Header->size_) + quint64(Header->count_) * Header->entrySize_ > Bytes || retainedEntries_ + Value.entries_.size() + Header->count_ > 65536) return Invalid("dopus-vfs-directory-size");
                    for (int Index = 0; Index < Header->count_; ++Index) {
                        const auto* Entry = reinterpret_cast<const dopus::VfsFileData*>(reinterpret_cast<const char*>(Header) + Header->size_ + std::size_t(Index) * Header->entrySize_);
                        if (Entry->columnCount_ < 0 || Entry->columnCount_ > 64 || (Entry->columnCount_ && !Entry->columns_)) return Invalid("dopus-vfs-column-count");
                        Record Record{Entry->file_, {}};
                        for (int Col = 0; Col < Entry->columnCount_; ++Col) {
                            const auto& Cell = Entry->columns_[Col]; const auto Key = QString::number(Cell.id_);
                            if (!Cell.value_ || Record.columns_.contains(Key) || std::none_of(columns_.begin(), columns_.end(), [&](const auto& Def) { return Def.toObject()["id"].toInt() == Cell.id_; })) return Invalid("dopus-vfs-column-id");
                            const auto* End = std::find(Cell.value_, Cell.value_ + 4096, L'\0'); if (End == Cell.value_ + 4096) return Invalid("dopus-vfs-column-termination");
                            TextBytes += (End - Cell.value_) * 2; if (TextBytes > 8 * 1024 * 1024) return Invalid("dopus-vfs-column-budget");
                            Record.columns_[Key] = QString::fromWCharArray(Cell.value_, int(End - Cell.value_));
                        }
                        Value.entries_.push_back(std::move(Record));
                    }
                }
                retainedEntries_ += Value.entries_.size(); const auto Id = nextCursor_++; cursors_.emplace(Id, std::move(Value)); return QJsonObject{{"cursor", Id}, {"end", false}};
            }
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
                Value.entries_.push_back({Data, {}});
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
        auto& Cursor = At->second; QJsonArray Entries; int Examined = 0; qsizetype PageBytes = 64;
        while (Cursor.next_ < Cursor.entries_.size() && Entries.size() < Request["count"].toInt() && ++Examined <= 1024)
        {
            const auto& Record = Cursor.entries_[Cursor.next_++]; const auto& Data = Record.file_; const auto End = std::find(std::begin(Data.cFileName), std::end(Data.cFileName), L'\0');
            if (End == std::end(Data.cFileName)) return Invalid("dopus-vfs-name-termination");
            const auto Name = QString::fromWCharArray(Data.cFileName);
            if (Name.isEmpty() || Name.contains('/') || Name.contains('\\') || Name.contains(':')) return Invalid("dopus-vfs-entry-name");
            if (Name != "." && Name != "..") {
                const QJsonObject Entry{{"columns", Record.columns_}, {"name", Name}, {"attributes", qint64(Data.dwFileAttributes)},
                    {"size", QString::number((quint64(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow)},
                    {"modified", QString::number(Ticks(Data.ftLastWriteTime))}, {"created", QString::number(Ticks(Data.ftCreationTime))}};
                const auto Bytes = QJsonDocument(Entry).toJson(QJsonDocument::Compact).size() + 1;
                if (Bytes > WirePayloadLimit - 1024) return Invalid("dopus-vfs-column-entry-budget");
                if (!Entries.isEmpty() && PageBytes + Bytes > WirePayloadLimit - 1024) { --Cursor.next_; break; }
                PageBytes += Bytes; Entries.append(Entry);
            }
        }
        return QJsonObject{{"entries", Entries}, {"end", Cursor.next_ == Cursor.entries_.size()}};
    }
    bool DopusVfsHostService::batchAvailable(std::wstring Path, UINT Operation, std::wstring Destination)
    {
        if (!GetProcAddress(module_, "VFS_BatchOperationW")) return false;
        if (const auto Query = Export<dopus::VfsProperty>(module_, "VFS_PropGetW"))
        {
            DWORD Value = 0;
            if (!Query(instance_, dopus::VfsBatchProperty, &Value, Path.data(), Destination.empty() ? nullptr : Destination.data(), reinterpret_cast<void*>(ULONG_PTR(Operation)))) return false;
            return (Value & ~4096u) == 2;
        }
        return true;
    }
    Result<bool> DopusVfsHostService::batch(std::wstring Path, const std::vector<std::wstring>& Files, UINT Operation, std::wstring Destination)
    {
        if (Files.empty() || Files.size() > 256) return Invalid("dopus-vfs-batch-count");
        if (!batchAvailable(Path, Operation, Destination)) return false;
        const auto Call = Export<dopus::VfsBatch>(module_, "VFS_BatchOperationW");
        for (std::size_t First = 0; First < Files.size();)
        {
            std::vector<wchar_t> Names;
            for (auto Index = First; Index < Files.size(); ++Index)
            { Names.insert(Names.end(), Files[Index].begin(), Files[Index].end()); Names.push_back(0); }
            Names.push_back(0);
            // A plugin must positively report each processed entry. Missing
            // result writes cannot turn partial archive changes into success.
            // The supplied RAR sample uses -1 to recognize unprocessed files.
            std::vector<int> Results(Files.size() - First, -1);
            dopus::VfsBatchData Data{sizeof(Data), nullptr, abort_.Get(), Operation, int(Results.size()), Names.data(), Results.data(),
                Destination.empty() ? nullptr : Destination.data(), Operation == 3 ? (1u << 11) | (1u << 12) : 0, nullptr};
            const auto Code = Call(instance_, nullptr, Path.data(), &Data);
            const auto Result = Code & ~4096u;
            if (Result == 0 && First == 0) return false;
            if (Result == 3) return Error{ErrorCategory::Cancelled, "dopus-vfs-batch-aborted", {"dopus-vfs"}};
            if (Result != 2 && Result != 4) return Error{ErrorCategory::Unavailable, "dopus-vfs-batch-incomplete", {"dopus-vfs"}};
            const auto Count = Result == 4 ? Results.size() : std::size_t(1);
            for (std::size_t Index = 0; Index < Count; ++Index)
                if (Results[Index] != 0) return Error{ErrorCategory::Unavailable, "dopus-vfs-batch-entry:" + std::to_string(First + Index) + ":" + std::to_string(Results[Index]), {"dopus-vfs"}};
            First += Count;
        }
        return true;
    }
    Result<std::shared_ptr<IReadStream>> DopusVfsHostService::OpenStream(const QJsonObject& Request, OperationContext Context)
    {
        if (const auto Stop = Context.Check({"dopus-vfs"})) return *Stop;
        if (!manifest_.extensions_.contains(Extension::StorageRead) || !manifest_.permissions_.contains(Permission::ReadContent)) return Unsupported("dopus-vfs-stream-denied");
        auto Name = path(Request["path"]); if (!Name.HasValue()) return Name.Failure();
        const auto Open = Export<dopus::VfsOpenFile>(module_, "VFS_CreateFileW");
        const auto Read = Export<dopus::VfsReadFile>(module_, "VFS_ReadFile");
        const auto Close = Export<dopus::VfsCloseFile>(module_, "VFS_CloseFile");
        // A batch-only archive may expose stubs for file I/O. Do not silently
        // replace its known batch extraction path with those stubs.
        const auto Parent = Name.Value().substr(0, Name.Value().find_last_of(L'\\'));
        if (!Open || !Read || !Close || batchAvailable(Parent, 2)) return Unsupported("dopus-vfs-direct-stream-unavailable");
        auto File = Open(instance_, nullptr, Name.Value().data(), GENERIC_READ, FILE_ATTRIBUTE_NORMAL, OPEN_EXISTING, nullptr);
        if (!File || File == INVALID_HANDLE_VALUE) return failure("stream-open");
        class Stream final : public IReadStream
        {
        public:
            Stream(HANDLE Instance, HANDLE File, dopus::VfsReadFile Read, dopus::VfsCloseFile Close) : instance_(Instance), file_(File), read_(Read), close_(Close) {}
            ~Stream() override { close_(instance_, nullptr, file_); }
            bool CanSeek() const override { return false; }
            ReadAccess Access() const override { return ReadAccess::Sequential; }
            Task<Unit> Seek(std::uint64_t, OperationContext) override { return Ready<Unit>(Error{ErrorCategory::Unsupported, "dopus-stream-seek-unavailable"}); }
            Task<ReadChunk> Read(std::size_t Count, OperationContext Context) override
            {
                if (const auto Stop = Context.Check({"dopus-vfs"})) return Ready<ReadChunk>(*Stop);
                Count = std::min({Count, Context.maxReadBytes_, std::size_t(WirePayloadLimit / 2)});
                if (!Count) return Ready<ReadChunk>(Error{ErrorCategory::InvalidRequest, "dopus-stream-read-size"});
                ReadChunk Chunk; Chunk.bytes_.resize(Count); DWORD Done = 0;
                if (!read_(instance_, nullptr, file_, Chunk.bytes_.data(), DWORD(Count), &Done)) return Ready<ReadChunk>(HostError("dopus-stream-read"));
                if (Done > Count) return Ready<ReadChunk>(HostError("dopus-stream-read-count"));
                Chunk.bytes_.resize(Done); Chunk.eof_ = Done == 0;
                if (Context.progress_) Context.progress_(Done);
                return Ready<ReadChunk>(std::move(Chunk));
            }
        private:
            HANDLE instance_, file_; dopus::VfsReadFile read_; dopus::VfsCloseFile close_;
        };
        return std::shared_ptr<IReadStream>(std::make_shared<Stream>(instance_, File, Read, Close));
    }
    Result<QJsonObject> DopusVfsHostService::transfer(const QJsonObject& Request, bool Write)
    {
        auto Name = path(Request["path"]); if (!Name.HasValue()) return Name.Failure();
        if (!Local(Request["local"].toString())) return Invalid("dopus-vfs-local-path");
        const auto Open = Export<dopus::VfsOpenFile>(module_, "VFS_CreateFileW");
        const auto Close = Export<dopus::VfsCloseFile>(module_, "VFS_CloseFile");
        const auto Read = Export<dopus::VfsReadFile>(module_, "VFS_ReadFile"); const auto Put = Export<dopus::VfsWriteFile>(module_, "VFS_WriteFile");
        // Batch add can implement archive rewriting without CreateFile/WriteFile.
        // Preserve the destination basename in the private staging directory;
        // SDK ADD takes local filenames, not a separate rename argument.
        const auto Separator = Name.Value().find_last_of(L'\\'); const auto Parent = Name.Value().substr(0, Separator);
        if (batchAvailable(Parent, Write ? 1 : 2))
        {
            QTemporaryDir Stage; if (!Stage.isValid()) return HostError("dopus-vfs-batch-stage");
            const auto Leaf = QString::fromStdWString(Name.Value().substr(Separator + 1));
            const auto Staged = Stage.filePath(Leaf);
            if (Write && (QFileInfo(Request["local"].toString()).size() > 256LL * 1024 * 1024 || !QFile::copy(Request["local"].toString(), Staged))) return HostError("dopus-vfs-batch-stage-copy");
            auto Done = batch(Parent, {Write ? QDir::toNativeSeparators(Staged).toStdWString() : Name.Value()}, Write ? 1 : 2, Write ? std::wstring{} : QDir::toNativeSeparators(Stage.path()).toStdWString());
            if (!Done.HasValue()) return Done.Failure();
            if (Done.Value())
            {
                if (!Write)
                {
                    QFile Input(Staged), Output(Request["local"].toString());
                    if (!Input.open(QIODevice::ReadOnly) || Input.size() > 256LL * 1024 * 1024 || !Output.open(QIODevice::WriteOnly | QIODevice::Truncate)) return HostError("dopus-vfs-batch-extract-output");
                    while (!Input.atEnd()) { const auto Bytes = Input.read(256 * 1024); if (Bytes.isEmpty() || Output.write(Bytes) != Bytes.size()) return HostError("dopus-vfs-batch-extract-copy"); }
                    if (!Output.flush()) return HostError("dopus-vfs-batch-extract-flush");
                }
                return QJsonObject{};
            }
        }
        if (!Open || !Close || (Write ? !Put : !Read)) return Unsupported("dopus-vfs-transfer-exports");
        QFile LocalFile(Request["local"].toString());
        // The content store reserves this output file before invoking its
        // producer, keeping the lease directory and ownership record pinned.
        if (!LocalFile.open(Write ? QIODevice::ReadOnly : QIODevice::WriteOnly | QIODevice::Truncate)) return HostError("dopus-vfs-local-open");
        const auto Budget = context_.workload_.IsAutomatic() ? context_.workload_.automaticFileBytes_ : UINT64_MAX;
        if (Write && quint64(LocalFile.size()) > Budget) return Error{ErrorCategory::BudgetExceeded, "dopus-vfs-transfer-budget"};
        auto File = Open(instance_, nullptr, Name.Value().data(), Write ? GENERIC_WRITE : GENERIC_READ, FILE_ATTRIBUTE_NORMAL,
            Write ? Request["replace"].toBool() ? CREATE_ALWAYS : CREATE_NEW : OPEN_EXISTING, nullptr);
        if (!File || File == INVALID_HANDLE_VALUE) { if (!Write) LocalFile.remove(); return failure("open-file"); }
        struct Guard { HANDLE instance_, file_; dopus::VfsCloseFile close_; ~Guard() { close_(instance_, nullptr, file_); } } Cleanup{instance_, File, Close};
        std::array<char, 256 * 1024> Buffer{}; quint64 Total = 0;
        for (;;)
        {
            if (const auto Stop = context_.Check({"dopus-vfs"})) return *Stop;
            if (context_.progress_) context_.progress_(Total);
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
    Result<QJsonObject> DopusVfsHostService::Handle(WireKind Kind, const QJsonObject& Request, OperationContext Context)
    {
        ResetEvent(abort_.Get());
        std::stop_callback Cancel(Context.stopToken_, [this] { SetEvent(abort_.Get()); });
        context_ = Context;
        auto ClearContext = qScopeGuard([this] { context_ = {}; });
        if (const auto Stop = Context.Check({"dopus-vfs"})) return *Stop;
        if (!instance_) return Invalid("dopus-vfs-not-initialized");
        if (!manifest_.extensions_.contains(Extension::StorageRead)) return Unsupported("dopus-vfs-storage-extension");
        if (Kind == WireKind::StorageEnumerate) return enumerate(Request);
        if (Kind == WireKind::StorageGet && Request.size() == 3 && Request["info"].isObject()) return transfer(Request, false);
        if (Kind != WireKind::StorageMutate) return Invalid("dopus-vfs-command");
        if (!manifest_.extensions_.contains(Extension::StorageWrite) || !manifest_.permissions_.contains(Permission::WriteContent)) return Error{ErrorCategory::AccessDenied, "dopus-vfs-write-denied"};
        if (capabilities_ & dopus::VfsReadOnly) return Unsupported("dopus-vfs-read-only");
        auto Name = path(Request["path"]); if (!Name.HasValue()) return Name.Failure();
        if (Request["action"] == "batch-delete" && Request.size() == 3 && Request["files"].isArray())
        {
            std::vector<std::wstring> Files;
            for (const auto& Item : Request["files"].toArray())
            { if (!Item.isString() || Item.toString() == "\\") return Invalid("dopus-vfs-batch-file"); auto File = path(Item); if (!File.HasValue()) return File.Failure(); Files.push_back(std::move(File.Value())); }
            auto Done = batch(Name.Value(), Files, 3); if (!Done.HasValue()) return Done.Failure();
            if (!Done.Value()) return Unsupported("dopus-vfs-batch-declined"); return QJsonObject{{"processed", int(Files.size())}};
        }
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
            auto Done = batch(Name.Value().substr(0, Name.Value().find_last_of(L'\\')), {Name.Value()}, 3);
            if (!Done.HasValue()) return Done.Failure(); if (Done.Value()) return QJsonObject{};
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
