#include "stdafx.h"
#include "largeText.hpp"
#include "directoryPlan.hpp"
#include "requestFile.hpp"
#include "saveRecovery.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QtEndian>
#include <QRegularExpression>
#include <map>
#include <cmath>

namespace hc::compare
{
    namespace
    {
        template<class T> T require(storage::Result<T> Value)
        { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        storage::Locator locator(const QString& Path)
        { return {Path.startsWith("//") || Path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toUtf8().toStdString()}; }
        storage::OperationContext context(const Budget& Limits)
        { storage::OperationContext result; result.workerRead_ = true; result.denyConcurrentMutation_ = true; result.deadline_ = Limits.deadline_; return result; }
        QString revision(const Snapshot& Snapshot)
        { return Snapshot.entry_.id_.revision_ ? QString::fromStdString(Snapshot.entry_.id_.revision_->token_) : QString{}; }
        constexpr qint64 headerLimit = 64 * 1024 * 1024;
        constexpr std::uint64_t workLimit = 8ull * 1024 * 1024 * 1024;
        const QByteArray magic("HCLARGE1\r\n");
        ReportFile writeArtifact(const QString& Path, const Request& Input, const LargeTextComparison& Value, bool ReplaceExisting,
            const Budget& Limits, const std::function<void(const std::function<void(QByteArrayView)>&)>& Produce)
        {
            const auto path = CheckedDirectoryPath(Path); auto protectedInput = Input;
            if (Value.session_) { for (const auto& document : Value.session_->documents_) if (!document.original_.path_.isEmpty()) protectedInput.protectedInputs_.append(document.original_.path_); }
            else { for (const auto& document : Value.documents_) protectedInput.protectedInputs_.append(document.original_.path_); }
            CheckProtectedInputs(protectedInput, {path}, Limits);
            const auto at = locator(path); const auto writer = storage::MakeNativeWritableStorage(at.kind_, storage::MakeNativeReadProvider(at.kind_)); auto ctx = context(Limits);
            const auto entry = require(writer->Inspect(at, ctx)); if (entry && (!ReplaceExisting || entry->directory_)) throw Failure("LargeTextArtifactOverwriteRequired");
            SaveRecoveryJournal journal; ctx.writeCheckpoint_ = journal.Checkpoint(); std::unique_ptr<storage::IWriteSession> output; bool committed = false;
            try {
                output = require(writer->BeginWrite(at, {bool(entry), entry ? entry->id_.revision_ : std::nullopt}, ctx)); std::uint64_t total = 0;
                Produce([&](QByteArrayView Bytes) {
                    if (std::uint64_t(Bytes.size()) > workLimit - total) throw Failure("LargeTextWorkSizeLimit"); total += Bytes.size();
                    for (qsizetype offset = 0; offset < Bytes.size(); offset += 64 * 1024) { Limits.Check(); require(output->Append({reinterpret_cast<const std::byte*>(Bytes.data() + offset), std::size_t(std::min<qsizetype>(64 * 1024, Bytes.size() - offset))}, ctx)); }
                });
                Limits.Check(); const auto receipt = require(output->Commit(ctx)); committed = receipt.destinationCommitted_; if (!committed) throw Failure("NotCommitted");
                ReportFile result; journal.Complete(receipt, result.warnings_, result.recovery_); return result;
            } catch (const std::exception& error) { if (output) output->Abort(); QJsonArray recovery; journal.Track(recovery); throw SaveRecoveryFailure(committed ? std::string("LargeTextArtifactCommitted:") + error.what() : error.what(), recovery); }
        }
        struct Utf8Validator
        {
            int remaining_ = 0; uchar low_ = 0x80, high_ = 0xbf;
            void Add(QByteArrayView Bytes) {
                for (const auto raw : Bytes) { const auto byte = uchar(raw);
                    if (remaining_) { if (byte < low_ || byte > high_) throw Failure("LargeTextWorkInvalidUtf8"); --remaining_; low_ = 0x80; high_ = 0xbf; }
                    else if (!byte) throw Failure("LargeTextWorkInvalidUtf8");
                    else if (byte < 0x80) continue;
                    else if (byte >= 0xc2 && byte <= 0xdf) remaining_ = 1;
                    else if (byte >= 0xe0 && byte <= 0xef) { remaining_ = 2; low_ = byte == 0xe0 ? 0xa0 : 0x80; high_ = byte == 0xed ? 0x9f : 0xbf; }
                    else if (byte >= 0xf0 && byte <= 0xf4) { remaining_ = 3; low_ = byte == 0xf0 ? 0x90 : 0x80; high_ = byte == 0xf4 ? 0x8f : 0xbf; }
                    else throw Failure("LargeTextWorkInvalidUtf8");
                }
            }
        };
        std::uint64_t number(const QJsonValue& Value, std::uint64_t Maximum)
        {
            const auto number = Value.toDouble(-1); if (number < 0 || number > double(Maximum) || number != std::floor(number)) throw Failure("LargeTextWorkSchema"); return std::uint64_t(number);
        }
    }
    LargeTextMatch FindLargeText(const LargeTextDocument& Document, const QString& Query, std::uint64_t Start, bool Backwards, Qt::CaseSensitivity Case, const Budget& Limits)
    {
        if (Query.isEmpty() || Query.toUtf8().size() > 4096 || !Query.isValidUtf16()) throw Failure("LargeTextSearchQueryLimit"); Start = std::min(Start, Document.bytes_);
        const auto admission = Limits.memoryReserve_ ? Limits.memoryReserve_(4 * 1024 * 1024) : std::shared_ptr<void>{};
        const auto overlap = std::uint64_t(Query.toUtf8().size() * 4 + 8);
        // Fold code points before the exact UTF-16 search. Keep original
        // positions so supplementary characters never shift UTF-8 locations.
        const auto fold = [&](const QString& Text) {
            std::pair<QString, std::vector<qsizetype>> result; result.first.reserve(Text.size()); result.second.reserve(Text.size() + 1);
            for (qsizetype i = 0; i < Text.size();) {
                if (!(i & 1023)) Limits.Check(); const auto width = Text[i].isHighSurrogate() && i + 1 < Text.size() ? 2 : 1;
                const auto code = width == 2 ? QChar::surrogateToUcs4(Text[i], Text[i + 1]) : char32_t(Text[i].unicode());
                const auto value = Case == Qt::CaseInsensitive ? QChar::toCaseFolded(code) : code;
                if (value <= 0xffff) { result.first.append(QChar(char16_t(value))); result.second.push_back(i); }
                else { result.first.append(QChar(QChar::highSurrogate(value))); result.first.append(QChar(QChar::lowSurrogate(value))); result.second.push_back(i); result.second.push_back(i); }
                i += width;
            }
            result.second.push_back(Text.size()); return result;
        };
        const auto needle = fold(Query).first; std::uint64_t work = 0;
        const auto scan = [&](std::uint64_t Begin, std::uint64_t End) {
            LargeTextMatch result;
            if (Begin == End) return result;
            auto block = Backwards ? End - std::min<std::uint64_t>(64 * 1024, End - Begin) : Begin;
            for (;;) {
                Limits.Check(); auto begin = block, end = std::min(Document.bytes_, block + 64 * 1024 + overlap);
                const auto continuation = [&](std::uint64_t At) { return At < Document.bytes_ && (uchar(ReadLargeTextRange(Document, At, At + 1, Limits)[0]) & 0xc0) == 0x80; };
                while (begin && continuation(begin)) --begin; while (end > begin && continuation(end)) --end;
                const auto bytes = ReadLargeTextRange(Document, begin, end, Limits); const auto text = QString::fromUtf8(bytes); const auto folded = fold(text);
                std::vector<std::uint64_t> positions(text.size() + 1); std::uint64_t byteOffset = begin;
                for (qsizetype i = 0; i < text.size();) { const auto width = text[i].isHighSurrogate() && i + 1 < text.size() ? 2 : 1; const auto code = width == 2 ? QChar::surrogateToUcs4(text[i], text[i + 1]) : char32_t(text[i].unicode()); positions[i] = byteOffset; if (width == 2) positions[i + 1] = byteOffset; byteOffset += code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4; i += width; } positions[text.size()] = byteOffset;
                qsizetype cursor = Backwards ? folded.first.size() : 0;
                for (;;) {
                    Limits.Check(); if (++work > Limits.steps_) throw Failure("LargeTextSearchWorkLimit");
                    const auto at = Backwards ? folded.first.lastIndexOf(needle, cursor, Qt::CaseSensitive) : folded.first.indexOf(needle, cursor, Qt::CaseSensitive); if (at < 0) break;
                    const auto first = positions[folded.second[at]], last = positions[folded.second[at + needle.size()]];
                    if (first >= block && first < std::min(End, block + 64 * 1024) && last <= Document.bytes_ && (!Backwards || last <= End)) {
                        return LargeTextMatch{true, false, first, last};
                    }
                    if (Backwards && !at) break; cursor = Backwards ? at - 1 : at + 1;
                }
                if (Backwards) { if (block == Begin) break; block -= std::min<std::uint64_t>(64 * 1024, block - Begin); }
                else { if (End - block <= 64 * 1024) break; block += 64 * 1024; }
            }
            return result;
        };
        auto result = Backwards ? scan(0, Start) : scan(Start, Document.bytes_);
        if (!result.found_ && (Backwards ? Start != Document.bytes_ : Start != 0)) { result = scan(0, Document.bytes_); result.wrapped_ = result.found_; }
        return result;
    }
    ReportFile SaveLargeTextWork(const QString& Path, const Request& Input, const LargeTextComparison& Value, bool ReplaceExisting, const Budget& Limits)
    {
        if (!Value.session_) throw Failure("LargeTextWorkRequiresSession");
        const auto& session = *Value.session_; std::vector<std::shared_ptr<LargeTextBacking>> backings; std::map<const LargeTextBacking*, int> indexes;
        const auto encode = [&](const LargeTextDocument& Document) {
            QJsonArray pieces;
            for (const auto& piece : Document.pieces_) {
                Limits.Check(); const auto [at, added] = indexes.emplace(piece.backing_.get(), int(backings.size())); if (added) { backings.push_back(piece.backing_); if (backings.size() > 10000) throw Failure("LargeTextWorkBackingLimit"); }
                pieces.append(QJsonObject{{"backing", at->second}, {"begin", double(piece.begin_)}, {"bytes", double(piece.bytes_)}});
            }
            return QJsonObject{{"path", Document.original_.path_}, {"revision", revision(Document.original_)}, {"exists", Document.original_.exists_}, {"encoding", Document.original_.encoding_}, {"bom", Document.original_.bom_}, {"bytes", double(Document.bytes_)}, {"pieces", pieces}};
        };
        QJsonArray documents, conflicts, files; for (const auto& document : session.documents_) documents.append(encode(document));
        for (const auto& conflict : session.conflicts_) conflicts.append(QJsonObject{{"begin", double(conflict.begin_)}, {"end", double(conflict.end_)}, {"choice", int(conflict.choice_)}, {"base", encode(conflict.base_)}, {"left", encode(conflict.left_)}, {"right", encode(conflict.right_)}});
        std::uint64_t total = 0;
        for (const auto& backing : backings) {
            Limits.Check(); QFile file(backing->file_.fileName()); if (!file.open(QIODevice::ReadOnly)) throw Failure("LargeTextWorkReadFailed"); QCryptographicHash hash(QCryptographicHash::Sha256);
            total += file.size(); if (total > workLimit - headerLimit) throw Failure("LargeTextWorkSizeLimit");
            while (!file.atEnd()) { Limits.Check(); const auto bytes = file.read(64 * 1024); if (bytes.isEmpty()) throw Failure("LargeTextWorkReadFailed"); hash.addData(bytes); }
            files.append(QJsonObject{{"bytes", double(file.size())}, {"sha256", QString::fromLatin1(hash.result().toHex())}});
        }
        const QJsonObject header{{"kind", "HyperCompareLargeWork"}, {"schemaVersion", 1}, {"documents", documents}, {"titles", QJsonArray::fromStringList(session.titles_)}, {"result", session.result_}, {"pair", QJsonArray{session.pair_[0], session.pair_[1]}}, {"reviewRequired", session.reviewRequired_}, {"conflicts", conflicts}, {"backings", files}};
        const auto json = QJsonDocument(header).toJson(QJsonDocument::Compact); if (json.size() > headerLimit) throw Failure("LargeTextWorkHeaderLimit");
        return writeArtifact(Path, Input, Value, ReplaceExisting, Limits, [&](const auto& Output) {
            Output(magic); const quint64 size = qToLittleEndian(quint64(json.size())); Output(QByteArrayView(reinterpret_cast<const char*>(&size), sizeof(size))); Output(json);
            for (const auto& backing : backings) { QFile file(backing->file_.fileName()); if (!file.open(QIODevice::ReadOnly)) throw Failure("LargeTextWorkReadFailed"); while (!file.atEnd()) { Limits.Check(); const auto bytes = file.read(64 * 1024); if (bytes.isEmpty()) throw Failure("LargeTextWorkReadFailed"); Output(bytes); } }
        });
    }
    std::shared_ptr<LargeTextComparison> LoadLargeTextWork(const QString& Path, const Request& Input, const LargeTextComparison& Current, const Budget& Limits, bool RecoverResult)
    {
        if (!Current.session_) throw Failure("LargeTextWorkRequiresSession"); ValidateLargeTextInputs(*Current.session_, Limits);
        if (RecoverResult && Current.session_->result_ < 0) throw Failure("LargeTextRecoveryRequiresResult");
        const auto path = CheckedDirectoryPath(Path); QFile file(path); if (!file.open(QIODevice::ReadOnly) || std::uint64_t(file.size()) > workLimit) throw Failure("LargeTextWorkReadFailed");
        if (file.read(magic.size()) != magic) throw Failure("LargeTextWorkMagic"); const auto sizeBytes = file.read(sizeof(quint64)); if (sizeBytes.size() != sizeof(quint64)) throw Failure("LargeTextWorkTruncated");
        const auto size = qFromLittleEndian<quint64>(sizeBytes.constData()); if (size > headerLimit) throw Failure("LargeTextWorkHeaderLimit");
        const auto admission = Limits.memoryReserve_ ? Limits.memoryReserve_(size * 8 + 4 * 1024 * 1024) : std::shared_ptr<void>{};
        const auto json = file.read(qint64(size)); if (json.size() != qint64(size)) throw Failure("LargeTextWorkTruncated"); QJsonParseError error; const auto parsed = QJsonDocument::fromJson(json, &error);
        if (error.error != QJsonParseError::NoError || !parsed.isObject()) throw Failure("LargeTextWorkJson"); const auto header = parsed.object();
        if (header["kind"] != "HyperCompareLargeWork" || header["schemaVersion"] != 1 || header["result"].toInt(-2) != Current.session_->result_ || !header["reviewRequired"].isBool()) throw Failure("LargeTextWorkSchema");
        const auto files = header["backings"].toArray(); if (files.size() > 10000) throw Failure("LargeTextWorkBackingLimit"); std::vector<std::shared_ptr<LargeTextBacking>> backings; std::uint64_t total = 0;
        for (const auto& value : files) {
            Limits.Check(); const auto entry = value.toObject(); const auto bytes = number(entry["bytes"], largeTextMaximum); total += bytes;
            if (total > workLimit || bytes > std::uint64_t(file.size() - file.pos()) || !QRegularExpression("^[a-fA-F0-9]{64}$").match(entry["sha256"].toString()).hasMatch()) throw Failure("LargeTextWorkSchema");
            auto backing = std::make_shared<LargeTextBacking>(); if (!backing->file_.open()) throw Failure("LargeTextTemporaryFileFailed"); QCryptographicHash hash(QCryptographicHash::Sha256); Utf8Validator validator;
            for (auto remaining = bytes; remaining;) { Limits.Check(); const auto chunk = file.read(qint64(std::min<std::uint64_t>(remaining, 64 * 1024))); if (chunk.isEmpty() || backing->file_.write(chunk) != chunk.size()) throw Failure("LargeTextWorkTruncated"); validator.Add(chunk); hash.addData(chunk); remaining -= chunk.size(); }
            if (validator.remaining_ || hash.result().toHex() != entry["sha256"].toString().toLatin1().toLower() || !backing->file_.flush()) throw Failure("LargeTextWorkHashMismatch"); backings.push_back(std::move(backing));
        }
        if (!file.atEnd()) throw Failure("LargeTextWorkTrailingData");
        const auto decode = [&](const QJsonObject& Object, const Snapshot& Original) {
            LargeTextDocument document; document.original_ = Original; document.bytes_ = number(Object["bytes"], largeTextMaximum); const auto pieces = Object["pieces"].toArray(); if (pieces.size() > 100000) throw Failure("LargeTextPieceLimit"); std::uint64_t bytes = 0;
            for (const auto& value : pieces) { Limits.Check(); const auto piece = value.toObject(); const auto index = number(piece["backing"], backings.empty() ? 0 : backings.size() - 1); if (backings.empty()) throw Failure("LargeTextWorkSchema");
                const auto begin = number(piece["begin"], largeTextMaximum), count = number(piece["bytes"], largeTextMaximum); if (begin + count > std::uint64_t(backings[index]->file_.size()) || count > largeTextMaximum - bytes) throw Failure("LargeTextWorkSchema"); bytes += count; document.pieces_.push_back({backings[index], begin, count});
            }
            if (bytes != document.bytes_) throw Failure("LargeTextWorkSchema");
            // Reject ranges that split a Unicode character, including inside a
            // backing shared by several independently valid pieces.
            for (const auto& piece : document.pieces_) { QFile source(piece.backing_->file_.fileName()); if (!source.open(QIODevice::ReadOnly)) throw Failure("LargeTextWorkReadFailed"); for (const auto at : {piece.begin_, piece.begin_ + piece.bytes_}) if (at < std::uint64_t(source.size())) { if (!source.seek(qint64(at))) throw Failure("LargeTextWorkReadFailed"); const auto byte = source.read(1); if (byte.size() != 1 || (uchar(byte[0]) & 0xc0) == 0x80) throw Failure("LargeTextCharacterBoundary"); } }
            return document;
        };
        auto session = *Current.session_; const auto documents = header["documents"].toArray(); if (documents.size() != int(session.documents_.size()) || header["titles"].toArray() != QJsonArray::fromStringList(session.titles_)) throw Failure("LargeTextWorkInputMismatch");
        for (int i = 0; i < documents.size(); ++i) { const auto item = documents[i].toObject(); const auto original = session.documents_[i].original_;
            if (i != session.result_ && (item["path"] != original.path_ || (!RecoverResult && (item["revision"] != revision(original) || item["exists"] != original.exists_ || item["encoding"] != original.encoding_ || item["bom"] != original.bom_)))) throw Failure("LargeTextWorkInputMismatch");
            const auto restored = decode(item, original); if (!RecoverResult || i == session.result_) session.documents_[i] = restored;
        }
        session.reviewRequired_ = header["reviewRequired"].toBool(); session.conflicts_.clear(); const auto conflicts = header["conflicts"].toArray(); if (conflicts.size() > 200000 || (session.result_ < 0 && !conflicts.isEmpty())) throw Failure("LargeTextWorkSchema");
        std::uint64_t previous = 0; for (const auto& value : conflicts) { const auto item = value.toObject(); LargeTextConflict conflict; conflict.begin_ = number(item["begin"], largeTextMaximum); conflict.end_ = number(item["end"], largeTextMaximum); const auto choice = number(item["choice"], int(Resolution::Omit));
            if (conflict.begin_ > conflict.end_ || (!session.reviewRequired_ && (conflict.begin_ < previous || conflict.end_ > session.documents_[session.result_].bytes_))) throw Failure("LargeTextWorkSchema"); previous = conflict.end_; conflict.choice_ = Resolution(choice);
            conflict.base_ = decode(item["base"].toObject(), {}); conflict.left_ = decode(item["left"].toObject(), {}); conflict.right_ = decode(item["right"].toObject(), {}); session.conflicts_.push_back(std::move(conflict));
        }
        const auto pair = header["pair"].toArray(); if (pair.size() != 2) throw Failure("LargeTextWorkSchema"); const std::array selected{int(number(pair[0], session.documents_.size() - 1)), int(number(pair[1], session.documents_.size() - 1))};
        if (session.result_ >= 0 && (selected[1] != session.result_ || selected[0] == session.result_)) throw Failure("LargeTextWorkSchema");
        if (RecoverResult) { session.reviewRequired_ = true; session.conflicts_.clear(); }
        Q_UNUSED(Input); return PairLargeTextSession(std::move(session), selected, Limits);
    }
    ReportFile SaveLargeTextReport(const QString& Path, const Request& Input, const LargeTextComparison& Value, bool ReplaceExisting, const Budget& Limits)
    {
        auto report = LargeTextSessionJson(Value); report["kind"] = "HyperCompareLargeReport"; report["schemaVersion"] = 1; report["engine"] = textEngineVersion; report["workingCopy"] = true; report["contentIncluded"] = false;
        if (Value.session_) report["comparisons"] = Value.session_->comparisons_;
        QJsonArray changes; for (const auto& edit : Value.changes_) { Limits.Check(); changes.append(QJsonObject{{"begin", double(edit.begin_)}, {"end", double(edit.end_)}, {"otherBegin", double(edit.otherBegin_)}, {"otherEnd", double(edit.otherEnd_)}}); }
        report["activePairChanges"] = changes; const auto json = QJsonDocument(report).toJson(QJsonDocument::Indented); if (json.size() > headerLimit) throw Failure("ReportSizeLimit");
        return writeArtifact(Path, Input, Value, ReplaceExisting, Limits, [&](const auto& Output) { Output(json); });
    }
}
