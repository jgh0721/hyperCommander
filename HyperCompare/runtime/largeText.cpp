#include "stdafx.h"
#include "largeText.hpp"
#include "controllerMemory.hpp"
#include "saveRecovery.hpp"
#include "requestFile.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/document/documentTextCodec.hpp"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <algorithm>

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
        std::shared_ptr<LargeTextBacking> backing()
        {
            auto result = std::make_shared<LargeTextBacking>();
            if (!result->file_.open()) throw Failure("LargeTextTemporaryFileFailed");
            return result;
        }
        void append(QFile& File, const QByteArray& Bytes)
        {
            if (std::uint64_t(File.size()) + std::uint64_t(Bytes.size()) > largeTextMaximum) throw Failure("LargeTextSizeLimit");
            if (File.write(Bytes) != Bytes.size()) throw Failure("LargeTextTemporaryWriteFailed");
        }
        // Cut only at complete encoding units; the codec then validates the chunk
        // strictly. Prefixing one character prevents BOM stripping inside a file.
        qsizetype completeBytes(const QByteArray& Bytes, const QString& Encoding, bool Final)
        {
            auto count = Bytes.size(); if (Final || !count) return count;
            if (Encoding == "UTF-16LE" || Encoding == "UTF-16BE") {
                count -= count % 2;
                if (count >= 2) {
                    const auto a = uchar(Bytes[count - 2]), b = uchar(Bytes[count - 1]);
                    const auto unit = Encoding == "UTF-16LE" ? a | (b << 8) : (a << 8) | b;
                    if (unit >= 0xd800 && unit <= 0xdbff) count -= 2;
                }
            } else if (Encoding == "CP949") {
                qsizetype i = 0; while (i < count) i += IsDBCSLeadByteEx(949, uchar(Bytes[i])) ? 2 : 1;
                if (i > count) --count;
            } else {
                auto start = count - 1; while (start > 0 && (uchar(Bytes[start]) & 0xc0) == 0x80) --start;
                const auto lead = uchar(Bytes[start]);
                const int width = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
                if (count - start < width) count = start;
            }
            return count;
        }
        QString decode(const QByteArray& Bytes, const QString& Encoding)
        {
            if (Encoding == "UTF-8" && !Bytes.isValidUtf8()) throw Failure("LargeTextInvalidUtf8");
            const QByteArray prefix = Encoding == "UTF-16LE" ? QByteArray("X\0", 2) : Encoding == "UTF-16BE" ? QByteArray("\0X", 2) : QByteArray("X");
            auto decoded = require(document::DecodeDocumentText(prefix + Bytes, Encoding));
            // Detect incomplete UTF-16 surrogate tails even if a stateful decoder
            // buffers them rather than reporting an immediate conversion error.
            const auto roundTrip = require(document::EncodeDocumentText(decoded.text_, Encoding, false));
            if (roundTrip != prefix + Bytes) throw Failure("LargeTextIncompleteEncoding");
            return decoded.text_.mid(1);
        }
        template<class Emit> void visit(const LargeTextDocument& Document, std::uint64_t Begin, std::uint64_t End, const Budget& Limits, Emit Output)
        {
            if (Begin > End || End > Document.bytes_) throw Failure("LargeTextRangeInvalid");
            std::uint64_t offset = 0;
            for (const auto& piece : Document.pieces_) {
                Limits.Check(); const auto first = std::max(Begin, offset), last = std::min(End, offset + piece.bytes_);
                if (first < last) {
                    QFile file(piece.backing_->file_.fileName());
                    if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(piece.begin_ + first - offset))) throw Failure("LargeTextTemporaryReadFailed");
                    for (auto remaining = last - first; remaining;) {
                        Limits.Check(); const auto bytes = file.read(qint64(std::min<std::uint64_t>(remaining, 64 * 1024)));
                        if (bytes.isEmpty()) throw Failure("LargeTextTemporaryReadFailed");
                        Output(bytes); remaining -= bytes.size();
                    }
                }
                offset += piece.bytes_;
            }
        }
        void slice(std::vector<LargeTextPiece>& Output, const LargeTextDocument& Document, std::uint64_t Begin, std::uint64_t End)
        {
            std::uint64_t offset = 0;
            for (const auto& piece : Document.pieces_) {
                const auto first = std::max(Begin, offset), last = std::min(End, offset + piece.bytes_);
                if (first < last) Output.push_back({piece.backing_, piece.begin_ + first - offset, last - first});
                offset += piece.bytes_;
            }
        }
        struct Mapping
        {
            std::shared_ptr<LargeTextBacking> backing_;
            uchar* data_ = nullptr;
            explicit Mapping(const LargeTextDocument& Document, const Budget& Limits) : backing_(backing())
            {
                visit(Document, 0, Document.bytes_, Limits, [&](const QByteArray& Bytes) { append(backing_->file_, Bytes); });
                if (!backing_->file_.flush()) throw Failure("LargeTextTemporaryWriteFailed");
                if (Document.bytes_) { data_ = backing_->file_.map(0, qint64(Document.bytes_)); if (!data_) throw Failure("LargeTextMapFailed"); }
            }
            ~Mapping() { if (data_) backing_->file_.unmap(data_); }
            std::string_view View() const { return {data_ ? reinterpret_cast<const char*>(data_) : "", std::size_t(backing_->file_.size())}; }
        };
    }
    LargeTextDocument ReadLargeText(const QString& Path, const QString& Encoding, const Budget& Limits, bool AllowMissing)
    {
        Limits.Check(); const auto at = locator(Path); const auto provider = storage::MakeNativeReadProvider(at.kind_); const auto ctx = context(Limits);
        LargeTextDocument result; result.original_.path_ = Path;
        if (AllowMissing) {
            const auto entry = require(storage::MakeNativeWritableStorage(at.kind_, provider)->Inspect(at, ctx));
            if (!entry) { result.original_.exists_ = false; result.original_.encoding_ = Encoding.isEmpty() ? QString("UTF-8") : Encoding; result.rawHash_ = QCryptographicHash::hash(QByteArray{}, QCryptographicHash::Sha256); return result; }
            result.original_.entry_ = *entry;
        } else result.original_.entry_ = require(provider->Resolve(at, ctx).get());
        const auto& entry = result.original_.entry_;
        if (entry.directory_ || entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) throw Failure("LargeTextExpectedRegularFile");
        if (entry.size_ > largeTextMaximum) throw Failure("LargeTextSizeLimit");
        const auto reservation = Limits.memoryReserve_ ? Limits.memoryReserve_(2 * 1024 * 1024) : std::shared_ptr<void>{};
        const auto stream = require(provider->OpenRead(entry.id_, ctx).get()); auto store = backing(); QByteArray pending; bool first = true;
        std::uint64_t readBytes = 0; QCryptographicHash rawHash(QCryptographicHash::Sha256);
        for (;;) {
            Limits.Check(); const auto chunk = require(stream->Read(64 * 1024, ctx).get());
            readBytes += chunk.bytes_.size(); if (readBytes > largeTextMaximum) throw Failure("LargeTextSizeLimit");
            if (chunk.bytes_.empty() && !chunk.eof_) throw Failure("ReadMadeNoProgress");
            pending.append(reinterpret_cast<const char*>(chunk.bytes_.data()), qsizetype(chunk.bytes_.size()));
            rawHash.addData(QByteArrayView(reinterpret_cast<const char*>(chunk.bytes_.data()), qsizetype(chunk.bytes_.size())));
            if (first && (pending.size() >= 4 || chunk.eof_)) {
                first = false; const auto detected = pending.startsWith("\xff\xfe") ? QString("UTF-16LE") : pending.startsWith("\xfe\xff") ? QString("UTF-16BE") : QString("UTF-8");
                result.original_.encoding_ = Encoding.isEmpty() ? detected : Encoding;
                const auto bom = detected.startsWith("UTF-16") ? 2 : pending.startsWith("\xef\xbb\xbf") ? 3 : 0;
                if (bom && detected != result.original_.encoding_) throw Failure("LargeTextBomEncodingMismatch");
                result.original_.bom_ = bom != 0; pending.remove(0, bom);
            }
            if (!first) {
                const auto count = completeBytes(pending, result.original_.encoding_, chunk.eof_);
                append(store->file_, decode(pending.left(count), result.original_.encoding_).toUtf8()); pending.remove(0, count);
            }
            if (chunk.eof_) break;
        }
        const auto current = require(provider->Stat(entry.id_, ctx).get());
        if (readBytes != entry.size_ || current.id_.revision_ != entry.id_.revision_) throw Failure("ChangedDuringRead");
        if (!store->file_.flush()) throw Failure("LargeTextTemporaryWriteFailed");
        result.bytes_ = std::uint64_t(store->file_.size()); if (result.bytes_) result.pieces_.push_back({store, 0, result.bytes_});
        result.rawHash_ = rawHash.result();
        return result;
    }
    LargeTextComparison CompareLargeText(std::array<LargeTextDocument, 2> Documents, const Budget& Limits)
    {
        Limits.Check(); Mapping left(Documents[0], Limits), right(Documents[1], Limits); auto ranges = Limits; ranges.rangesOnly_ = true;
        const auto leftLines = CountLines(left.View(), Limits); CountLines(right.View(), Limits, 2'000'000 - leftLines);
        LargeTextComparison result; result.documents_ = std::move(Documents); result.changes_ = Diff(left.View(), right.View(), ranges);
        for (int side = 0; side < 2; ++side) {
            const auto view = side ? right.View() : left.View(); const auto count = CountLines(view, Limits);
            const auto reservation = Limits.memoryReserve_ ? Limits.memoryReserve_((count + 1) * sizeof(std::uint64_t)) : std::shared_ptr<void>{};
            auto& lines = result.lines_[side]; lines.reserve(count + 1); lines.push_back(0);
            for (std::size_t i = 0; i < view.size(); ++i) {
                if (!(i & 4095)) Limits.Check();
                if (view[i] == '\r') { if (i + 1 < view.size() && view[i + 1] == '\n') ++i; lines.push_back(i + 1); }
                else if (view[i] == '\n') lines.push_back(i + 1);
            }
            if (lines.back() != view.size()) lines.push_back(view.size());
        }
        return result;
    }
    QByteArray ReadLargeTextRange(const LargeTextDocument& Document, std::uint64_t Begin, std::uint64_t End, const Budget& Limits)
    {
        if (End < Begin || End - Begin > largeTextPageMaximum) throw Failure("LargeTextPageLimit");
        QByteArray result; result.reserve(qsizetype(End - Begin)); visit(Document, Begin, End, Limits, [&](const QByteArray& Bytes) { result += Bytes; }); return result;
    }
    LargeTextDocument ReplaceLargeText(const LargeTextDocument& Target, std::uint64_t Begin, std::uint64_t End,
        const LargeTextDocument& Source, std::uint64_t SourceBegin, std::uint64_t SourceEnd, const Budget& Limits)
    {
        Limits.Check();
        if (Begin > End || End > Target.bytes_ || SourceBegin > SourceEnd || SourceEnd > Source.bytes_) throw Failure("LargeTextRangeInvalid");
        for (const auto& boundary : {std::pair{&Target, Begin}, std::pair{&Target, End}, std::pair{&Source, SourceBegin}, std::pair{&Source, SourceEnd}})
            if (boundary.second < boundary.first->bytes_) {
                const auto byte = ReadLargeTextRange(*boundary.first, boundary.second, boundary.second + 1, Limits);
                if ((uchar(byte[0]) & 0xc0) == 0x80) throw Failure("LargeTextCharacterBoundary");
            }
        LargeTextDocument result; result.original_ = Target.original_; result.bytes_ = Target.bytes_ - (End - Begin) + SourceEnd - SourceBegin;
        if (result.bytes_ > largeTextMaximum) throw Failure("LargeTextSizeLimit");
        slice(result.pieces_, Target, 0, Begin); slice(result.pieces_, Source, SourceBegin, SourceEnd); slice(result.pieces_, Target, End, Target.bytes_);
        if (result.pieces_.size() > 100000) throw Failure("LargeTextPieceLimit");
        return result;
    }
    LargeTextDocument EditLargeText(const LargeTextDocument& Target, std::uint64_t Begin, std::uint64_t End, const QByteArray& Utf8, const Budget& Limits)
    {
        Limits.Check(); if (Utf8.size() > largeTextPageMaximum) throw Failure("LargeTextPageLimit"); decode(Utf8, "UTF-8");
        auto store = backing(); append(store->file_, Utf8); if (!store->file_.flush()) throw Failure("LargeTextTemporaryWriteFailed");
        LargeTextDocument replacement; replacement.bytes_ = Utf8.size(); replacement.pieces_.push_back({store, 0, replacement.bytes_});
        return ReplaceLargeText(Target, Begin, End, replacement, 0, replacement.bytes_, Limits);
    }
    LargeTextDocument MergeLargeTextChange(const LargeTextComparison& Comparison, int Source, std::size_t Change,
        const std::optional<std::pair<std::uint64_t, std::uint64_t>>& Selection, const Budget& Limits)
    {
        Limits.Check(); if (Source < 0 || Source > 1 || Change >= Comparison.changes_.size()) throw Failure("LargeTextChangeInvalid");
        const auto& edit = Comparison.changes_[Change]; const int target = 1 - Source;
        const auto& source = Comparison.documents_[Source]; const auto& destination = Comparison.documents_[target];
        const auto& from = Comparison.lines_[Source]; const auto& to = Comparison.lines_[target];
        const auto first = Source ? edit.otherBegin_ : edit.begin_, last = Source ? edit.otherEnd_ : edit.end_;
        const auto targetFirst = Source ? edit.begin_ : edit.otherBegin_, targetLast = Source ? edit.end_ : edit.otherEnd_;
        auto begin = from[first], end = from[last], targetBegin = to[targetFirst], targetEnd = to[targetLast];
        if (Selection) {
            begin = std::max(begin, Selection->first); end = std::min(end, Selection->second);
            if (begin >= end) throw Failure("LargeTextSelectionOutsideChange");
            const auto project = [&](std::uint64_t Position) {
                if (Position == from[last]) return to[targetLast];
                const auto row = std::size_t(std::upper_bound(from.begin(), from.end(), Position) - from.begin() - 1);
                const auto targetRow = targetFirst + row - first; if (targetRow >= targetLast) return to[targetLast];
                std::uint64_t characters = 0;
                visit(source, from[row], Position, Limits, [&](const QByteArray& Bytes) { for (const auto byte : Bytes) if ((uchar(byte) & 0xc0) != 0x80) ++characters; });
                auto limit = to[targetRow + 1];
                if (limit > to[targetRow] && ReadLargeTextRange(destination, limit - 1, limit, Limits)[0] == '\n') --limit;
                if (limit > to[targetRow] && ReadLargeTextRange(destination, limit - 1, limit, Limits)[0] == '\r') --limit;
                std::uint64_t offset = to[targetRow], remaining = characters, projected = limit; bool found = false;
                visit(destination, to[targetRow], limit, Limits, [&](const QByteArray& Bytes) {
                    if (!found) for (const auto byte : Bytes) {
                        if ((uchar(byte) & 0xc0) != 0x80) { if (!remaining) { projected = offset; found = true; break; } --remaining; }
                        ++offset;
                    }
                });
                return projected;
            };
            targetBegin = project(begin); targetEnd = project(end);
        }
        return ReplaceLargeText(destination, targetBegin, targetEnd, source, begin, end, Limits);
    }
    LargeTextSave SaveLargeText(const Request& Input, const LargeTextDocument& Document, const Budget& Limits)
    {
        Limits.Check(); CheckProtectedInputs(Input, {Document.original_.path_}, Limits);
        const auto at = locator(Document.original_.path_); const auto provider = storage::MakeNativeReadProvider(at.kind_);
        const auto writer = storage::MakeNativeWritableStorage(at.kind_, provider); auto ctx = context(Limits);
        const auto current = require(writer->Inspect(at, ctx));
        if (bool(current) != Document.original_.exists_ || (current && current->id_.revision_ != Document.original_.entry_.id_.revision_)) throw Failure("InputChangedBeforeSave");
        SaveRecoveryJournal journal; ctx.writeCheckpoint_ = journal.Checkpoint(); std::unique_ptr<storage::IWriteSession> output;
        LargeTextSave result; bool committed = false;
        try {
            if (!current) {
                QStringList parents; auto parent = QFileInfo(Document.original_.path_).absolutePath();
                for (;;) {
                    Limits.Check(); const auto entry = require(writer->Inspect(locator(parent), ctx));
                    if (entry) { if (!entry->directory_) throw Failure("ExpectedDirectory"); break; }
                    parents.prepend(parent); const auto next = QFileInfo(parent).absolutePath(); if (next == parent) throw Failure("MissingDestinationRoot"); parent = next;
                }
                for (const auto& path : parents) { Limits.Check(); require(writer->MakeDirectory(locator(path), ctx)); }
            }
            output = require(writer->BeginWrite(at, {Document.original_.exists_, Document.original_.entry_.id_.revision_}, ctx));
            QByteArray pending; bool first = true; QCryptographicHash expectedHash(QCryptographicHash::Sha256);
            const auto write = [&](bool Final) {
                const auto count = completeBytes(pending, "UTF-8", Final); const auto text = decode(pending.left(count), "UTF-8");
                if (count || Final) {
                    auto encoded = require(document::EncodeDocumentText(text, Document.original_.encoding_, false));
                    if (first && Document.original_.bom_) encoded.prepend(Document.original_.encoding_ == "UTF-8" ? QByteArray::fromHex("efbbbf") : Document.original_.encoding_ == "UTF-16LE" ? QByteArray::fromHex("fffe") : QByteArray::fromHex("feff"));
                    first = false;
                    expectedHash.addData(encoded);
                    for (qsizetype offset = 0; offset < encoded.size(); offset += 64 * 1024) {
                        Limits.Check(); require(output->Append({reinterpret_cast<const std::byte*>(encoded.constData() + offset), std::size_t(std::min<qsizetype>(64 * 1024, encoded.size() - offset))}, ctx));
                    }
                }
                pending.remove(0, count);
            };
            visit(Document, 0, Document.bytes_, Limits, [&](const QByteArray& Bytes) { pending += Bytes; write(false); }); write(true);
            Limits.Check(); const auto receipt = require(output->Commit(ctx));
            if (!receipt.destinationCommitted_) throw Failure("NotCommitted");
            committed = true;
            result.snapshot_ = Document.original_; result.snapshot_.exists_ = true; result.snapshot_.entry_ = require(provider->Resolve(at, ctx).get());
            const auto stream = require(provider->OpenRead(result.snapshot_.entry_.id_, ctx).get()); QCryptographicHash actualHash(QCryptographicHash::Sha256);
            for (;;) {
                Limits.Check(); const auto bytes = require(stream->Read(64 * 1024, ctx).get());
                actualHash.addData(QByteArrayView(reinterpret_cast<const char*>(bytes.bytes_.data()), qsizetype(bytes.bytes_.size())));
                if (bytes.eof_) break; if (bytes.bytes_.empty()) throw Failure("ReadMadeNoProgress");
            }
            if (expectedHash.result() != actualHash.result() || require(provider->Stat(result.snapshot_.entry_.id_, ctx).get()).id_.revision_ != result.snapshot_.entry_.id_.revision_) throw Failure("InputChangedAfterSave");
            result.rawHash_ = actualHash.result();
            journal.Complete(receipt, result.warnings_, result.recovery_);
            return result;
        } catch (...) {
            if (output) output->Abort();
            QJsonArray recovery; journal.Track(recovery);
            try { throw; } catch (const std::exception& error) { throw SaveRecoveryFailure(committed ? std::string("LargeTextCommittedVerificationFailed:") + error.what() : error.what(), recovery); }
        }
    }
}
