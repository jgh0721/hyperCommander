#pragma once
#include "qtPagedStorage.hpp"
#include "qtWorkspaceState.hpp"

namespace hc::refresh {
inline constexpr auto OrderHint = "hc.refreshOrder";
inline hc::storage::PagedEntryIndex::Less ArrivalOrder() {
    return [](const auto& A, const auto& B) {
        const auto Left = A.hints_.find(OrderHint), Right = B.hints_.find(OrderHint);
        const auto Rank = [](const auto& It, const auto& Entry) {
            return It == Entry.hints_.end() ? quint64(0) : QString::fromStdString(It->second).toULongLong();
        };
        return Rank(Left, A) < Rank(Right, B);
    };
}

// Every pass is bounded by a cursor page. The identity join uses the existing
// external sorter rather than retaining every filename in a large directory.
class OrderCursor final : public hc::storage::IEntryCursor {
public:
    explicit OrderCursor(std::shared_ptr<hc::storage::PagedEntryIndex> Index,
        std::vector<hc::storage::EntrySnapshot> Rows = {}) : index_(std::move(Index)), rows_(std::move(Rows)) {}
    hc::storage::Task<hc::storage::EntryPage> Next(std::size_t MaxItems, hc::storage::OperationContext Context) override {
        using namespace hc::storage;
        Result<EntryPage> ResultPage = index_ ? index_->Read(offset_, MaxItems, Context) : Result<EntryPage>(EntryPage{});
        if (!ResultPage.HasValue()) return Ready<EntryPage>(ResultPage.Failure());
        auto Page = std::move(ResultPage.Value());
        if (!index_) {
            const auto End = std::min<std::size_t>(rows_.size(), offset_ + MaxItems);
            for (auto I = offset_; I < End; ++I) Page.entries_.push_back(rows_[I]);
            Page.end_ = End == rows_.size();
        }
        for (auto& Entry : Page.entries_) Entry.hints_[OrderHint] = std::to_string(offset_++);
        return Ready<EntryPage>(std::move(Page));
    }
private:
    std::shared_ptr<hc::storage::PagedEntryIndex> index_;
    std::vector<hc::storage::EntrySnapshot> rows_;
    std::uint64_t offset_ = 0;
};

inline hc::storage::Result<std::shared_ptr<hc::storage::PagedEntryIndex>> AppendNewEntries(
    const std::shared_ptr<hc::storage::PagedEntryIndex>& Current,
    const std::shared_ptr<hc::storage::PagedEntryIndex>& Previous,
    std::vector<hc::storage::EntrySnapshot> PreviousRows, const hc::storage::OperationContext& Context) {
    using namespace hc::storage;
    OrderCursor Ordered(Previous, std::move(PreviousRows));
    auto Old = PagedEntryIndex::Build(Ordered, Context); if (!Old.HasValue()) return Old.Failure();
    const auto ByKey = [](const EntrySnapshot& A, const EntrySnapshot& B) {
        return hc::workspace::SelectionKey(A) < hc::workspace::SelectionKey(B);
    };
    const auto All = [](const EntrySnapshot&) { return true; };
    auto OldKeys = Old.Value()->Project(ByKey, All, Context, {}); if (!OldKeys.HasValue()) return OldKeys.Failure();
    auto NewKeys = Current->Project(ByKey, All, Context, {}); if (!NewKeys.HasValue()) return NewKeys.Failure();
    class JoinCursor final : public IEntryCursor {
    public:
        JoinCursor(std::shared_ptr<PagedEntryIndex> New, std::shared_ptr<PagedEntryIndex> Old, std::uint64_t Count)
            : next_(std::move(New)), old_(std::move(Old)), arrival_(Count) {}
        Task<EntryPage> Next(std::size_t MaxItems, OperationContext Context) override {
            auto Read = next_->Read(offset_, MaxItems, Context);
            if (!Read.HasValue()) return Ready<EntryPage>(Read.Failure());
            auto Page = std::move(Read.Value()); offset_ += Page.entries_.size();
            for (auto& Entry : Page.entries_) {
                const auto Key = hc::workspace::SelectionKey(Entry);
                for (;;) {
                    if (oldPosition_ == oldPage_.entries_.size() && oldOffset_ < old_->Count()) {
                        auto OldRead = old_->Read(oldOffset_, MaxItems, Context);
                        if (!OldRead.HasValue()) return Ready<EntryPage>(OldRead.Failure());
                        oldPage_ = std::move(OldRead.Value()); oldOffset_ += oldPage_.entries_.size(); oldPosition_ = 0;
                    }
                    if (oldPosition_ == oldPage_.entries_.size()) break;
                    if (hc::workspace::SelectionKey(oldPage_.entries_[oldPosition_]) >= Key) break;
                    ++oldPosition_;
                }
                if (oldPosition_ < oldPage_.entries_.size() && hc::workspace::SelectionKey(oldPage_.entries_[oldPosition_]) == Key)
                    Entry.hints_[OrderHint] = oldPage_.entries_[oldPosition_].hints_.at(OrderHint);
                else Entry.hints_[OrderHint] = std::to_string(arrival_++);
            }
            return Ready<EntryPage>(std::move(Page));
        }
    private:
        std::shared_ptr<PagedEntryIndex> next_, old_;
        EntryPage oldPage_;
        std::uint64_t offset_ = 0, oldOffset_ = 0, arrival_;
        std::size_t oldPosition_ = 0;
    };
    JoinCursor Join(NewKeys.Value(), OldKeys.Value(), Old.Value()->Count());
    auto Tagged = PagedEntryIndex::Build(Join, Context); if (!Tagged.HasValue()) return Tagged.Failure();
    return Tagged.Value()->Project(ArrivalOrder(), All, Context, {});
}
}
