// Included inside the backend namespace: SDK interfaces remain host-private.
struct UpdateItem {
    UInt32 old_ = UINT32_MAX;
    QString name_, path_;
    bool directory_ = false;
    uint64_t size_ = 0;
    DWORD attributes_ = 0;
    FILETIME modified_{};
    QByteArray digest_;
    std::shared_ptr<Input> input_;
    UInt32 metadata_ = UINT32_MAX;
    bool renamed_ = false;
};
QString EntryKey(QString Name) { Name.replace('\\', '/'); if (Name.endsWith('/')) Name.chop(1); return Name; }
bool SafeUpdateName(const QString& Name) {
    if (Name.isEmpty() || Name.startsWith('/') || Name.contains(':') || Name.contains(QChar::Null)) return false;
    for (const auto& Part : Name.split('/')) {
        if (Part.isEmpty() || Part == "." || Part == ".." || Part.endsWith('.') || Part.endsWith(' ')) return false;
        if (Part.contains(QRegularExpression("[<>\"|?*\\x00-\\x1f]"))) return false;
        const auto Base = Part.section('.', 0, 0).toUpper();
        if (Base == "CON" || Base == "PRN" || Base == "AUX" || Base == "NUL" || Base.contains(QRegularExpression("^(COM|LPT)[1-9]$"))) return false;
    }
    return true;
}
class UpdateOutput final : public IOutStream {
public:
    UpdateOutput(QFile* File, uint64_t Limit, std::shared_ptr<State> State) : file_(File), limit_(std::min<uint64_t>(Limit, INT64_MAX)), state_(std::move(State)) {}
    HC_COM_REFS;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
        *Value = nullptr; if (Id == IID_IUnknown || Id == IID_IOutStream || Id == IID_ISequentialOutStream) *Value = static_cast<IOutStream*>(this);
        if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* Data, UInt32 Size, UInt32* Done) noexcept override {
        if (Done) *Done = 0; if (state_->Check(uint64_t(file_->pos())) != S_OK) return E_ABORT;
        if (uint64_t(file_->pos()) > limit_ || Size > limit_ - uint64_t(file_->pos())) return state_->Fail(Failure("7zip-output-limit", ErrorCategory::BudgetExceeded));
        if (file_->write(static_cast<const char*>(Data), Size) != Size) return state_->Fail(Failure("7zip-update-write"));
        if (Done) *Done = Size; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 Offset, UInt32 Origin, UInt64* Position) noexcept override {
        const auto Base = Origin == 0 ? uint64_t(0) : Origin == 1 ? uint64_t(file_->pos()) : uint64_t(file_->size());
        if (Origin > 2 || Base > limit_ || (Offset < 0 ? uint64_t(-(Offset + 1)) + 1 > Base : uint64_t(Offset) > limit_ - Base)) return E_INVALIDARG;
        if (!file_->seek(qint64(Base + Offset))) return E_FAIL; if (Position) *Position = uint64_t(file_->pos()); return state_->Check();
    }
    HRESULT STDMETHODCALLTYPE SetSize(UInt64 Size) noexcept override {
        if (Size > limit_) return state_->Fail(Failure("7zip-output-limit", ErrorCategory::BudgetExceeded));
        if (state_->Check() != S_OK) return E_ABORT; return file_->resize(qint64(Size)) ? S_OK : E_FAIL;
    }
