#include "qtItemOutcomeStore.hpp"
#include <QDataStream>
#include <QIODevice>

namespace hc::operations
{
namespace
{
    QByteArray Encode(const ItemOutcome& Item)
    {
        QByteArray Bytes; QDataStream Out(&Bytes, QIODevice::WriteOnly); Out.setVersion(QDataStream::Qt_6_0);
        Out << quint32(1) << QByteArray::fromStdString(Item.item_) << quint8(Item.status_)
            << Item.destinationCommitted_ << Item.sourceRemoved_ << bool(Item.source_);
        if (Item.source_) Out << storage::EncodeEntry(*Item.source_);
        Out << bool(Item.error_);
        if (Item.error_) {
            const auto& Failure = *Item.error_;
            Out << quint8(Failure.category_) << QByteArray::fromStdString(Failure.code_)
                << QByteArray::fromStdString(Failure.provider_.value_) << Failure.retryable_ << QByteArray::fromStdString(Failure.userAction_) << bool(Failure.resource_);
            if (Failure.resource_) { storage::EntrySnapshot Entry; Entry.id_ = *Failure.resource_; Out << storage::EncodeEntry(Entry); }
        }
        return Bytes;
    }
    Result<ItemOutcome> Decode(const QByteArray& Bytes)
    {
        const auto Corrupt = [] { return Error{storage::ErrorCategory::Unavailable, "result-record-corrupt"}; };
        QDataStream In(Bytes); In.setVersion(QDataStream::Qt_6_0);
        ItemOutcome Item; quint32 Version = 0; quint8 Status = 0; QByteArray Name, Source; bool HasSource = false, HasError = false;
        In >> Version >> Name >> Status >> Item.destinationCommitted_ >> Item.sourceRemoved_ >> HasSource;
        if (Version != 1 || Status > quint8(ItemOutcome::Status::Failed)) return Corrupt();
        Item.item_ = Name.toStdString(); Item.status_ = ItemOutcome::Status(Status);
        if (HasSource) {
            In >> Source; auto Entry = storage::DecodeEntry(Source); if (!Entry.HasValue()) return Entry.Failure();
            Item.source_ = std::move(Entry.Value());
        }
        In >> HasError;
        if (HasError) {
            Error Failure; quint8 Category = 0; QByteArray Code, Provider, Action; bool HasResource = false;
            In >> Category >> Code >> Provider >> Failure.retryable_ >> Action >> HasResource;
            if (Category > quint8(storage::ErrorCategory::Internal)) return Corrupt();
            Failure.category_ = storage::ErrorCategory(Category); Failure.code_ = Code.toStdString();
            Failure.provider_.value_ = Provider.toStdString(); Failure.userAction_ = Action.toStdString();
            if (HasResource) {
                In >> Source; auto Entry = storage::DecodeEntry(Source); if (!Entry.HasValue()) return Entry.Failure();
                Failure.resource_ = std::move(Entry.Value().id_);
            }
            Item.error_ = std::move(Failure);
        }
        if (In.status() != QDataStream::Ok || !In.atEnd()) return Corrupt();
        return Item;
    }
}
Result<std::shared_ptr<IItemOutcomeStore>> PagedItemOutcomeStore::Create(const QString& Parent)
{
    auto Records = storage::DiskRecords::Create(Parent); if (!Records.HasValue()) return Records.Failure();
    return std::shared_ptr<IItemOutcomeStore>(new PagedItemOutcomeStore(Records.Value()));
}
std::uint64_t PagedItemOutcomeStore::Count() const { std::lock_guard Lock(mutex_); return records_->Count(); }
Result<std::uint64_t> PagedItemOutcomeStore::Append(const ItemOutcome& Item)
{
    std::lock_guard Lock(mutex_);
    auto Written = records_->Append(Encode(Item)); if (!Written.HasValue()) return Written.Failure();
    auto Flushed = records_->Flush(); if (!Flushed.HasValue()) return Flushed.Failure();
    return Written.Value();
}
Result<storage::Unit> PagedItemOutcomeStore::Replace(std::uint64_t Row, const ItemOutcome& Item)
{
    std::lock_guard Lock(mutex_);
    auto Written = records_->Replace(Row, Encode(Item)); if (!Written.HasValue()) return Written.Failure();
    return records_->Flush();
}
Result<ItemOutcomePage> PagedItemOutcomeStore::Read(std::uint64_t Offset, std::size_t MaxItems, const storage::OperationContext& Context) const
{
    std::lock_guard Lock(mutex_);
    if (Offset > records_->Count() || !MaxItems) return Error{storage::ErrorCategory::InvalidRequest, "result-page-out-of-range"};
    if (auto Failure = Context.Check({})) return *Failure;
    ItemOutcomePage Page; const auto End = Offset + std::min<std::uint64_t>(MaxItems, records_->Count() - Offset);
    for (auto Row = Offset; Row < End; ++Row) {
        if (auto Failure = Context.Check({})) return *Failure;
        auto Bytes = records_->Read(Row); if (!Bytes.HasValue()) return Bytes.Failure();
        auto Item = Decode(Bytes.Value()); if (!Item.HasValue()) return Item.Failure();
        Page.items_.push_back(std::move(Item.Value()));
    }
    Page.end_ = End == records_->Count(); return Page;
}
}
