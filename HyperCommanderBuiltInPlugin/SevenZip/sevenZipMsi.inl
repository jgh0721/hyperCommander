// MSI presents two namespaces: untouched Compound streams and an opt-in logical
// tree. Listing ordinals in the raw namespace never change. No installer session
// or action sequence is opened; Windows Installer is used only as a database reader.
namespace msiBrowse {
constexpr auto Root = "<Browse>";
struct Handle {
    MSIHANDLE value_ = 0;
    ~Handle() { if (value_) MsiCloseHandle(value_); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
Error Bad(std::string Code, ErrorCategory Category = ErrorCategory::InvalidRequest) { return Failure("msi-" + Code, Category); }
void Check(UINT Code, const char* Operation) {
    if (Code != ERROR_SUCCESS) throw Bad(std::string(Operation) + ":" + std::to_string(Code), ErrorCategory::Unavailable);
}
void Poll(const OperationContext& Context) { if (const auto Stop = Context.Check({"7zip"})) throw *Stop; }
QString String(MSIHANDLE Record, UINT Field) {
    DWORD Size = 0; wchar_t Empty = 0;
    const auto Code = MsiRecordGetStringW(Record, Field, &Empty, &Size);
    if (Code != ERROR_SUCCESS && Code != ERROR_MORE_DATA) Check(Code, "record-string");
    if (Size > 65535) throw Bad("string-budget", ErrorCategory::BudgetExceeded);
    std::vector<wchar_t> Buffer(size_t(Size) + 1); ++Size;
    Check(MsiRecordGetStringW(Record, Field, Buffer.data(), &Size), "record-string");
    return QString::fromWCharArray(Buffer.data(), int(Size));
}
template<class Visit> void Query(MSIHANDLE Database, const wchar_t* Sql, OperationContext Context, Visit Visitor) {
    Handle View; Check(MsiDatabaseOpenViewW(Database, Sql, &View.value_), "query-open");
    Check(MsiViewExecute(View.value_, 0), "query-execute");
    size_t Count = 0;
    for (;;) {
        Poll(Context); Handle Record; const auto Code = MsiViewFetch(View.value_, &Record.value_);
        if (Code == ERROR_NO_MORE_ITEMS) break;
        Check(Code, "query-fetch");
        if (++Count > Context.EnumerationLimit()) throw Bad("table-budget", ErrorCategory::BudgetExceeded);
        Visitor(Record.value_);
    }
}
bool SafeLeaf(const QString& Name) {
    if (Name.isEmpty() || Name == "." || Name == ".." || Name.endsWith('.') || Name.endsWith(' ') || Name.size() > 255) return false;
    for (const auto Ch : Name) if (Ch.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(Ch)) return false;
    const auto Stem = Name.section('.', 0, 0).toUpper();
    static const QRegularExpression Device("^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])$");
    return !Device.match(Stem).hasMatch();
}
QString NamePart(QString Name, bool Short = false) {
    if (Name.count('|') > 1) throw Bad("invalid-name");
    if (Name.contains('|')) Name = Name.section('|', Short ? 0 : 1, Short ? 0 : 1);
    if (!SafeLeaf(Name)) throw Bad("unsafe-name");
    return Name;
}
struct Directory { QString parent_, target_, source_; };
struct File { ArchiveHeader header_; QString key_, cabinet_, source_; };
struct Media { int last_; QString cabinet_; };
struct Package {
    Handle database_;
    QString path_;
    std::vector<File> files_;
};
Result<std::unique_ptr<Package>> Load(const std::string& Path, OperationContext Context) {
    try {
        Poll(Context); auto Out = std::make_unique<Package>(); Out->path_ = QString::fromStdString(Path);
        Check(MsiOpenDatabaseW(reinterpret_cast<LPCWSTR>(Out->path_.utf16()), MSIDBOPEN_READONLY, &Out->database_.value_), "open");
        Handle Summary; Check(MsiGetSummaryInformationW(Out->database_.value_, nullptr, 0, &Summary.value_), "summary");
        UINT Type = 0; INT Flags = 0; DWORD Length = 0;
        Check(MsiSummaryInfoGetPropertyW(Summary.value_, 15 /* PID_WORDCOUNT */, &Type, &Flags, nullptr, nullptr, &Length), "summary-flags");
        const bool ShortSource = (Flags & 1) != 0, Compressed = (Flags & 2) != 0;
        std::map<QString, Directory> Dirs; std::map<QString, QString> Components;
        Query(Out->database_.value_, L"SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`", Context, [&](MSIHANDLE R) {
            auto Value = String(R, 3); const auto Key = String(R, 1);
            if (Value.count(':') > 1 || Key.isEmpty()) throw Bad("directory-record");
            const auto Target = Value.section(':', 0, 0), Source = Value.contains(':') ? Value.section(':', 1, 1) : Target;
            if (!Dirs.emplace(Key, Directory{String(R, 2), Target, Source}).second) throw Bad("duplicate-directory");
        });
        Query(Out->database_.value_, L"SELECT `Component`, `Directory_` FROM `Component`", Context, [&](MSIHANDLE R) {
            if (!Components.emplace(String(R, 1), String(R, 2)).second) throw Bad("duplicate-component");
        });
        std::vector<Media> MediaRows;
        Query(Out->database_.value_, L"SELECT `LastSequence`, `Cabinet` FROM `Media` ORDER BY `DiskId`", Context, [&](MSIHANDLE R) {
            const auto Last = MsiRecordGetInteger(R, 1);
            if (Last < 0 || (!MediaRows.empty() && Last < MediaRows.back().last_)) throw Bad("media-sequence");
            MediaRows.push_back({Last, String(R, 2)});
        });
        const auto Resolve = [&](QString Id, bool Source) {
            QStringList Parts; std::set<QString> Seen;
            while (!Id.isEmpty()) {
                Poll(Context);
                if (!Seen.insert(Id).second || Seen.size() > 28 || !Dirs.contains(Id)) throw Bad("directory-cycle-or-missing");
                const auto& D = Dirs.at(Id);
                if (Id != "TARGETDIR") {
                    auto Part = Source ? D.source_ : D.target_;
                    // Standard destination properties have no static absolute
                    // path. Retain their symbolic root, without consulting this PC.
                    if (!Source && Part == "." && D.parent_ == "TARGETDIR") Part = Id;
                    if (Part != ".") Parts.prepend(NamePart(Part, Source && ShortSource));
                }
                Id = D.parent_;
            }
            return Parts.join('/');
        };
        std::set<QString> Paths, Keys;
        Query(Out->database_.value_, L"SELECT `File`, `Component_`, `FileName`, `FileSize`, `Attributes`, `Sequence` FROM `File` ORDER BY `File`", Context, [&](MSIHANDLE R) {
            File F; F.key_ = String(R, 1); const auto Component = String(R, 2), Names = String(R, 3);
            const auto Size = MsiRecordGetInteger(R, 4), Attributes = MsiRecordGetInteger(R, 5), Sequence = MsiRecordGetInteger(R, 6);
            if (F.key_.isEmpty() || !Keys.insert(F.key_.toCaseFolded()).second || !Components.contains(Component) || Size < 0 || Sequence <= 0) throw Bad("file-record");
            auto Relative = Resolve(Components.at(Component), false);
            if (!Relative.isEmpty()) Relative += '/'; Relative += NamePart(Names);
            if (Relative.size() > 950 || !Paths.insert(Relative.toCaseFolded()).second) throw Bad("duplicate-or-long-output-path");
            F.header_.name_ = std::string(Root) + '/' + Relative.toStdString(); F.header_.size_ = uint64_t(Size);
            F.header_.attributes_ = Attributes == MSI_NULL_INTEGER ? 0 : (Attributes & 7);
            const auto Bits = Attributes == MSI_NULL_INTEGER ? 0 : Attributes;
            if ((Bits & 0x6000) == 0x6000) throw Bad("conflicting-compression-flags");
            const bool Packed = (Bits & 0x4000) || (!(Bits & 0x2000) && Compressed);
            if (Packed) {
                const auto It = std::find_if(MediaRows.begin(), MediaRows.end(), [&](const Media& M) { return Sequence <= M.last_; });
                if (It == MediaRows.end() || It->cabinet_.isEmpty()) throw Bad("file-media-missing");
                F.cabinet_ = It->cabinet_; const auto Leaf = F.cabinet_.startsWith('#') ? F.cabinet_.mid(1) : F.cabinet_;
                if (!SafeLeaf(Leaf)) throw Bad("unsafe-cabinet-name");
            } else {
                F.source_ = Resolve(Components.at(Component), true);
                if (!F.source_.isEmpty()) F.source_ += '/'; F.source_ += NamePart(Names, ShortSource);
            }
            Out->files_.push_back(std::move(F));
        });
        // A file cannot also serve as another file's parent directory.
        for (const auto& F : Out->files_) {
            auto Parent = QString::fromStdString(F.header_.name_).mid(int(strlen(Root)) + 1);
            while (Parent.contains('/')) { Parent = Parent.left(Parent.lastIndexOf('/')); if (Paths.contains(Parent.toCaseFolded())) throw Bad("file-directory-collision"); }
        }
        if (MsiDatabaseIsTablePersistentW(Out->database_.value_, L"CreateFolder") == MSICONDITION_TRUE) {
            std::set<QString> Folders;
            Query(Out->database_.value_, L"SELECT `Directory_` FROM `CreateFolder` ORDER BY `Directory_`", Context, [&](MSIHANDLE R) {
                const auto Relative = Resolve(String(R, 1), false);
                if (Relative.isEmpty() || !Folders.insert(Relative.toCaseFolded()).second) return;
                if (Paths.contains(Relative.toCaseFolded())) throw Bad("file-directory-collision");
                auto Parent = Relative;
                while (Parent.contains('/')) { Parent = Parent.left(Parent.lastIndexOf('/')); if (Paths.contains(Parent.toCaseFolded())) throw Bad("file-directory-collision"); }
                File F; F.header_.name_ = std::string(Root) + '/' + Relative.toStdString();
                F.header_.directory_ = true; F.header_.attributes_ = FILE_ATTRIBUTE_DIRECTORY; Out->files_.push_back(std::move(F));
            });
        }
        return Out;
    } catch (const Error& E) { return E; }
}
bool IsMsi(HMODULE Module, const std::string& Path, OperationContext Context) {
    QFile Probe(QString::fromStdString(Path));
    if (!Probe.open(QIODevice::ReadOnly) || Probe.read(8) != QByteArray::fromHex("d0cf11e0a1b11ae1")) return false;
    auto Value = Open(Module, Path, Context); if (!Value.HasValue() || Value.Value()->format_ != "compound") return false;
    Property Extension;
    return Value.Value()->archive_->GetArchiveProperty(kpidExtension, &Extension) == S_OK && Extension.Text() == "msi";
}
Result<std::vector<ArchiveHeader>> Headers(IArchiveBackend& Raw, const std::string& Path, bool Test, OperationContext Context) {
    auto List = Raw.Enumerate(Path, Test, Context); if (!List.HasValue()) return List.Failure();
    std::vector<ArchiveHeader> Out;
    for (;;) {
        auto Page = List.Value()->Next(128, Context); if (!Page.HasValue()) return Page.Failure();
        if (Out.size() + Page.Value().entries_.size() > Context.EnumerationLimit()) return Bad("entry-budget", ErrorCategory::BudgetExceeded);
        for (auto& H : Page.Value().entries_) Out.push_back(std::move(H));
        if (Page.Value().end_) return Out;
    }
}
class VectorListing final : public IArchiveListing {
public:
    explicit VectorListing(std::vector<ArchiveHeader> Headers) : headers_(std::move(Headers)) {}
    Result<ArchivePage> Next(size_t Count, OperationContext Context) override {
        if (const auto Stop = Context.Check({"7zip"})) return *Stop;
        if (!Count || !Context.maxPageItems_) return Bad("zero-page-size");
        Count = std::min({Count, Context.maxPageItems_, headers_.size() - offset_});
        ArchivePage Page; Page.entries_.assign(headers_.begin() + offset_, headers_.begin() + offset_ + Count);
        offset_ += Count; Page.end_ = offset_ == headers_.size(); return Page;
    }
private:
    std::vector<ArchiveHeader> headers_; size_t offset_ = 0;
};
// Check every external-source component. The file itself is opened with the
// existing deny-write/reparse policy before its bytes are read.
Result<QString> SourcePath(const Package& Package, const QString& Relative) {
    auto Path = QFileInfo(Package.path_).absolutePath();
    for (const auto& Part : Relative.split('/')) {
        if (!SafeLeaf(Part)) return Bad("unsafe-source-path");
        Path = QDir(Path).filePath(Part);
        const auto Attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(Path).utf16()));
        if (Attr == INVALID_FILE_ATTRIBUTES) return Bad("source-missing:" + Relative.toStdString(), ErrorCategory::NotFound);
        if (Attr & FILE_ATTRIBUTE_REPARSE_POINT) return Bad("source-reparse", ErrorCategory::Unsupported);
    }
    return Path;
}
Result<QString> Cabinet(Package& Package, const QString& Name, QTemporaryDir& Temporary, OperationContext Context) {
    if (!Name.startsWith('#')) return SourcePath(Package, Name);
    try {
        const auto Path = Temporary.filePath(Name.mid(1));
        if (QFileInfo::exists(Path)) return Path;
        Poll(Context); Handle View, Parameter, Record;
        Check(MsiDatabaseOpenViewW(Package.database_.value_, L"SELECT `Data` FROM `_Streams` WHERE `Name` = ?", &View.value_), "stream-query");
        Parameter.value_ = MsiCreateRecord(1); if (!Parameter.value_) throw Bad("stream-record");
        Check(MsiRecordSetStringW(Parameter.value_, 1, reinterpret_cast<LPCWSTR>(Name.mid(1).utf16())), "stream-name");
        Check(MsiViewExecute(View.value_, Parameter.value_), "stream-execute");
        Check(MsiViewFetch(View.value_, &Record.value_), "stream-missing");
        QFile Output(Path); if (!Output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw Bad("cabinet-staging");
        auto Cleanup = qScopeGuard([&] { Output.close(); Output.remove(); });
        QByteArray Buffer(1024 * 1024, 0); uint64_t Total = 0;
        for (;;) {
            Poll(Context); DWORD Size = DWORD(Buffer.size()); Check(MsiRecordReadStream(Record.value_, 1, Buffer.data(), &Size), "stream-read");
            if (!Size) break; Total += Size;
            if (Total > uint64_t(QFileInfo(Package.path_).size())) throw Bad("cabinet-size");
            if (Context.workload_.IsAutomatic() && Total > Context.workload_.automaticFileBytes_) throw Bad("cabinet-budget", ErrorCategory::BudgetExceeded);
            if (Output.write(Buffer.constData(), Size) != Size) throw Bad("cabinet-write");
        }
        if (!Output.flush()) throw Bad("cabinet-flush"); Cleanup.dismiss(); return Path;
    } catch (const Error& E) { return E; }
}
class Adapter final : public IArchiveBackend {
public:
    explicit Adapter(HMODULE Module) : module_(Module), raw_(std::make_shared<Backend>(Module)) {}
    Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Path, bool Test, OperationContext Context) override {
        if (!IsMsi(module_, Path, Context)) return raw_->Enumerate(std::move(Path), Test, Context);
        auto Raw = Headers(*raw_, Path, Test, Context); if (!Raw.HasValue()) return Raw.Failure();
        for (const auto& H : Raw.Value()) if (H.name_ == Root || H.name_.starts_with(std::string(Root) + '/')) return Bad("reserved-browse-path");
        ArchiveHeader Marker; Marker.name_ = Root; Marker.directory_ = true; Marker.attributes_ = FILE_ATTRIBUTE_DIRECTORY; Marker.virtualRoot_ = true;
        auto Data = Load(Path, Context);
        if (!Data.HasValue()) {
            if (Data.Failure().category_ == ErrorCategory::Cancelled || Data.Failure().category_ == ErrorCategory::BudgetExceeded) return Data.Failure();
            Marker.virtualError_ = Data.Failure().code_;
        }
        Raw.Value().push_back(std::move(Marker));
        if (Data.HasValue()) for (const auto& F : Data.Value()->files_) Raw.Value().push_back(F.header_);
        if (Raw.Value().size() > Context.EnumerationLimit()) return Bad("entry-budget", ErrorCategory::BudgetExceeded);
        return std::unique_ptr<IArchiveListing>(new VectorListing(std::move(Raw.Value())));
    }
    Result<Unit> Extract(std::string Path, ArchiveHeader Expected, std::string Output, OperationContext Context) override {
        if (!Expected.name_.starts_with(std::string(Root) + '/') || !IsMsi(module_, Path, Context)) return raw_->Extract(std::move(Path), std::move(Expected), std::move(Output), Context);
        auto Data = Load(Path, Context); if (!Data.HasValue()) return Data.Failure();
        const auto It = std::find_if(Data.Value()->files_.begin(), Data.Value()->files_.end(), [&](const File& F) { return F.header_.name_ == Expected.name_; });
        if (It == Data.Value()->files_.end() || It->header_.size_ != Expected.size_ || Expected.directory_ || Expected.crc_) return Bad("entry-changed", ErrorCategory::StaleRevision);
        QTemporaryDir Temporary; if (!Temporary.isValid()) return Bad("temporary-directory");
        return extractFile(*Data.Value(), *It, Output, Temporary, Context);
    }
    Result<Unit> ExtractAll(std::string Path, std::string Directory, OperationContext Context) override {
        // A whole-materialization request has no namespace selection. Let the
        // provider request individual/selected members instead of duplicating
        // the logical view or requiring external media for raw-stream extraction.
        if (IsMsi(module_, Path, Context)) return Failure("archive-batch-extract-unsupported", ErrorCategory::Unsupported);
        return raw_->ExtractAll(std::move(Path), std::move(Directory), Context);
    }
    Result<Unit> ExtractSelected(std::string Path, std::string Directory, std::vector<uint32_t> Indices, OperationContext Context) override {
        if (!IsMsi(module_, Path, Context)) return raw_->ExtractSelected(std::move(Path), std::move(Directory), std::move(Indices), Context);
        if (Indices.empty() || !std::is_sorted(Indices.begin(), Indices.end()) || std::adjacent_find(Indices.begin(), Indices.end()) != Indices.end()) return Bad("selected-indices");
        const QDir To(QString::fromStdString(Directory));
        if (!To.isAbsolute() || !To.exists() || QFileInfo(To.path()).isSymLink() || !To.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty()) return Bad("private-directory");
        auto Raw = Headers(*raw_, Path, false, Context); if (!Raw.HasValue()) return Raw.Failure();
        if (Indices.back() < Raw.Value().size()) return raw_->ExtractSelected(std::move(Path), std::move(Directory), std::move(Indices), Context);
        auto Data = Load(Path, Context); if (!Data.HasValue()) return Data.Failure();
        const auto Base = Raw.Value().size() + 1;
        if (Indices.back() >= Base + Data.Value()->files_.size()) return Bad("selected-range");
        QTemporaryDir Temporary; if (!Temporary.isValid()) return Bad("temporary-directory");
        std::map<QString, std::vector<uint32_t>> Cabinets;
        for (const auto Index : Indices) {
            if (Index == Base - 1) continue;
            const auto& Header = Index < Raw.Value().size() ? Raw.Value()[Index] : Data.Value()->files_[Index - Base].header_;
            if (Header.directory_) continue;
            if (Index >= Base && !Data.Value()->files_[Index - Base].cabinet_.isEmpty()) {
                Cabinets[Data.Value()->files_[Index - Base].cabinet_].push_back(Index); continue;
            }
            const auto Output = To.filePath(QString::number(Index) + ".bin").toStdString();
            auto Done = Index < Raw.Value().size() ? raw_->Extract(Path, Header, Output, Context) : extractFile(*Data.Value(), Data.Value()->files_[Index - Base], Output, Temporary, Context);
            if (!Done.HasValue()) return Done;
        }
        for (const auto& [Name, Requested] : Cabinets) {
            auto Cab = Cabinet(*Data.Value(), Name, Temporary, Context); if (!Cab.HasValue()) return Cab.Failure();
            auto Members = Headers(*raw_, Cab.Value().toStdString(), false, Context); if (!Members.HasValue()) return Members.Failure();
            std::map<QString, uint32_t> Keys;
            for (uint32_t I = 0; I < Members.Value().size(); ++I)
                if (!Keys.emplace(QString::fromStdString(Members.Value()[I].name_).toCaseFolded(), I).second) return Bad("ambiguous-cabinet-key");
            std::vector<uint32_t> Physical; std::map<uint32_t, uint32_t> Logical;
            for (const auto Index : Requested) {
                const auto& F = Data.Value()->files_[Index - Base]; const auto Found = Keys.find(F.key_.toCaseFolded());
                if (Found == Keys.end()) return Bad("cabinet-file-missing", ErrorCategory::NotFound);
                const auto Ordinal = Found->second; const auto& H = Members.Value()[Ordinal];
                if (H.directory_ || H.size_ != F.header_.size_) return Bad("cabinet-file-size", ErrorCategory::StaleRevision);
                Physical.push_back(Ordinal); Logical.emplace(Ordinal, Index);
            }
            std::sort(Physical.begin(), Physical.end());
            ArchiveSelectionDigest PhysicalDigest; for (const auto Ordinal : Physical) PhysicalDigest.Add(Ordinal, Members.Value()[Ordinal]);
            QTemporaryDir Decoded(Temporary.filePath("decoded-XXXXXX")); if (!Decoded.isValid()) return Bad("temporary-directory");
            // Keep the CAB decoder alive for the entire selected group. Reopening
            // for each MSI file would repeatedly decode the same solid CAB folder.
            auto Done = raw_->ExtractSelected(Cab.Value().toStdString(), Decoded.path().toStdString(), Physical, Context); if (!Done.HasValue()) return Done;
            QFile Receipt(Decoded.filePath("selection.sha256"));
            if (!Receipt.open(QIODevice::ReadOnly) || Receipt.read(65) != PhysicalDigest.Result()) return Bad("cabinet-changed", ErrorCategory::StaleRevision);
            for (const auto& [Ordinal, Index] : Logical) {
                if (const auto Stop = Context.Check({"7zip"})) return *Stop;
                if (!QFile::rename(Decoded.filePath(QString::number(Ordinal) + ".bin"), To.filePath(QString::number(Index) + ".bin"))) return Bad("batch-publish");
            }
        }
        // The receipt uses logical listing order, independent of CAB grouping.
        ArchiveSelectionDigest Ordered;
        for (const auto Index : Indices) {
            if (Index == Base - 1) continue;
            const auto& H = Index < Raw.Value().size() ? Raw.Value()[Index] : Data.Value()->files_[Index - Base].header_;
            if (!H.directory_) Ordered.Add(Index, H);
        }
        return Ordered.Save(To.path());
    }
    Result<std::unique_ptr<IArchiveReadSession>> OpenReadSession(std::string Path, std::vector<uint32_t> Ordinals, OperationContext Context) override {
        if (IsMsi(module_, Path, Context)) return Failure("archive-stream-unsupported", ErrorCategory::Unsupported);
        return raw_->OpenReadSession(std::move(Path), std::move(Ordinals), Context);
    }
    Result<Unit> Update(ArchiveUpdate Request, OperationContext Context) override { return raw_->Update(std::move(Request), Context); }
private:
    Result<Unit> extractFile(Package& Package, const File& File, const std::string& OutputPath, QTemporaryDir& Temporary, OperationContext Context) {
        if (const auto Stop = Context.Check({"7zip"})) return *Stop;
        if (!QFileInfo(QString::fromStdString(OutputPath)).isAbsolute()) return Bad("absolute-output-required");
        if (Context.workload_.IsAutomatic() && File.header_.size_ > Context.workload_.automaticFileBytes_) return Bad("file-budget", ErrorCategory::BudgetExceeded);
        if (!File.cabinet_.isEmpty()) {
            auto Path = Cabinet(Package, File.cabinet_, Temporary, Context); if (!Path.HasValue()) return Path.Failure();
            auto Members = Headers(*raw_, Path.Value().toStdString(), false, Context); if (!Members.HasValue()) return Members.Failure();
            const ArchiveHeader* Selected = nullptr;
            for (const auto& H : Members.Value()) if (QString::fromStdString(H.name_).compare(File.key_, Qt::CaseInsensitive) == 0) {
                if (Selected) return Bad("ambiguous-cabinet-key"); Selected = &H;
            }
            if (!Selected || Selected->directory_ || Selected->size_ != File.header_.size_) return Bad("cabinet-file-missing-or-size", ErrorCategory::Unavailable);
            return raw_->Extract(Path.Value().toStdString(), *Selected, OutputPath, Context);
        }
        auto Path = SourcePath(Package, File.source_); if (!Path.HasValue()) return Path.Failure();
        auto Shared = std::make_shared<State>(); Shared->context_ = Context;
        Com<Input> Source{new Input(Shared)};
        if (!Source->Open(Path.Value()) || Source->Size() != File.header_.size_) return Bad("source-file-changed", ErrorCategory::StaleRevision);
        QFile Output(QString::fromStdString(OutputPath)); if (!Output.open(QIODevice::WriteOnly | QIODevice::Truncate)) return Bad("output-open");
        auto Cleanup = qScopeGuard([&] { Output.close(); Output.remove(); });
        QByteArray Buffer(1024 * 1024, 0);
        for (;;) {
            UInt32 Size = 0; const auto Code = Source->Read(Buffer.data(), UInt32(Buffer.size()), &Size);
            if (Code != S_OK) return Shared->Result(Code); if (!Size) break;
            if (Output.write(Buffer.constData(), Size) != Size) return Bad("output-write");
        }
        if (!Output.flush()) return Bad("output-flush"); Cleanup.dismiss(); return Unit{};
    }
    HMODULE module_; std::shared_ptr<IArchiveBackend> raw_;
};
}
