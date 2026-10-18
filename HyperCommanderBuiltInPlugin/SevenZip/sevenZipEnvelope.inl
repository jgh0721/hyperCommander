// SFX and trailer bytes belong to the container, not to the SDK's archive.
// Keep the source pinned while updating a separate, bare archive candidate.
namespace envelope {
Result<Unit> Copy(Input& Source, uint64_t At, uint64_t Count, QFile& To, OperationContext Context) {
    if (At > INT64_MAX || Source.Seek(Int64(At), 0, nullptr) != S_OK) return Failure("7zip-envelope-seek");
    QByteArray Buffer(1024 * 1024, Qt::Uninitialized);
    while (Count) {
        if (auto Stop = Context.Check({"7zip"})) return *Stop;
        UInt32 Done = 0; const auto Want = UInt32(std::min<uint64_t>(Count, Buffer.size()));
        if (Source.Read(Buffer.data(), Want, &Done) != S_OK || !Done || Done > Want || To.write(Buffer.constData(), Done) != Done) return Failure("7zip-envelope-copy");
        Count -= Done;
    }
    return Unit{};
}
Result<Unit> Same(Input& Source, uint64_t At, uint64_t Count, QFile& To, uint64_t Offset, OperationContext Context) {
    if (At > INT64_MAX || Offset > INT64_MAX || Source.Seek(Int64(At), 0, nullptr) != S_OK || !To.seek(qint64(Offset))) return Failure("7zip-envelope-verify-seek");
    QByteArray Buffer(1024 * 1024, Qt::Uninitialized);
    while (Count) {
        if (auto Stop = Context.Check({"7zip"})) return *Stop;
        UInt32 Done = 0; const auto Want = UInt32(std::min<uint64_t>(Count, Buffer.size()));
        if (Source.Read(Buffer.data(), Want, &Done) != S_OK || !Done || Done > Want || To.read(Done) != QByteArrayView(Buffer.constData(), Done)) return Failure("7zip-envelope-not-preserved");
        Count -= Done;
    }
    return Unit{};
}
Result<Unit> CheckStub(Input& Source, uint64_t Size) {
    // A PE certificate authenticates the archive overlay too. Keeping its bytes
    // after a mutation is not signature preservation; reject signed executables.
    const auto ReadAt = [&](uint64_t At, UInt32 Count) {
        QByteArray Bytes(Count, Qt::Uninitialized); UInt32 Done = 0;
        if (At > Size || Count > Size - At || Source.Seek(Int64(At), 0, nullptr) != S_OK || Source.Read(Bytes.data(), Count, &Done) != S_OK || Done != Count) return QByteArray{};
        return Bytes;
    };
    const auto Dos = ReadAt(0, 64);
    if (Dos.size() < 2 || Dos.first(2) != "MZ") return Unit{};
    if (Dos.size() != 64) return Failure("7zip-sfx-invalid-pe", ErrorCategory::Unsupported);
    const auto At = zipMetadata::U32(Dos, 60); const auto Header = ReadAt(At, 24);
    if (Header.size() != 24 || zipMetadata::U32(Header, 0) != 0x4550) return Failure("7zip-sfx-invalid-pe", ErrorCategory::Unsupported);
    const auto Optional = ReadAt(uint64_t(At) + 24, zipMetadata::U16(Header, 20));
    const auto Magic = Optional.size() >= 2 ? zipMetadata::U16(Optional, 0) : 0;
    const qsizetype Directory = Magic == 0x10b ? 96 : Magic == 0x20b ? 112 : 0;
    if (!Directory || Optional.size() < Directory || (zipMetadata::U32(Optional, Directory - 4) > 4 && Optional.size() < Directory + 40)) return Failure("7zip-sfx-invalid-pe", ErrorCategory::Unsupported);
    if (zipMetadata::U32(Optional, Directory - 4) > 4 && (zipMetadata::U32(Optional, Directory + 32) || zipMetadata::U32(Optional, Directory + 36))) return Failure("7zip-sfx-signature-update", ErrorCategory::Unsupported);
    return Unit{};
}
}
Result<Unit> UpdateArchive(HMODULE Module, ArchiveUpdate Request, OperationContext Context) {
    const auto Target = QString::fromStdString(Request.archive_);
    if (!QFileInfo::exists(Target) || Target.endsWith(".001", Qt::CaseInsensitive)) return UpdatePlainArchive(Module, std::move(Request), Context);
    auto Opened = Open(Module, Request.archive_, Context); if (!Opened.HasValue()) return Opened.Failure(); auto& Original = *Opened.Value();
    Property Offset, Size, Volume;
    if (Original.archive_->GetArchiveProperty(kpidOffset, &Offset) != S_OK || Original.archive_->GetArchiveProperty(kpidPhySize, &Size) != S_OK || Original.archive_->GetArchiveProperty(kpidIsVolume, &Volume) != S_OK) return Failure("7zip-envelope-properties");
    const auto Prefix = Offset.Number(), Body = Size.Number(), Total = Original.input_->Size();
    if (Volume.Boolean() || (Original.format_ != "7z" && Original.format_ != "zip") || (!Prefix && Body == Total)) {
        Opened.Value().reset(); return UpdatePlainArchive(Module, std::move(Request), Context);
    }
    if (Request.creation_ || !Body || Prefix > Total || Body > Total - Prefix) return Failure("7zip-envelope-layout", ErrorCategory::Unsupported);
    auto Stub = envelope::CheckStub(*Original.input_.value_, Prefix); if (!Stub.HasValue()) return Stub;
    const auto Suffix = Total - Prefix - Body;
    if (Prefix > Request.maxOutputBytes_ || Suffix > Request.maxOutputBytes_ - Prefix) return Failure("7zip-output-limit", ErrorCategory::BudgetExceeded);
    QTemporaryDir Stage(QFileInfo(Target).dir().filePath(".hc-envelope-XXXXXX")); if (!Stage.isValid()) return Failure("7zip-envelope-directory");
    const auto Bare = Stage.filePath("body." + Original.format_); QFile File(Bare);
    if (!File.open(QIODevice::ReadWrite | QIODevice::NewOnly)) return Failure("7zip-envelope-open");
    auto Copied = envelope::Copy(*Original.input_.value_, Prefix, Body, File, Context); if (!Copied.HasValue()) return Copied;
    if (Original.format_ == "zip" && Prefix) {
        if (!File.flush()) return Failure("7zip-envelope-flush");
        auto Layout = zipMetadata::Parse(File, Context); if (!Layout.HasValue()) return Layout.Failure();
        // Alignment is absolute to the containing file. The bare ZIP repair is
        // valid only when the unchanged prefix preserves every alignment unit.
        for (const auto& Record : Layout.Value().records_) if (Record.alignment_ && Prefix % Record.alignment_) return Failure("zip-sfx-alignment-unsupported", ErrorCategory::Unsupported);
    }
    File.close();
    auto Inner = Request; Inner.archive_ = Bare.toStdString(); Inner.maxOutputBytes_ -= Prefix + Suffix;
    if (Original.state_->password_) { auto Shared = Original.state_; Context.passwordPrompt_ = [Shared](const std::string&, unsigned, const OperationContext&) -> Result<storage::Password> { return storage::Password(Shared->password_->Value()); }; }
    auto Updated = UpdatePlainArchive(Module, std::move(Inner), Context); if (!Updated.HasValue()) return Updated;
    Com<Input> Payload{new Input(Original.state_)}; if (!Payload->Open(Bare)) return Failure("7zip-envelope-body-open");
    const auto NewBody = Payload->Size(); if (NewBody > Request.maxOutputBytes_ - Prefix - Suffix) return Failure("7zip-output-limit", ErrorCategory::BudgetExceeded);
    const auto Candidate = Stage.filePath("prepared." + Original.format_); QFile Output(Candidate);
    if (!Output.open(QIODevice::ReadWrite | QIODevice::NewOnly)) return Failure("7zip-envelope-output-open");
    Copied = envelope::Copy(*Original.input_.value_, 0, Prefix, Output, Context); if (!Copied.HasValue()) return Copied;
    Copied = envelope::Copy(*Payload.value_, 0, NewBody, Output, Context); if (!Copied.HasValue()) return Copied;
    Copied = envelope::Copy(*Original.input_.value_, Prefix + Body, Suffix, Output, Context); if (!Copied.HasValue()) return Copied;
    if (!Output.flush()) return Failure("7zip-envelope-flush");
    auto Match = envelope::Same(*Original.input_.value_, 0, Prefix, Output, 0, Context); if (!Match.HasValue()) return Match;
    Match = envelope::Same(*Original.input_.value_, Prefix + Body, Suffix, Output, Prefix + NewBody, Context); if (!Match.HasValue()) return Match; Output.close();
    auto Checked = Open(Module, Candidate.toStdString(), Context); if (!Checked.HasValue()) return Checked.Failure();
    Property NewOffset, NewSize;
    if (Checked.Value()->archive_->GetArchiveProperty(kpidOffset, &NewOffset) != S_OK || Checked.Value()->archive_->GetArchiveProperty(kpidPhySize, &NewSize) != S_OK || NewOffset.Number() != Prefix || NewSize.Number() != NewBody) return Failure("7zip-envelope-output-layout");
    Com<ExtractCallback> Test{new ExtractCallback(Checked.Value()->state_, nullptr, UINT32_MAX)};
    const auto Tested = Checked.Value()->archive_->Extract(nullptr, UINT32_MAX, 1, Test.value_);
    if (Tested != S_OK || Checked.Value()->state_->error_) return Checked.Value()->state_->Result(Tested);
    Checked.Value().reset(); Opened.Value().reset();
    if (auto Stop = Context.Check({"7zip"})) return *Stop;
    if (!MoveFileExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Candidate).utf16()), reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Target).utf16()), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return Failure("7zip-envelope-publish:" + std::to_string(GetLastError()));
    return Unit{};
}
