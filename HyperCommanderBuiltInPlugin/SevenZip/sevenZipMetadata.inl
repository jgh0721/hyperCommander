// Host-private ZIP metadata repair. Layout follows PKWARE APPNOTE 6.3.10:
// https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT
// Payloads and descriptors are copied in bounded chunks; only headers reside in
// memory. The repaired candidate still passes the SDK's full integrity checks.
namespace zipMetadata {
uint16_t U16(const QByteArray& B, qsizetype At) { return qFromLittleEndian<quint16>(B.constData() + At); }
uint32_t U32(const QByteArray& B, qsizetype At) { return qFromLittleEndian<quint32>(B.constData() + At); }
uint64_t U64(const QByteArray& B, qsizetype At) { return qFromLittleEndian<quint64>(B.constData() + At); }
void Put16(QByteArray& B, qsizetype At, uint16_t V) { qToLittleEndian<quint16>(V, B.data() + At); }
void Put32(QByteArray& B, qsizetype At, uint32_t V) { qToLittleEndian<quint32>(V, B.data() + At); }
void Put64(QByteArray& B, qsizetype At, uint64_t V) { qToLittleEndian<quint64>(V, B.data() + At); }
Error Bad() { return Failure("zip-metadata-layout", ErrorCategory::Unsupported); }
QByteArray Read(QFile& File, uint64_t At, qsizetype Size) {
    if (At > uint64_t(File.size()) || Size < 0 || uint64_t(Size) > uint64_t(File.size()) - At || !File.seek(qint64(At))) return {};
    return File.read(Size);
}
struct Field { uint16_t id_; QByteArray data_; };
Result<std::vector<Field>> Fields(const QByteArray& Bytes) {
    std::vector<Field> Out;
    for (qsizetype At = 0; At < Bytes.size();) {
        if (Bytes.size() - At < 4) return Bad(); const auto Id = U16(Bytes, At), Size = U16(Bytes, At + 2); At += 4;
        if (Size > Bytes.size() - At) return Bad(); Out.push_back({Id, Bytes.mid(At, Size)}); At += Size;
    }
    return Out;
}
QByteArray Encode(const std::vector<Field>& Fields) {
    QByteArray Out;
    for (const auto& F : Fields) { QByteArray Header(4, 0); Put16(Header, 0, F.id_); Put16(Header, 2, uint16_t(F.data_.size())); Out += Header; Out += F.data_; }
    return Out;
}
bool Generated(uint16_t Id) { return Id == 1 || Id == 0x5455 || Id == 0x7075 || Id == 0x6375 || Id == 0x9901; }
Result<QByteArray> XceedName(const QByteArray& Data, const QString& Name) {
    // Xceed's NUCX layout: local has name length; central adds comment length.
    // https://github.com/pmqs/zipdetails/blob/main/bin/zipdetails
    if (Data.size() < 6 || Data.first(4) != "NUCX") return Bad();
    const auto Length = qsizetype(U16(Data, 4)) * 2;
    const bool Central = Data.size() != 6 + Length;
    if (Central && (Data.size() < 8 || Data.size() != 8 + Length + qsizetype(U16(Data, 6)) * 2)) return Bad();
    if (Name.size() > UINT16_MAX) return Bad();
    QByteArray Out = Data.first(Central ? 8 : 6); Put16(Out, 4, uint16_t(Name.size()));
    QByteArray Encoded(Name.size() * 2, 0); for (qsizetype I = 0; I < Name.size(); ++I) Put16(Encoded, I * 2, Name[I].unicode());
    Out += Encoded; if (Central) Out += Data.mid(8 + Length); return Out;
}
Result<QByteArray> Persistent(const QByteArray& Bytes) {
    auto Parsed = Fields(Bytes); if (!Parsed.HasValue()) return Parsed.Failure(); std::vector<Field> Out;
    for (auto F : Parsed.Value()) {
        if (Generated(F.id_)) continue;
        // These records bind to payload hashes, signatures, names or physical
        // alignment; copying their bytes would falsely claim valid semantics.
        // Legacy UNIX times and alternate-name records need their own rewrite
        // rules as well; preserving stale values would mislead other readers.
        if ((F.id_ >= 0x0014 && F.id_ <= 0x0019) || F.id_ == 0x000d || F.id_ == 0x756e
            || F.id_ == 0x4f4c || F.id_ == 0x2605)
            return Failure("zip-dependent-extra-unsupported:" + std::to_string(F.id_), ErrorCategory::Unsupported);
        if (F.id_ == 0xa11e || F.id_ == 0xd935) { if (F.data_.size() < 2) return Bad(); F.data_.truncate(2); }
        if (F.id_ == 0x4b46) { if (F.data_.size() != 19 || F.data_.first(3) != "MD5") return Bad(); F.data_ = "MD5" + QByteArray(16, 0); }
        if (F.id_ == 0x5855) {
            if (F.data_.size() != 8 && F.data_.size() != 12) return Bad();
            Put32(F.data_, 4, 0); // mtime is rewritten; atime and UID/GID stay exact.
        }
        if (F.id_ == 0x554e) { auto Normal = XceedName(F.data_, {}); if (!Normal.HasValue()) return Normal.Failure(); F.data_ = std::move(Normal.Value()); }
        if (F.id_ == 0x000a) {
            if (F.data_.size() < 4) return Bad(); auto Tags = Fields(F.data_.mid(4)); if (!Tags.HasValue()) return Tags.Failure();
            std::vector<Field> Unknown;
            for (const auto& Tag : Tags.Value()) { if (Tag.id_ == 1) { if (Tag.data_.size() != 24) return Bad(); } else Unknown.push_back(Tag); }
            F.data_ = F.data_.first(4) + Encode(Unknown);
            if (Unknown.empty() && F.data_ == QByteArray(4, 0)) continue;
        }
        Out.push_back(std::move(F));
    }
    return Encode(Out);
}
Result<QByteArray> Merge(const QByteArray& Fresh, const QByteArray& Old, const UpdateItem& Item) {
    const auto Modified = (uint64_t(Item.modified_.dwHighDateTime) << 32) | Item.modified_.dwLowDateTime;
    auto Keep = Persistent(Old); if (!Keep.HasValue()) return Keep.Failure();
    auto A = Fields(Fresh), B = Fields(Keep.Value()), Original = Fields(Old); if (!A.HasValue() || !B.HasValue() || !Original.HasValue()) return Bad();
    std::vector<Field> Out; QByteArray Times;
    for (const auto& F : A.Value()) {
        if (F.id_ == 0x000a) {
            if (F.data_.size() < 4) return Bad(); auto Tags = Fields(F.data_.mid(4)); if (!Tags.HasValue()) return Bad();
            for (const auto& Tag : Tags.Value()) if (Tag.id_ == 1) Times += Encode({Tag});
        } else if (Generated(F.id_) && F.id_ != 0x5455) Out.push_back(F);
    }
    bool HadTimes = false;
    for (auto F : Original.Value()) {
        if (F.id_ == 0x5855) {
            if (!Item.renamed_) {
                if (Modified < 116444736000000000ULL || (Modified - 116444736000000000ULL) / 10000000 > UINT32_MAX) return Bad();
                Put32(F.data_, 4, uint32_t((Modified - 116444736000000000ULL) / 10000000));
            }
            Out.push_back(std::move(F));
        }
        if (F.id_ == 0x554e) { auto Name = XceedName(F.data_, Item.name_ + (Item.directory_ ? "/" : "")); if (!Name.HasValue()) return Name.Failure(); F.data_ = std::move(Name.Value()); Out.push_back(std::move(F)); }
        if (F.id_ == 0x000a) {
            auto Tags = Fields(F.data_.mid(4)); if (!Tags.HasValue()) return Bad();
            for (const auto& Tag : Tags.Value()) if (Tag.id_ == 1) HadTimes = true;
        }
        if (F.id_ == 0x5455) {
            if (F.data_.isEmpty()) return Bad();
            // Retain which UNIX times were present, including header-specific
            // flags. Replace only mtime, using the pinned input's FILETIME.
            if (quint8(F.data_[0]) & 1) {
                if (F.data_.size() < 5 || Modified < 116444736000000000ULL || (Modified - 116444736000000000ULL) / 10000000 > UINT32_MAX) return Bad();
                Put32(F.data_, 1, uint32_t((Modified - 116444736000000000ULL) / 10000000));
            }
            Out.push_back(std::move(F));
        }
    }
    // Adding an NTFS block to a DOS/UNIX-only entry would invent zero-valued
    // creation/access properties. Keep their original absence instead.
    if (!HadTimes) Times.clear();
    bool Ntfs = false;
    for (auto F : B.Value()) { if (F.id_ == 0x5855 || F.id_ == 0x554e) continue; if (F.id_ == 0x4b46) { if (Item.reuseData_) { for (const auto& Source : Original.Value()) if (Source.id_ == F.id_) { F.data_ = Source.data_; break; } } else { if (Item.legacyMd5_.size() != 16) return Bad(); F.data_ = "MD5" + Item.legacyMd5_; } } if (F.id_ == 0x000a) { F.data_ = F.data_.first(4) + Times + F.data_.mid(4); Ntfs = true; } Out.push_back(std::move(F)); }
    if (!Ntfs && !Times.isEmpty()) Out.push_back({0x000a, QByteArray(4, 0) + Times});
    for (const auto& F : Out) if (F.data_.size() > UINT16_MAX) return Failure("zip-extra-size-overflow", ErrorCategory::Unsupported);
    const auto Bytes = Encode(Out); if (Bytes.size() > UINT16_MAX) return Failure("zip-extra-size-overflow", ErrorCategory::Unsupported); return Bytes;
}
struct Record {
    QByteArray central_, name_, extra_, comment_, local_, localExtra_;
    uint64_t offset_ = 0, data_ = 0, packed_ = 0, end_ = 0, output_ = 0;
    uint16_t alignment_ = 0;
    bool keepMethod_ = false;
};
struct Directory { std::vector<Record> records_; QByteArray comment_; uint64_t central_ = 0; };
Result<Directory> Parse(QFile& File, OperationContext Context) {
    const auto TailAt = std::max<qint64>(0, File.size() - 65557); const auto Tail = Read(File, uint64_t(TailAt), File.size() - TailAt);
    qsizetype End = -1;
    for (auto At = Tail.size() - 22; At >= 0; --At) if (U32(Tail, At) == 0x06054b50 && At + 22 + U16(Tail, At + 20) == Tail.size()) { End = At; break; }
    if (End < 0 || U16(Tail, End + 4) || U16(Tail, End + 6) || U16(Tail, End + 8) != U16(Tail, End + 10)) return Bad();
    uint64_t Count = U16(Tail, End + 10), Size = U32(Tail, End + 12), Central = U32(Tail, End + 16), Limit = uint64_t(TailAt + End);
    if (Count == UINT16_MAX || Size == UINT32_MAX || Central == UINT32_MAX) {
        if (Limit < 20) return Bad(); const auto Locator = Read(File, Limit - 20, 20);
        if (Locator.size() != 20 || U32(Locator, 0) != 0x07064b50 || U32(Locator, 4) || U32(Locator, 16) != 1) return Bad();
        const auto At = U64(Locator, 8); const auto Zip64 = Read(File, At, 56);
        if (Zip64.size() != 56 || U32(Zip64, 0) != 0x06064b50 || U64(Zip64, 4) != 44 || At > Limit - 20 || Limit - 20 - At != 56
            || U32(Zip64, 16) || U32(Zip64, 20) || U64(Zip64, 24) != U64(Zip64, 32)) return Bad();
        Count = U64(Zip64, 32); Size = U64(Zip64, 40); Central = U64(Zip64, 48); Limit = At;
    }
    if (Central > Limit || Size != Limit - Central || Count > Size / 46) return Bad();
    if (Central >= 24 && Read(File, Central - 16, 16) == "APK Sig Block 42") return Failure("zip-apk-signature-update", ErrorCategory::Unsupported);
    Directory Out; Out.central_ = Central; Out.comment_ = Tail.mid(End + 22); auto At = Central;
    for (uint64_t I = 0; I < Count; ++I) {
        if (const auto Stop = Context.Check({"7zip"})) return *Stop;
        Record R; R.central_ = Read(File, At, 46);
        if (R.central_.size() != 46 || U32(R.central_, 0) != 0x02014b50 || U16(R.central_, 34)) return Bad();
        const auto N = U16(R.central_, 28), E = U16(R.central_, 30), C = U16(R.central_, 32);
        const auto Bytes = Read(File, At + 46, qsizetype(N) + E + C); if (Bytes.size() != qsizetype(N) + E + C || At + 46 + Bytes.size() > Limit) return Bad();
        R.name_ = Bytes.first(N); R.extra_ = Bytes.mid(N, E); R.comment_ = Bytes.last(C); R.offset_ = U32(R.central_, 42); R.packed_ = U32(R.central_, 20);
        auto Extras = Fields(R.extra_); if (!Extras.HasValue()) return Bad();
        bool OffsetOk = R.offset_ != UINT32_MAX, PackedOk = R.packed_ != UINT32_MAX;
        for (const auto& F : Extras.Value()) if (F.id_ == 1) {
            qsizetype Pos = U32(R.central_, 24) == UINT32_MAX ? 8 : 0;
            if (!PackedOk) { if (F.data_.size() - Pos < 8) return Bad(); R.packed_ = U64(F.data_, Pos); Pos += 8; PackedOk = true; }
            if (!OffsetOk) { if (F.data_.size() - Pos < 8) return Bad(); R.offset_ = U64(F.data_, Pos); OffsetOk = true; }
        }
        if (!OffsetOk || !PackedOk || R.offset_ >= Central) return Bad();
        R.local_ = Read(File, R.offset_, 30);
        if (R.local_.size() != 30 || U32(R.local_, 0) != 0x04034b50 || U16(R.local_, 6) != U16(R.central_, 8) || U16(R.local_, 8) != U16(R.central_, 10)) return Bad();
        const auto LN = U16(R.local_, 26), LE = U16(R.local_, 28); const auto Local = Read(File, R.offset_ + 30, qsizetype(LN) + LE);
        if (Local.size() != qsizetype(LN) + LE || Local.first(LN) != R.name_) return Bad();
        R.localExtra_ = Local.mid(LN); auto LocalFields = Fields(R.localExtra_); if (!LocalFields.HasValue()) return Bad();
        R.data_ = R.offset_ + 30 + LN + LE; if (R.data_ > Central || R.packed_ > Central - R.data_) return Bad();
        std::optional<uint16_t> Alignment, Kind;
        for (const auto* List : {&Extras.Value(), &LocalFields.Value()}) {
            bool Seen = false;
            for (const auto& F : *List) if (F.id_ == 0xa11e || F.id_ == 0xd935) {
                if (Seen || F.data_.size() < 2) return Bad(); Seen = true; const auto Word = U16(F.data_, 0);
                const auto Value = uint16_t(Word & (F.id_ == 0xa11e ? 0x7fff : 0xffff));
                if ((F.id_ == 0xd935 && !Value) || (Alignment && (*Alignment != Value || *Kind != F.id_))) return Bad(); Alignment = Value; Kind = F.id_;
                R.keepMethod_ = R.keepMethod_ || F.id_ == 0xd935 || !(Word & 0x8000);
            }
        }
        R.alignment_ = Alignment.value_or(0);
        // SDK output is repaired below; input alignment is validated by the
        // preflight, not here, because the temporary writer may shift offsets.
        Out.records_.push_back(std::move(R)); At += 46 + Bytes.size();
    }
    if (At != Limit) {
        const auto Signature = Read(File, At, 6);
        if (Signature.size() == 6 && U32(Signature, 0) == 0x05054b50) return Failure("zip-central-signature-update", ErrorCategory::Unsupported);
        return Bad();
    }
    std::map<uint64_t, Record*> Ordered; for (auto& R : Out.records_) if (!Ordered.emplace(R.offset_, &R).second) return Bad();
    if (!Ordered.empty() && Ordered.begin()->first != 0) return Bad();
    for (auto It = Ordered.begin(); It != Ordered.end(); ++It) { const auto Next = std::next(It); auto& R = *It->second; R.end_ = Next == Ordered.end() ? Central : Next->first; if (R.data_ > R.end_ || R.packed_ > R.end_ - R.data_) return Bad(); }
    return Out;
}
struct Inspection { bool alignment_ = false; QString method_; };
Result<Inspection> CheckSignatures(Archive& Original, const std::set<QString>& Origins, OperationContext Context) {
    QFile File(QString::fromStdString(Original.state_->archive_)); if (!File.open(QIODevice::ReadOnly)) return Failure("zip-metadata-open");
    auto Directory = Parse(File, Context); if (!Directory.HasValue()) return Directory.Failure();
    Inspection Result; UInt32 Index = 0;
    for (const auto& R : Directory.Value().records_) {
        auto H = Original.Header(Index++); if (!H.HasValue()) return H.Failure();
        if (R.alignment_) {
            if (R.data_ % R.alignment_) return Failure("zip-invalid-source-alignment", ErrorCategory::Unsupported);
            Result.alignment_ = true;
            if (R.keepMethod_ && Origins.contains(EntryKey(QString::fromStdString(H.Value().name_)))) {
                const std::map<uint16_t, QString> Methods{{0, "Copy"}, {8, "Deflate"}, {9, "Deflate64"}, {12, "BZip2"}, {14, "LZMA"}, {98, "PPMd"}};
                const auto Id = U16(R.local_, 8); if (!Methods.contains(Id)) return Failure("zip-aligned-method-unsupported", ErrorCategory::Unsupported);
                if (!Result.method_.isEmpty() && Result.method_ != Methods.at(Id)) return Failure("zip-aligned-mixed-methods", ErrorCategory::Unsupported);
                Result.method_ = Methods.at(Id);
            }
        }
        // JAR signature manifests bind the other members' digests. Retaining
        // their bytes after mutation is not a valid signature-preservation path.
        const auto Name = R.name_.toUpper();
        if (Name.startsWith("META-INF/") && (Name.endsWith(".SF") || Name.endsWith(".RSA") || Name.endsWith(".DSA") || Name.endsWith(".EC")))
            return Failure("zip-jar-signature-update", ErrorCategory::Unsupported);
        for (const auto* Bytes : {&R.extra_, &R.localExtra_}) {
            auto Parsed = Fields(*Bytes); if (!Parsed.HasValue()) return Parsed.Failure();
            for (const auto& F : Parsed.Value()) if (F.id_ >= 0x0014 && F.id_ <= 0x0019)
                return Failure("zip-signature-or-strong-encryption-update", ErrorCategory::Unsupported);
        }
    }
    return Result;
}
Result<Unit> Align(Record& R, const Record& From) {
    if (!From.alignment_) return Unit{};
    if (From.keepMethod_ && U16(From.local_, 8) != U16(R.local_, 8)) return Failure("zip-aligned-method-changed", ErrorCategory::Unsupported);
    auto Local = Fields(R.localExtra_), Central = Fields(R.extra_), OldLocal = Fields(From.localExtra_), OldCentral = Fields(From.extra_);
    if (!Local.HasValue() || !Central.HasValue() || !OldLocal.HasValue() || !OldCentral.HasValue()) return Bad();
    const auto IsAlignment = [](const Field& F) { return F.id_ == 0xa11e || F.id_ == 0xd935; };
    std::erase_if(Local.Value(), IsAlignment); std::erase_if(Central.Value(), IsAlignment);
    std::optional<Field> Padding;
    for (const auto& F : OldCentral.Value()) if (IsAlignment(F)) { Padding = Field{F.id_, F.data_.first(2)}; Central.Value().push_back(*Padding); }
    for (const auto& F : OldLocal.Value()) if (IsAlignment(F)) Padding = Field{F.id_, F.data_.first(2)};
    if (!Padding) return Bad(); Local.Value().push_back(*Padding);
    const auto Base = R.output_ + 30 + R.name_.size() + Encode(Local.Value()).size();
    const auto Count = (From.alignment_ - Base % From.alignment_) % From.alignment_;
    Local.Value().back().data_ += QByteArray(qsizetype(Count), 0);
    R.localExtra_ = Encode(Local.Value()); R.extra_ = Encode(Central.Value());
    if (R.localExtra_.size() > UINT16_MAX || R.extra_.size() > UINT16_MAX) return Bad();
    return Unit{};
}
Result<Unit> Restore(Archive& Original, Archive& Fresh, const QString& Path, const std::map<QString, const UpdateItem*>& Expected, uint64_t Limit, OperationContext Context) {
    QFile Old(QString::fromStdString(Original.state_->archive_)), Input(Path);
    if (!Old.open(QIODevice::ReadOnly) || !Input.open(QIODevice::ReadOnly)) return Failure("zip-metadata-open");
    auto A = Parse(Old, Context), B = Parse(Input, Context); if (!A.HasValue()) return A.Failure(); if (!B.HasValue()) return B.Failure();
    if (A.Value().records_.size() != Original.count_ || B.Value().records_.size() != Fresh.count_) return Bad();
    std::map<uint64_t, const Record*> Originals; for (const auto& R : A.Value().records_) Originals.emplace(R.offset_, &R);
    std::set<uint64_t> FreshOffsets; for (const auto& R : B.Value().records_) FreshOffsets.insert(R.offset_);
    std::map<uint64_t, const Record*> Transfers;
    std::map<uint64_t, const Record*> Aligned;
    std::map<uint64_t, const UpdateItem*> Updates;
    for (UInt32 I = 0; I < Fresh.count_; ++I) {
        auto H = Fresh.Header(I); if (!H.HasValue()) return H.Failure(); const auto Found = Expected.find(EntryKey(QString::fromStdString(H.Value().name_))); if (Found == Expected.end()) return Bad();
        const auto Origin = Found->second->metadata_ != UINT32_MAX ? Found->second->metadata_ : Found->second->old_; if (Origin == UINT32_MAX) continue;
        Property Before, After;
        if (Original.archive_->GetProperty(Origin, kpidOffset, &Before) != S_OK || Fresh.archive_->GetProperty(I, kpidOffset, &After) != S_OK || (Before.vt != VT_UI4 && Before.vt != VT_UI8) || (After.vt != VT_UI4 && After.vt != VT_UI8) || !Originals.contains(Before.Number()) || !FreshOffsets.contains(After.Number())) return Bad();
        const auto* From = Originals.at(Before.Number()); if (From->alignment_) Aligned.emplace(After.Number(), From);
        if (Found->second->metadata_ != UINT32_MAX) {
            if (!Transfers.emplace(After.Number(), From).second) return Bad();
            Updates.emplace(After.Number(), Found->second);
        }
    }
    // Release the SDK reader before replacing this private staging file.
    if (Fresh.archive_->Close() != S_OK) return Bad(); Fresh.input_.value_->Release(); Fresh.input_.value_ = nullptr;
    QFile Output(Path + ".metadata"); if (!Output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return Failure("zip-metadata-stage");
    const auto Cleanup = qScopeGuard([&] { Output.close(); QFile::remove(Output.fileName()); });
    auto Shared = Original.state_; UpdateOutput Stream(&Output, Limit, Shared);
    const auto Write = [&](const QByteArray& Bytes) { UInt32 Done = 0; return Stream.Write(Bytes.constData(), UInt32(Bytes.size()), &Done) == S_OK && Done == Bytes.size(); };
    std::map<uint64_t, Record*> Ordered; for (auto& R : B.Value().records_) Ordered.emplace(R.offset_, &R);
    for (const auto& [Offset, Ptr] : Ordered) {
        auto& R = *Ptr; R.output_ = uint64_t(Output.pos());
        if (Transfers.contains(Offset)) {
            const auto& From = *Transfers.at(Offset); auto Local = Merge(R.localExtra_, From.localExtra_, *Updates.at(Offset)), Central = Merge(R.extra_, From.extra_, *Updates.at(Offset));
            if (!Local.HasValue()) return Local.Failure(); if (!Central.HasValue()) return Central.Failure(); R.localExtra_ = std::move(Local.Value()); R.extra_ = std::move(Central.Value());
            Put16(R.central_, 4, U16(From.central_, 4)); Put32(R.central_, 38, U32(From.central_, 38));
        }
        if (Aligned.contains(Offset)) { auto Done = Align(R, *Aligned.at(Offset)); if (!Done.HasValue()) return Done; }
        Put16(R.local_, 28, uint16_t(R.localExtra_.size()));
        if (!Write(R.local_) || !Write(R.name_) || !Write(R.localExtra_)) return Shared->Result(E_FAIL);
        for (auto At = R.data_; At < R.end_;) { const auto Size = qsizetype(std::min<uint64_t>(1024 * 1024, R.end_ - At)); const auto Bytes = Read(Input, At, Size); if (Bytes.size() != Size || !Write(Bytes)) return Shared->Result(E_FAIL); At += Size; }
    }
    const auto Central = uint64_t(Output.pos());
    for (auto& R : B.Value().records_) {
        auto Parsed = Fields(R.extra_); if (!Parsed.HasValue()) return Bad(); bool HadOffset = U32(R.central_, 42) == UINT32_MAX;
        if (HadOffset || R.output_ >= UINT32_MAX) {
            bool Found = false;
            for (auto& F : Parsed.Value()) if (F.id_ == 1) { const qsizetype Pos = (U32(R.central_, 24) == UINT32_MAX ? 8 : 0) + (U32(R.central_, 20) == UINT32_MAX ? 8 : 0); if (F.data_.size() < Pos + (HadOffset ? 8 : 0)) return Bad(); if (HadOffset) Put64(F.data_, Pos, R.output_); else { QByteArray Offset(8, 0); Put64(Offset, 0, R.output_); F.data_.insert(Pos, Offset); } Found = true; break; }
            if (!Found) { if (HadOffset) return Bad(); QByteArray Offset(8, 0); Put64(Offset, 0, R.output_); Parsed.Value().push_back({1, Offset}); }
            Put32(R.central_, 42, UINT32_MAX); Put16(R.central_, 6, std::max<uint16_t>(45, U16(R.central_, 6)));
        } else Put32(R.central_, 42, uint32_t(R.output_));
        R.extra_ = Encode(Parsed.Value()); if (R.extra_.size() > UINT16_MAX) return Bad(); Put16(R.central_, 30, uint16_t(R.extra_.size()));
        if (!Write(R.central_) || !Write(R.name_) || !Write(R.extra_) || !Write(R.comment_)) return Shared->Result(E_FAIL);
    }
    const auto Size = uint64_t(Output.pos()) - Central, Count = uint64_t(B.Value().records_.size());
    const bool Zip64 = Count >= UINT16_MAX || Size >= UINT32_MAX || Central >= UINT32_MAX;
    if (Zip64) {
        const auto At = uint64_t(Output.pos()); QByteArray End(56, 0); Put32(End, 0, 0x06064b50); Put64(End, 4, 44); Put16(End, 12, 45); Put16(End, 14, 45); Put64(End, 24, Count); Put64(End, 32, Count); Put64(End, 40, Size); Put64(End, 48, Central);
        QByteArray Locator(20, 0); Put32(Locator, 0, 0x07064b50); Put64(Locator, 8, At); Put32(Locator, 16, 1); if (!Write(End) || !Write(Locator)) return Shared->Result(E_FAIL);
    }
    QByteArray End(22, 0); Put32(End, 0, 0x06054b50); Put16(End, 8, uint16_t(std::min<uint64_t>(Count, UINT16_MAX))); Put16(End, 10, uint16_t(std::min<uint64_t>(Count, UINT16_MAX))); Put32(End, 12, uint32_t(std::min<uint64_t>(Size, UINT32_MAX))); Put32(End, 16, uint32_t(std::min<uint64_t>(Central, UINT32_MAX))); Put16(End, 20, uint16_t(B.Value().comment_.size()));
    if (!Write(End) || !Write(B.Value().comment_) || !Output.flush()) return Shared->Result(E_FAIL);
    Output.close(); Input.close();
    // Reparse independently before letting the SDK verify content and properties.
    QFile Check(Output.fileName()); if (!Check.open(QIODevice::ReadOnly)) return Bad(); auto Parsed = Parse(Check, Context); if (!Parsed.HasValue()) return Parsed.Failure();
    if (Parsed.Value().records_.size() != B.Value().records_.size() || Parsed.Value().comment_ != B.Value().comment_) return Bad();
    for (size_t I = 0; I < Parsed.Value().records_.size(); ++I) {
        const auto& Want = B.Value().records_[I]; const auto& Got = Parsed.Value().records_[I];
        if (Aligned.contains(Want.offset_) && Got.data_ % Aligned.at(Want.offset_)->alignment_) return Bad();
        if (Got.offset_ != Want.output_ || Got.name_ != Want.name_ || Got.extra_ != Want.extra_ || Got.localExtra_ != Want.localExtra_ || Got.central_ != Want.central_ || Got.local_ != Want.local_ || Got.comment_ != Want.comment_) return Bad();
        if (Transfers.contains(Want.offset_)) {
            const auto* Before = Transfers.at(Want.offset_);
            auto L = Persistent(Before->localExtra_), C = Persistent(Before->extra_), GL = Persistent(Got.localExtra_), GC = Persistent(Got.extra_);
            if (!L.HasValue() || !C.HasValue() || !GL.HasValue() || !GC.HasValue() || L.Value() != GL.Value() || C.Value() != GC.Value()) return Bad();
        }
    }
    Check.close(); if (const auto Stop = Context.Check({"7zip"})) return *Stop;
    if (!MoveFileExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Output.fileName()).utf16()), reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Path).utf16()), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return Failure("zip-metadata-publish");
    return Unit{};
}
} // namespace zipMetadata