private:
    QFile* file_; uint64_t limit_; std::shared_ptr<State> state_;
};
class UpdateCallback final : public IArchiveUpdateCallback, public ICryptoGetTextPassword2, public ICryptoGetTextPassword {
public:
    UpdateCallback(std::vector<UpdateItem>& Items, std::shared_ptr<State> State, bool Encrypt, bool Zip, IInArchive* Original)
        : items_(Items), state_(std::move(State)), encrypt_(Encrypt), zip_(Zip), original_(Original) {}
    HC_COM_REFS;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
        *Value = nullptr;
        if (Id == IID_IUnknown || Id == IID_IArchiveUpdateCallback || Id == IID_IProgress) *Value = static_cast<IArchiveUpdateCallback*>(this);
        else if (Id == IID_ICryptoGetTextPassword2) *Value = static_cast<ICryptoGetTextPassword2*>(this);
        else if (Id == IID_ICryptoGetTextPassword) *Value = static_cast<ICryptoGetTextPassword*>(this);
        if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return state_->Check(); }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* Done) noexcept override { return state_->Check(Done ? *Done : 0); }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* Value) noexcept override { return state_->Password(Value); }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword2(Int32* Defined, BSTR* Value) noexcept override {
        *Defined = encrypt_ ? 1 : 0; *Value = nullptr; if (!encrypt_) return S_OK;
        const auto Code = state_->Password(Value); if (Code != S_OK) return Code;
        bool Valid = SysStringLen(*Value) != 0;
        // This pinned ZIP handler accepts printable ASCII passwords only.
        if (zip_) for (UINT I = 0; I < SysStringLen(*Value); ++I) if ((*Value)[I] < 32 || (*Value)[I] > 126) Valid = false;
        if (Valid) return S_OK;
        SecureZeroMemory(*Value, SysStringByteLen(*Value)); SysFreeString(*Value); *Value = nullptr;
        return state_->Fail(Failure(zip_ ? "zip-password-printable-ascii-required" : "archive-password-empty", ErrorCategory::InvalidRequest));
    }
    HRESULT STDMETHODCALLTYPE GetUpdateItemInfo(UInt32 Index, Int32* Data, Int32* Props, UInt32* Old) noexcept override {
        if (Index >= items_.size()) return E_INVALIDARG;
        const auto& Item = items_[Index]; *Data = *Props = Item.old_ == UINT32_MAX; *Old = Item.old_; return state_->Check();
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UInt32 Index, PROPID Id, PROPVARIANT* Value) noexcept override {
        PropVariantInit(Value); if (Index >= items_.size()) return E_INVALIDARG; const auto& Item = items_[Index];
        if (Id == kpidPath) { Value->vt = VT_BSTR; Value->bstrVal = SysAllocStringLen(reinterpret_cast<const wchar_t*>(Item.name_.utf16()), UINT(Item.name_.size())); if (!Value->bstrVal) return E_OUTOFMEMORY; }
        else if (Id == kpidIsDir) { Value->vt = VT_BOOL; Value->boolVal = Item.directory_ ? VARIANT_TRUE : VARIANT_FALSE; }
        else if (Id == kpidSize) { Value->vt = VT_UI8; Value->uhVal.QuadPart = Item.size_; }
        else if (Id == kpidAttrib) { Value->vt = VT_UI4; Value->ulVal = Item.attributes_; }
        else if (Id == kpidMTime) { Value->vt = VT_FILETIME; Value->filetime = Item.modified_; }
        else if (Item.metadata_ != UINT32_MAX && (Id == kpidCTime || Id == kpidATime || Id == kpidComment)) {
            if (original_->GetProperty(Item.metadata_, Id, Value) != S_OK) return E_FAIL;
        }
        return state_->Check();
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 Index, ISequentialInStream** Stream) noexcept override {
        *Stream = nullptr; if (Index >= items_.size()) return E_INVALIDARG; const auto& Item = items_[Index];
        if (Item.input_) { if (Item.input_->Seek(0, 0, nullptr) != S_OK) return E_FAIL; Item.input_->AddRef(); *Stream = Item.input_.get(); }
        return state_->Check();
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 Code) noexcept override { return Code == 0 ? state_->Check() : state_->Fail(Failure("7zip-update-result:" + std::to_string(Code))); }
private:
    std::vector<UpdateItem>& items_; std::shared_ptr<State> state_; bool encrypt_, zip_;
    IInArchive* original_;
};
bool SameProperty(const Property& A, const Property& B) {
    if (A.vt != B.vt) return false;
    switch (A.vt) {
    case VT_EMPTY: return true;
    case VT_BSTR: return A.Text() == B.Text();
    case VT_UI4: case VT_UI8: return A.Number() == B.Number();
    case VT_BOOL: return A.Boolean() == B.Boolean();
    case VT_FILETIME: return CompareFileTime(&A.filetime, &B.filetime) == 0;
    default: return false; // Unknown preservation semantics require explicit support.
    }
}
bool SameSolidMethod(const Property& Before, const Property& After) {
    // The SDK reduces the LZMA2 dictionary when a rewritten solid block becomes
    // smaller. Accept only that normalization, not a codec/filter/crypto change.
    const QRegularExpression Pattern("^LZMA2:([0-9]+)([kmb]?)(.*)$");
    const auto A = Pattern.match(Before.Text()), B = Pattern.match(After.Text());
    if (!A.hasMatch() || !B.hasMatch() || A.captured(3) != B.captured(3)) return false;
    const auto Dictionary = [](const QRegularExpressionMatch& M) -> uint64_t {
        bool Ok = false; const auto Number = M.captured(1).toULongLong(&Ok); if (!Ok) return 0;
        const auto Unit = M.captured(2); const unsigned Shift = Unit == "k" ? 10 : Unit == "m" ? 20 : Unit == "b" ? 0 : 64;
        if (Shift == 64) return Number < 63 ? uint64_t(1) << Number : 0;
        return Number <= (UINT64_MAX >> Shift) ? Number << Shift : 0;
    };
    const auto Old = Dictionary(A), New = Dictionary(B); return Old && New && New <= Old;
}
class VerifyCallback final : public IArchiveExtractCallback, public ICryptoGetTextPassword {
public:
    VerifyCallback(std::shared_ptr<State> State, const std::map<UInt32, const UpdateItem*>& NewItems) : state_(std::move(State)), items_(NewItems) {}
    ~VerifyCallback() { if (output_) output_->Release(); }
    HC_COM_REFS;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
        *Value = nullptr;
        if (Id == IID_IUnknown || Id == IID_IArchiveExtractCallback || Id == IID_IProgress) *Value = static_cast<IArchiveExtractCallback*>(this);
        else if (Id == IID_ICryptoGetTextPassword) *Value = static_cast<ICryptoGetTextPassword*>(this);
        if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return state_->Check(); }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* Done) noexcept override { return state_->Check(Done ? *Done : 0); }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* Value) noexcept override { return state_->Password(Value); }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 Index, ISequentialOutStream** Stream, Int32 Mode) noexcept override {
        *Stream = nullptr; if (output_) { output_->Release(); output_ = nullptr; } item_ = nullptr;
        if (Mode == NArchive::NExtract::NAskMode::kExtract && items_.contains(Index)) {
            item_ = items_.at(Index); output_ = new Output(nullptr, item_->size_, state_, true); output_->AddRef(); *Stream = output_;
        }
        return state_->Check();
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override { return state_->Check(); }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 Code) noexcept override {
        if (Code) return state_->Fail(Failure("7zip-verify-result:" + std::to_string(Code)));
        if (item_) {
            if (output_->written_ != item_->size_ || output_->hash_.result() != item_->digest_) return state_->Fail(Failure("7zip-input-output-digest"));
            ++verified_;
        }
        return state_->Check();
    }
    size_t verified_ = 0;
