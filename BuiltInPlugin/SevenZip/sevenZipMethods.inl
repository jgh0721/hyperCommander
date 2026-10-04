// SDK method strings describe the actual coder chain, unlike the archive-level
// summary, which sorts codec IDs and drops filter/order properties.
struct CodecToken { QString identity_, option_; uint64_t dictionary_ = 0; };
uint64_t DictionaryValue(QString Value) {
    unsigned Shift = 64;
    if (Value.endsWith('k')) { Shift = 10; Value.chop(1); }
    else if (Value.endsWith('m')) { Shift = 20; Value.chop(1); }
    else if (Value.endsWith('b')) { Shift = 0; Value.chop(1); }
    bool Ok = false; const auto Number = Value.toULongLong(&Ok); if (!Ok) return 0;
    if (Shift == 64) return Number < 63 ? uint64_t(1) << Number : 0;
    return Number <= (UINT64_MAX >> Shift) ? Number << Shift : 0;
}
Result<std::vector<CodecToken>> ParseCodecs(QString Description) {
    Description.remove(QRegularExpression("(?:^| )7zAES(?::[0-9]+)?"));
    std::vector<CodecToken> Result;
    for (const auto& Token : Description.split(' ', Qt::SkipEmptyParts)) {
        CodecToken C; C.identity_ = C.option_ = Token;
        const auto Lzma = QRegularExpression("^(LZMA2|LZMA):([0-9]+[kmb]?)((?::(?:lc|lp|pb)[0-9]+)*)$").match(Token);
        const auto Ppmd = QRegularExpression("^PPMD:o([0-9]+):mem([0-9]+[kmb]?)$").match(Token);
        const auto Delta = QRegularExpression("^Delta:([0-9]+)$").match(Token);
        if (Lzma.hasMatch()) {
            C.dictionary_ = DictionaryValue(Lzma.captured(2)); if (!C.dictionary_) return Failure("7zip-dictionary-property", ErrorCategory::Unsupported);
            C.identity_ = Lzma.captured(1) + Lzma.captured(3); C.option_ = Lzma.captured(1) + ":d" + Lzma.captured(2) + Lzma.captured(3);
        } else if (Ppmd.hasMatch()) {
            C.dictionary_ = DictionaryValue(Ppmd.captured(2)); if (!C.dictionary_) return Failure("7zip-dictionary-property", ErrorCategory::Unsupported);
            C.identity_ = "PPMD:o" + Ppmd.captured(1); C.option_ = "PPMd:o" + Ppmd.captured(1) + ":mem" + Ppmd.captured(2);
        } else if (Delta.hasMatch()) {
            const auto Distance = Delta.captured(1).toUInt(); if (!Distance || Distance > 256) return Failure("7zip-delta-distance", ErrorCategory::Unsupported);
        } else if (!QStringList{"Copy", "Deflate", "Deflate64", "BZip2", "BCJ", "PPC", "IA64", "ARM", "ARMT", "SPARC", "ARM64", "RISCV"}.contains(Token))
            return Failure("7zip-coder-chain-unsupported:" + Token.toStdString(), ErrorCategory::Unsupported);
        Result.push_back(std::move(C));
    }
    return Result;
}
bool SameSolidMethod(const Property& Before, const Property& After) {
    auto A = ParseCodecs(Before.Text()), B = ParseCodecs(After.Text());
    if (!A.HasValue() || !B.HasValue() || A.Value().size() != B.Value().size()) return false;
    // Encryption parameters are not compression-dictionary normalization.
    const QRegularExpression Crypto("7zAES(?::[0-9]+)?");
    if (Crypto.match(Before.Text()).captured() != Crypto.match(After.Text()).captured()) return false;
    for (size_t I = 0; I < A.Value().size(); ++I) if (A.Value()[I].identity_ != B.Value()[I].identity_ || B.Value()[I].dictionary_ > A.Value()[I].dictionary_) return false;
    return true;
}
Result<Unit> MergeCodecs(std::vector<CodecToken>& Plan, const QString& Description) {
    auto Parsed = ParseCodecs(Description); if (!Parsed.HasValue()) return Parsed.Failure();
    if (Parsed.Value().empty()) return Unit{};
    if (Plan.empty()) { Plan = std::move(Parsed.Value()); return Unit{}; }
    if (Plan.size() != Parsed.Value().size()) return Failure("7zip-mixed-coder-chains", ErrorCategory::Unsupported);
    for (size_t I = 0; I < Plan.size(); ++I) {
        const auto& New = Parsed.Value()[I]; if (Plan[I].identity_ != New.identity_) return Failure("7zip-mixed-coder-chains", ErrorCategory::Unsupported);
        if (Plan[I].dictionary_ < New.dictionary_) Plan[I] = New;
    }
    return Unit{};
}
