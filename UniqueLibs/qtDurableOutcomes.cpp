#include "qtItemOutcomeStore.hpp"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace hc::operations {
using namespace storage;
namespace {
constexpr uint32_t Magic = 0x48434f31;
constexpr uint64_t HeaderBytes = 52, MaxRecordBytes = 16 * 1024 * 1024;
Error Failure(const char* Code) { return {ErrorCategory::Unavailable, Code, {"operation-journal"}}; }
class DurableOutcomes final : public IItemOutcomeStore {
public:
    explicit DurableOutcomes(QString Path) : path_(std::move(Path)) {}
    ~DurableOutcomes() override { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    Result<Unit> Open()
    {
        file_ = CreateFileW(reinterpret_cast<LPCWSTR>(path_.utf16()), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return Failure("outcome-log-open");
        BY_HANDLE_FILE_INFORMATION Info{};
        if (!GetFileInformationByHandle(file_, &Info) || (Info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
            || Info.nNumberOfLinks != 1) return Failure("outcome-log-file-type");
        auto Index = DiskRecords::Create(); if (!Index.HasValue()) return Index.Failure(); index_ = Index.Value();
        const uint64_t Size = (uint64_t(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow;
        while (end_ < Size) {
            if (Size - end_ < HeaderBytes) break; // Interrupted final append.
            auto Header = readAt(end_, HeaderBytes); if (!Header.HasValue()) return Header.Failure();
            QDataStream In(Header.Value()); uint32_t Tag = 0; quint64 Row = 0, Length = 0;
            In >> Tag >> Row >> Length;
            if (Tag != Magic || Row > index_->Count() || Length > MaxRecordBytes) return Failure("outcome-log-corrupt");
            if (Length > Size - end_ - HeaderBytes) break;
            auto Payload = readAt(end_ + HeaderBytes, Length); if (!Payload.HasValue()) return Payload.Failure();
            if (QCryptographicHash::hash(Header.Value().left(20) + Payload.Value(), QCryptographicHash::Sha256) != Header.Value().right(32)) return Failure("outcome-log-checksum");
            auto Decoded = DecodeOutcome(Payload.Value()); if (!Decoded.HasValue()) return Decoded.Failure();
            auto Indexed = index(Row, end_ + HeaderBytes, Length); if (!Indexed.HasValue()) return Indexed;
            end_ += HeaderBytes + Length;
        }
        if (end_ != Size) {
            LARGE_INTEGER At; At.QuadPart = LONGLONG(end_);
            if (!SetFilePointerEx(file_, At, nullptr, FILE_BEGIN) || !SetEndOfFile(file_) || !FlushFileBuffers(file_)) return Failure("outcome-log-tail-repair");
        }
        return Unit{};
    }
    uint64_t Count() const override { std::lock_guard Lock(mutex_); return index_->Count(); }
    Result<uint64_t> Append(const ItemOutcome& Item) override
    {
        std::lock_guard Lock(mutex_); const auto Row = index_->Count();
        auto Written = write(Row, Item); if (!Written.HasValue()) return Written.Failure(); return Row;
    }
    Result<Unit> Replace(uint64_t Row, const ItemOutcome& Item) override
    {
        std::lock_guard Lock(mutex_);
        if (Row >= index_->Count()) return Failure("outcome-row-range");
        return write(Row, Item);
    }
    Result<ItemOutcomePage> Read(uint64_t Offset, size_t MaxItems, const OperationContext& Context) const override
    {
        std::lock_guard Lock(mutex_);
        if (!MaxItems || Offset > index_->Count()) return Failure("outcome-page-range");
        ItemOutcomePage Page; const auto End = Offset + std::min<uint64_t>(MaxItems, index_->Count() - Offset);
        for (auto Row = Offset; Row < End; ++Row) {
            if (auto Error = Context.Check({})) return *Error;
            auto Pointer = index_->Read(Row); if (!Pointer.HasValue()) return Pointer.Failure();
            QDataStream In(Pointer.Value()); quint64 Position = 0, Size = 0; In >> Position >> Size;
            auto Bytes = readAt(Position, Size); if (!Bytes.HasValue()) return Bytes.Failure();
            auto Decoded = DecodeOutcome(Bytes.Value()); if (!Decoded.HasValue()) return Decoded.Failure();
            Page.items_.push_back(std::move(Decoded.Value()));
        }
        Page.end_ = End == index_->Count(); return Page;
    }
private:
    Result<QByteArray> readAt(uint64_t Position, uint64_t Size) const
    {
        if (Size > MaxRecordBytes) return Failure("outcome-record-size");
        LARGE_INTEGER At; At.QuadPart = LONGLONG(Position); DWORD Read = 0;
        QByteArray Bytes(qsizetype(Size), Qt::Uninitialized);
        if (!SetFilePointerEx(file_, At, nullptr, FILE_BEGIN) || !ReadFile(file_, Bytes.data(), DWORD(Size), &Read, nullptr)
            || Read != Size) return Failure("outcome-log-read");
        return Bytes;
    }
    Result<Unit> index(uint64_t Row, uint64_t Position, uint64_t Size)
    {
        QByteArray Pointer; QDataStream Out(&Pointer, QIODevice::WriteOnly); Out << quint64(Position) << quint64(Size);
        if (Row == index_->Count()) { auto Added = index_->Append(Pointer); if (!Added.HasValue()) return Added.Failure(); }
        else { auto Replaced = index_->Replace(Row, Pointer); if (!Replaced.HasValue()) return Replaced; }
        return Unit{};
    }
    Result<Unit> write(uint64_t Row, const ItemOutcome& Item)
    {
        if (failed_) return Failure("outcome-log-write-failed");
        const auto Payload = EncodeOutcome(Item);
        if (uint64_t(Payload.size()) > MaxRecordBytes) return Failure("outcome-record-size");
        QByteArray Frame; QDataStream Out(&Frame, QIODevice::WriteOnly);
        Out << Magic << quint64(Row) << quint64(Payload.size());
        Frame += QCryptographicHash::hash(Frame + Payload, QCryptographicHash::Sha256); Frame += Payload;
        if (end_ > uint64_t(INT64_MAX) - uint64_t(Frame.size())) return Failure("outcome-log-offset-range");
        LARGE_INTEGER At; At.QuadPart = LONGLONG(end_); DWORD Written = 0;
        if (!SetFilePointerEx(file_, At, nullptr, FILE_BEGIN) || !WriteFile(file_, Frame.constData(), DWORD(Frame.size()), &Written, nullptr)
            || Written != Frame.size() || !FlushFileBuffers(file_)) { failed_ = true; return Failure("outcome-log-write"); }
        const auto Indexed = index(Row, end_ + HeaderBytes, uint64_t(Payload.size())); end_ += uint64_t(Frame.size());
        if (!Indexed.HasValue()) failed_ = true;
        return Indexed;
    }
    QString path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::shared_ptr<DiskRecords> index_;
    uint64_t end_ = 0;
    bool failed_ = false;
    mutable std::mutex mutex_;
};
}
Result<std::shared_ptr<IItemOutcomeStore>> OpenDurableOutcomes(const QString& Path)
{
    auto Store = std::make_shared<DurableOutcomes>(QDir::toNativeSeparators(Path));
    auto Opened = Store->Open(); if (!Opened.HasValue()) return Opened.Failure();
    return std::shared_ptr<IItemOutcomeStore>(std::move(Store));
}
}