private:
    std::shared_ptr<State> state_; const std::map<UInt32, const UpdateItem*>& items_; const UpdateItem* item_ = nullptr; Output* output_ = nullptr;
};
#include "sevenZipMetadata.inl"
Result<Unit> UpdateArchive(HMODULE Module, ArchiveUpdate Request, OperationContext Context) {
    if (const auto Stop = Context.Check({"7zip"})) return *Stop;
    const auto Target = QString::fromStdString(Request.archive_);
    const auto Root = QDir::cleanPath(QString::fromStdString(Request.inputDirectory_));
    if (!QFileInfo(Target).isAbsolute() || !QFileInfo(Root).isAbsolute() || Target.contains(QChar::Null) || Root.contains(QChar::Null)
        || (Request.add_.empty() && Request.remove_.empty()) || !Request.maxOutputBytes_)
        return Failure("7zip-invalid-update", ErrorCategory::InvalidRequest);
    const bool Existing = QFileInfo::exists(Target);
    std::unique_ptr<Archive> Original;
    auto Shared = std::make_shared<State>(); Shared->context_ = Context; Shared->archive_ = Request.archive_;
    QString Format = QFileInfo(Target).suffix().toLower(), Method, Encryption;
    bool EncryptHeaders = false;
    bool Solid = Request.creation_.value_or(ArchiveCreationOptions{}).solid_;
    if (Request.creation_ && (Existing || Request.creation_->level_ > 9)) return Failure("7zip-creation-options-on-update", ErrorCategory::InvalidRequest);
    if (Existing) {
        if (Target.endsWith(".001", Qt::CaseInsensitive)) return Failure("7zip-split-update-unsupported", ErrorCategory::Unsupported);
        auto Opened = Open(Module, Request.archive_, Context); if (!Opened.HasValue()) return Opened.Failure(); Original = std::move(Opened.Value());
        Format = Original->format_; Shared = Original->state_;
        if (Format != "7z" && Format != "zip") return Failure("7zip-format-read-only", ErrorCategory::Unsupported);
        // 26.03 skips unknown FilesInfo properties (including experimental ACL
        // records) but CanUpdate() ignores UnsupportedFeatureWarning. Its ACL
        // reader/writer and raw-property exposure are commented out upstream.
        // Check the decoded header warning, including encrypted/encoded headers,
        // before any writer can silently discard properties it cannot represent.
        if (Format == "7z") {
            Property Warnings, ReadOnly;
            if (Original->archive_->GetArchiveProperty(kpidWarningFlags, &Warnings) != S_OK || Original->archive_->GetArchiveProperty(kpidReadOnly, &ReadOnly) != S_OK)
                return Failure("7zip-metadata-capability-query");
            if (Warnings.Number() || ReadOnly.Boolean()) return Failure("7zip-unrepresented-metadata-update", ErrorCategory::Unsupported);
        }
        Property Offset, Size, Volume, SolidProp, MethodProp;
        for (const auto Pair : {std::pair<PROPID, Property*>{kpidOffset, &Offset}, {kpidPhySize, &Size}, {kpidIsVolume, &Volume}, {kpidSolid, &SolidProp}, {kpidMethod, &MethodProp}})
            if (Original->archive_->GetArchiveProperty(Pair.first, Pair.second) != S_OK) return Failure("7zip-update-properties");
        if (Offset.Number() || Size.Number() != Original->input_->Size() || Volume.Boolean())
            return Failure("7zip-update-envelope-unsupported", ErrorCategory::Unsupported);
        EncryptHeaders = Shared->password_.has_value();
        Request.encrypt_ = Request.encrypt_ || EncryptHeaders;
        Solid = SolidProp.Boolean(); Method = MethodProp.Text();
        if (Format == "7z") Method.remove(QRegularExpression(" 7zAES$"));
        if (Format == "7z" && !Method.isEmpty() && !QRegularExpression("^(LZMA2:[0-9]+[kmb]?|Copy)$").match(Method).hasMatch())
            return Failure("7zip-update-method-preservation-unsupported:" + Method.toStdString(), ErrorCategory::Unsupported);
    } else if (Format != "7z" && Format != "zip") return Failure("7zip-create-format", ErrorCategory::Unsupported);

    std::map<QString, UInt32> OldNames; std::set<QString> Removed, Names;
    std::map<QString, QString> Origins; std::set<QString> OriginNames;
    std::vector<UpdateItem> Items;
    for (const auto& Name : Request.remove_) { const auto Key = EntryKey(QString::fromStdString(Name)); if (!SafeUpdateName(Key) || !Removed.insert(Key).second) return Failure("7zip-remove-name", ErrorCategory::InvalidRequest); }
    if (!Request.metadataOrigins_.empty() && Request.metadataOrigins_.size() != Request.add_.size()) return Failure("7zip-metadata-origin-count", ErrorCategory::InvalidRequest);
    for (size_t I = 0; I < Request.add_.size(); ++I) {
        const auto Name = EntryKey(QString::fromStdString(Request.add_[I]));
        auto Origin = Request.metadataOrigins_.empty() ? QString{} : EntryKey(QString::fromStdString(Request.metadataOrigins_[I]));
        if (Origin.isEmpty() && Removed.contains(Name)) Origin = Name;
        if (!Origin.isEmpty()) {
            if (!Removed.contains(Origin) || !SafeUpdateName(Origin) || !OriginNames.insert(Origin).second || !Origins.emplace(Name, Origin).second)
                return Failure("7zip-metadata-origin-invalid", ErrorCategory::InvalidRequest);
        }
    }
    if (Original) for (UInt32 I = 0; I < Original->count_; ++I) {
        if (const auto Stop = Context.Check({"7zip"})) return *Stop;
        auto Header = Original->Header(I); if (!Header.HasValue()) return Header.Failure(); const auto& H = Header.Value(); const auto Key = EntryKey(QString::fromStdString(H.name_));
        if (!SafeUpdateName(Key) || !OldNames.emplace(Key, I).second || !Names.insert(Key.toCaseFolded()).second) return Failure("7zip-ambiguous-update-name", ErrorCategory::Unsupported);
        if (H.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) return Failure("7zip-link-update-unsupported", ErrorCategory::Unsupported);
        Property Posix; if (Original->archive_->GetProperty(I, kpidPosixAttrib, &Posix) != S_OK) return Failure("7zip-posix-property");
        const auto Mode = Posix.Number() ? Posix.Number() : (H.attributes_ & 0x8000) ? H.attributes_ >> 16 : 0;
        const auto Kind = Mode & 0170000;
        if (Kind && Kind != (H.directory_ ? 0040000 : 0100000)) return Failure("7zip-special-file-update-unsupported", ErrorCategory::Unsupported);
        if (H.encrypted_) {
            Request.encrypt_ = true;
            if (Format == "zip") {
                Property P; if (Original->archive_->GetProperty(I, kpidMethod, &P) != S_OK) return Failure("zip-encryption-property");
                const auto Match = QRegularExpression("^(AES-(128|192|256)|ZipCrypto)( |$)").match(P.Text());
                if (!Match.hasMatch()) return Failure("zip-encryption-preservation-unsupported", ErrorCategory::Unsupported);
                auto Mode = Match.captured(1); Mode.remove('-');
                if (!Encryption.isEmpty() && Encryption != Mode) return Failure("zip-mixed-encryption-update-unsupported", ErrorCategory::Unsupported);
                Encryption = Mode;
            }
        }
        // Explicit origins distinguish rename/replacement from unrelated adds.
        // POSIX mode follows the original attributes; native security properties
        // still need a format-specific writer (ZIP ACL extras are copied below).
        if (Removed.contains(Key) && !Request.add_.empty()) {
            for (const auto Id : {kpidComment, kpidCTime, kpidATime, kpidPosixAttrib, kpidNtSecure}) {
                Property P; if (Original->archive_->GetProperty(I, Id, &P) != S_OK || (P.vt != VT_EMPTY && !(P.vt == VT_BSTR && P.Text().isEmpty()) && !(P.vt == VT_FILETIME && !P.filetime.dwHighDateTime && !P.filetime.dwLowDateTime)
                    && (Id == kpidNtSecure || !OriginNames.contains(Key))))
                    return Failure("7zip-replaced-metadata-preservation-unsupported:" + std::to_string(Id) + ":" + std::to_string(P.vt), ErrorCategory::Unsupported);
            }
        }
        if (!Removed.contains(Key)) Items.push_back({I, Key, {}, H.directory_, H.size_});
    }
    for (const auto& Name : Removed) if (!OldNames.contains(Name)) return Failure("7zip-remove-missing", ErrorCategory::NotFound);
    if (Original && Request.encrypt_) {
        // Authenticate all existing payloads before asking the writer to retain
        // or repack them. One cached password is used for read and write callbacks.
        Com<ExtractCallback> Test{new ExtractCallback(Shared, nullptr, UINT32_MAX)};
        const auto Code = Original->archive_->Extract(nullptr, UINT32_MAX, 1, Test.value_);
        if (Code != S_OK || Shared->error_) return Shared->Result(Code);
    }
    Names.clear(); for (const auto& Item : Items) Names.insert(Item.name_.toCaseFolded());
    for (const auto& Raw : Request.add_) {
        if (const auto Stop = Context.Check({"7zip"})) return *Stop;
        const auto Name = EntryKey(QString::fromStdString(Raw));
        if (!SafeUpdateName(Name) || !Names.insert(Name.toCaseFolded()).second) return Failure("7zip-add-name-collision", ErrorCategory::InvalidRequest);
        UpdateItem Item; Item.name_ = Name; Item.path_ = QDir(Root).filePath(Name); Item.directory_ = Raw.ends_with('/') || Raw.ends_with('\\');
        // Check every ancestor, not only the final file, before pinning input.
        for (auto Path = Item.path_;; Path = QFileInfo(Path).absolutePath()) {
            const auto Attr = GetFileAttributesW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Path).utf16()));
            if (Attr == INVALID_FILE_ATTRIBUTES || (Attr & FILE_ATTRIBUTE_REPARSE_POINT)) return Failure("7zip-input-link-or-missing", ErrorCategory::Unsupported);
            if (Path == Root) break;
            if (!Path.startsWith(Root.endsWith('/') ? Root : Root + '/', Qt::CaseInsensitive)) return Failure("7zip-input-outside-root", ErrorCategory::InvalidRequest);
        }
        WIN32_FILE_ATTRIBUTE_DATA Info{};
        if (!GetFileAttributesExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Item.path_).utf16()), GetFileExInfoStandard, &Info)
            || bool(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != Item.directory_) return Failure("7zip-input-kind", ErrorCategory::InvalidRequest);
        Item.attributes_ = Info.dwFileAttributes; Item.modified_ = Info.ftLastWriteTime;
        if (Origins.contains(Name)) {
            Item.metadata_ = OldNames.at(Origins.at(Name));
            Item.renamed_ = Origins.at(Name) != Name;
            auto Old = Original->Header(Item.metadata_); if (!Old.HasValue()) return Old.Failure();
            if (Old.Value().directory_ != Item.directory_) return Failure("7zip-metadata-origin-kind", ErrorCategory::InvalidRequest);
            Property Attributes, Time;
            if (Original->archive_->GetProperty(Item.metadata_, kpidAttrib, &Attributes) != S_OK || Original->archive_->GetProperty(Item.metadata_, kpidMTime, &Time) != S_OK) return Failure("7zip-metadata-origin-properties");
            if (Attributes.vt == VT_UI4) Item.attributes_ = Attributes.ulVal;
            if (Origins.at(Name) != Name && Time.vt == VT_FILETIME) Item.modified_ = Time.filetime;
        }
        if (!Item.directory_) {
            Item.input_ = std::shared_ptr<Input>(new Input(Shared), [](Input* Value) { Value->Release(); });
            if (!Item.input_->Open(Item.path_)) return Failure("7zip-input-pin"); Item.size_ = Item.input_->Size();
            if (Context.workload_.IsAutomatic() && Item.size_ > Context.workload_.automaticFileBytes_) return Failure("automatic-content-budget", ErrorCategory::BudgetExceeded);
            QCryptographicHash Hash(QCryptographicHash::Sha256); QByteArray Buffer(1024 * 1024, Qt::Uninitialized);
            for (;;) { UInt32 Done = 0; const auto Code = Item.input_->Read(Buffer.data(), UInt32(Buffer.size()), &Done); if (Code != S_OK) return Shared->Result(Code); if (!Done) break; Hash.addData(QByteArrayView(Buffer.constData(), Done)); }
            Item.digest_ = Hash.result();
        }
        Items.push_back(std::move(Item));
    }
    if (Items.size() > UINT32_MAX || Items.size() > Context.EnumerationLimit()) return Failure("7zip-sdk-item-count", ErrorCategory::BudgetExceeded);
    std::set<QString> Files; for (const auto& Item : Items) if (!Item.directory_) Files.insert(Item.name_.toCaseFolded());
    for (const auto& Item : Items) for (auto Parent = Item.name_.section('/', 0, -2); !Parent.isEmpty(); Parent = Parent.section('/', 0, -2))
        if (Files.contains(Parent.toCaseFolded())) return Failure("7zip-file-parent", ErrorCategory::InvalidRequest);

    Com<IOutArchive> Writer;
    if (Original) {
        if (Original->archive_->QueryInterface(IID_IOutArchive, reinterpret_cast<void**>(&Writer.value_)) != S_OK) return Failure("7zip-update-interface", ErrorCategory::Unsupported);
    } else {
        const auto Get = reinterpret_cast<HRESULT(WINAPI*)(UInt32, PROPID, PROPVARIANT*)>(GetProcAddress(Module, "GetHandlerProperty2"));
        const auto Count = reinterpret_cast<HRESULT(WINAPI*)(UInt32*)>(GetProcAddress(Module, "GetNumberOfFormats"));
        const auto Create = reinterpret_cast<HRESULT(WINAPI*)(const GUID*, const GUID*, void**)>(GetProcAddress(Module, "CreateObject"));
        UInt32 N = 0; if (!Get || !Count || !Create || Count(&N) != S_OK) return Failure("7zip-exports");
        for (UInt32 I = 0; I < N; ++I) { Property Name, Id; if (Get(I, NArchive::NHandlerPropID::kName, &Name) != S_OK || Name.Text().compare(Format, Qt::CaseInsensitive)) continue;
            if (Get(I, NArchive::NHandlerPropID::kClassID, &Id) != S_OK || Id.vt != VT_BSTR || SysStringByteLen(Id.bstrVal) != sizeof(GUID)) return Failure("7zip-format-guid");
            GUID Class{}; memcpy(&Class, Id.bstrVal, sizeof(Class)); if (Create(&Class, &IID_IOutArchive, reinterpret_cast<void**>(&Writer.value_)) != S_OK) return Failure("7zip-create-interface"); break; }
        if (!Writer.value_) return Failure("7zip-create-format", ErrorCategory::Unsupported);
    }
    Com<ISetProperties> Properties;
    if (Writer->QueryInterface(IID_ISetProperties, reinterpret_cast<void**>(&Properties.value_)) != S_OK) return Failure("7zip-set-properties");
    std::vector<std::wstring> Keys, Values;
    auto Set = [&](const wchar_t* Key, QString Value) { Keys.emplace_back(Key); Values.push_back(Value.toStdWString()); };
    Set(L"x", QString::number(Request.creation_.value_or(ArchiveCreationOptions{}).level_));
    if (!Origins.empty()) { Set(L"tc", "on"); Set(L"ta", "on"); }
    if (Format == "7z") {
        Set(L"s", Solid ? "on" : "off"); Set(L"f", "off");
        if (Method == "Copy" || (!Original && Request.creation_ && Request.creation_->level_ == 0)) Set(L"0", "Copy");
        else if (Method.startsWith("LZMA2:")) Set(L"0", "LZMA2:d" + Method.mid(6));
        else Set(L"0", "LZMA2");
        if (Request.encrypt_) Set(L"he", EncryptHeaders || !Original ? "on" : "off");
    } else { Set(L"cu", "on"); if (Request.encrypt_) Set(L"em", Encryption.isEmpty() ? "AES256" : Encryption); }
    std::vector<const wchar_t*> KeyPointers; std::vector<PROPVARIANT> Variants(Values.size());
    for (size_t I = 0; I < Values.size(); ++I) {
        KeyPointers.push_back(Keys[I].c_str()); PropVariantInit(&Variants[I]);
        if (Keys[I] == L"x") { Variants[I].vt = VT_UI4; Variants[I].ulVal = Request.creation_.value_or(ArchiveCreationOptions{}).level_; }
        else if (Values[I] == L"on" || Values[I] == L"off") { Variants[I].vt = VT_BOOL; Variants[I].boolVal = Values[I] == L"on" ? VARIANT_TRUE : VARIANT_FALSE; }
        else { Variants[I].vt = VT_BSTR; Variants[I].bstrVal = SysAllocString(Values[I].c_str()); }
    }
    const auto Settings = Properties->SetProperties(KeyPointers.data(), Variants.data(), UInt32(Variants.size()));
    for (auto& Value : Variants) PropVariantClear(&Value);
    if (Settings != S_OK) return Failure("7zip-compression-options");
    QTemporaryDir Staging(QFileInfo(Target).dir().filePath(".hc-7zip-XXXXXX"));
    if (!Staging.isValid()) return Failure("7zip-prepared-directory");
    QFile Prepared(Staging.filePath("archive." + Format));
    if (!Prepared.open(QIODevice::ReadWrite | QIODevice::NewOnly)) return Failure("7zip-prepared-open");
    Com<UpdateOutput> Stream{new UpdateOutput(&Prepared, Request.maxOutputBytes_, Shared)};
    Com<UpdateCallback> Callback{new UpdateCallback(Items, Shared, Request.encrypt_, Format == "zip", Original ? Original->archive_.value_ : nullptr)};
    const auto Code = Writer->UpdateItems(Stream.value_, UInt32(Items.size()), Callback.value_);
    if (Code != S_OK || Shared->error_) return Shared->Result(Code);
    if (!Prepared.flush()) return Failure("7zip-prepared-flush"); Prepared.close();
    auto VerifyContext = Context;
    if (Shared->password_) VerifyContext.passwordPrompt_ = [Shared](const std::string&, unsigned, const OperationContext&) -> Result<storage::Password> { return storage::Password(Shared->password_->Value()); };
    std::map<QString, const UpdateItem*> Expected; for (const auto& Item : Items) Expected.emplace(Item.name_, &Item);
    if (Original && Format == "zip" && !Origins.empty()) {
        auto Fresh = Open(Module, Prepared.fileName().toStdString(), VerifyContext); if (!Fresh.HasValue()) return Fresh.Failure();
        auto Restored = zipMetadata::Restore(*Original, *Fresh.Value(), Prepared.fileName(), Expected, Request.maxOutputBytes_, Context);
        if (!Restored.HasValue()) return Restored;
    }
    auto Checked = Open(Module, Prepared.fileName().toStdString(), VerifyContext); if (!Checked.HasValue()) return Checked.Failure(); auto& Verified = *Checked.Value();
    if (Verified.count_ != Items.size() || Verified.format_ != Format) return Failure("7zip-output-entry-count");
    std::map<UInt32, const UpdateItem*> NewItems;
    if (Original) for (const auto Id : {kpidComment}) { Property Before, After; if (Original->archive_->GetArchiveProperty(Id, &Before) != S_OK || Verified.archive_->GetArchiveProperty(Id, &After) != S_OK || !SameProperty(Before, After)) return Failure("7zip-archive-metadata-not-preserved", ErrorCategory::Unsupported); }
    for (UInt32 I = 0; I < Verified.count_; ++I) {
        auto Header = Verified.Header(I); if (!Header.HasValue()) return Header.Failure(); const auto& H = Header.Value(); const auto Found = Expected.find(EntryKey(QString::fromStdString(H.name_)));
        if (Found == Expected.end()) return Failure("7zip-output-name"); const auto& Item = *Found->second;
        if (H.directory_ != Item.directory_ || (!H.directory_ && (H.size_ != Item.size_ || (Item.old_ == UINT32_MAX && Request.encrypt_ && Item.size_ && !H.encrypted_)))) return Failure("7zip-output-content");
        if (Item.old_ != UINT32_MAX) for (const auto Id : {kpidCRC, kpidAttrib, kpidMTime, kpidCTime, kpidATime, kpidComment, kpidMethod, kpidEncrypted, kpidPosixAttrib, kpidNtSecure}) {
            Property Before, After;
            if (Original->archive_->GetProperty(Item.old_, Id, &Before) != S_OK || Verified.archive_->GetProperty(I, Id, &After) != S_OK
                || (!SameProperty(Before, After) && !(Id == kpidMethod && Format == "7z" && Solid && SameSolidMethod(Before, After))))
                return Failure("7zip-entry-metadata-not-preserved:" + std::to_string(Id), ErrorCategory::Unsupported);
        }
        if (Item.old_ == UINT32_MAX) {
            const auto Ticks = (uint64_t(Item.modified_.dwHighDateTime) << 32) | Item.modified_.dwLowDateTime;
            const auto Milliseconds = int64_t(Ticks / 10000) - 11644473600000LL;
            constexpr DWORD PortableAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE;
            if (std::abs(H.modifiedUtcMs_ - Milliseconds) > (Format == "zip" ? 2000 : 0)
                || (H.attributes_ & PortableAttributes) != (Item.attributes_ & PortableAttributes)) return Failure("7zip-input-metadata-not-preserved", ErrorCategory::Unsupported);
        }
        if (Item.metadata_ != UINT32_MAX) for (const auto Id : {kpidComment, kpidCTime, kpidATime, kpidPosixAttrib}) {
            Property Before, After;
            if (Original->archive_->GetProperty(Item.metadata_, Id, &Before) != S_OK || Verified.archive_->GetProperty(I, Id, &After) != S_OK || !SameProperty(Before, After))
                return Failure("7zip-replaced-metadata-not-preserved:" + std::to_string(Id), ErrorCategory::Unsupported);
        }
        if (Item.old_ == UINT32_MAX && !Item.directory_) NewItems.emplace(I, &Item);
        Expected.erase(Found);
    }
    if (!NewItems.empty()) {
        std::vector<UInt32> Indices; for (const auto& [Index, Item] : NewItems) Indices.push_back(Index);
        Com<VerifyCallback> Compare{new VerifyCallback(Verified.state_, NewItems)};
        const auto Compared = Verified.archive_->Extract(Indices.data(), UInt32(Indices.size()), 0, Compare.value_);
        if (Compared != S_OK || Verified.state_->error_) return Verified.state_->Result(Compared);
        if (Compare->verified_ != NewItems.size()) return Failure("7zip-output-not-verified");
    }
    // Single full pass checks retained compressed payloads, including solid blocks.
    Com<ExtractCallback> Test{new ExtractCallback(Verified.state_, nullptr, UINT32_MAX)};
    const auto Tested = Verified.archive_->Extract(nullptr, UINT32_MAX, 1, Test.value_);
    if (Tested != S_OK || Verified.state_->error_) return Verified.state_->Result(Tested);
    Checked.Value().reset(); Properties.value_->Release(); Properties.value_ = nullptr; Writer.value_->Release(); Writer.value_ = nullptr; Original.reset();
    if (const auto Stop = Context.Check({"7zip"})) return *Stop;
    // This only publishes the verified private candidate. The common operation
    // coordinator owns revision checks and atomic publication of the real target.
    if (!MoveFileExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Prepared.fileName()).utf16()),
        reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Target).utf16()), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return Failure("7zip-prepared-publish:" + std::to_string(GetLastError()));
    return Unit{};
}
