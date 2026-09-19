#include "search.hpp"
#include <deque>
#include <set>

namespace hc::storage {
namespace {
class SearchCursor final : public ISearchCursor, public std::enable_shared_from_this<SearchCursor> {
public:
    SearchCursor(std::shared_ptr<IStorageProvider> Provider, SearchScope Scope, SearchMatcher Matcher)
        : provider_(std::move(Provider)), scope_(std::move(Scope)), matcher_(std::move(Matcher))
    { pending_.push_back({scope_.root_, 0}); seen_.insert(Serialize(scope_.root_)); }
    Task<SearchPage> Next(std::size_t MaxItems, OperationContext Context) override
    {
        auto Self = shared_from_this();
        return std::async(std::launch::async, [Self, MaxItems, Context] { return Self->next(MaxItems, Context); });
    }
private:
    Result<SearchPage> next(std::size_t MaxItems, OperationContext Context)
    {
        std::unique_lock Lock(mutex_, std::try_to_lock);
        if (!Lock.owns_lock()) return Error{ErrorCategory::InvalidRequest, "search-next-pending", provider_->Id()};
        if (!MaxItems || !Context.maxPageItems_) return Error{ErrorCategory::InvalidRequest, "search-page-size", provider_->Id()};
        if (const auto Failure = Context.Check(provider_->Id())) return *Failure;
        SearchPage Page; Page.observedAt_ = std::chrono::system_clock::now();
        const auto Limit = std::min({MaxItems, Context.maxPageItems_, std::size_t(256)});
        std::size_t Scanned = 0;
        while (Page.matches_.size() < Limit && Scanned < 256 && !ended_)
        {
            if (const auto Failure = Context.Check(provider_->Id())) return *Failure;
            if (!cursor_ && pending_.empty()) { ended_ = true; break; }
            if (visited_ >= scope_.maxVisited_) {
                Page.warnings_.push_back({ErrorCategory::BudgetExceeded, "search-visit-limit", provider_->Id()});
                partial_ = ended_ = true; break;
            }
            if (!cursor_)
            {
                if (pending_.empty()) { ended_ = true; break; }
                auto [Location, Depth] = std::move(pending_.front()); pending_.pop_front(); depth_ = Depth;
                auto Open = provider_->Enumerate(std::move(Location), Context).get();
                if (!Open.HasValue()) {
                    if (Depth == 0 || Open.Failure().category_ == ErrorCategory::Cancelled) return Open.Failure();
                    Page.warnings_.push_back(Open.Failure()); partial_ = true; ++Scanned; continue;
                }
                cursor_ = std::move(Open.Value());
            }
            // Ask only for the remaining match capacity; no unconsumed entries
            // are dropped when a page fills halfway through a directory.
            const auto Requested = std::min(Limit - Page.matches_.size(), 256 - Scanned);
            auto Read = cursor_->Next(Requested, Context).get();
            if (!Read.HasValue()) return Read.Failure();
            if (Read.Value().entries_.size() > Requested) return Error{ErrorCategory::Internal, "search-provider-page-size", provider_->Id()};
            if (Read.Value().entries_.empty() && !Read.Value().end_) break;
            for (auto& Entry : Read.Value().entries_)
            {
                if (visited_ >= scope_.maxVisited_) { partial_ = ended_ = true;
                    Page.warnings_.push_back({ErrorCategory::BudgetExceeded, "search-visit-limit", provider_->Id()}); break; }
                ++Scanned; ++visited_;
                if (Entry.directory_ && scope_.recursive_ && (Entry.attributes_ & 0x400)) partial_ = true;
                // Native reparse points are not followed: junctions may escape
                // the selected scope or create an unbounded traversal cycle.
                if (Entry.directory_ && scope_.recursive_ && !(Entry.attributes_ & 0x400))
                {
                    if (depth_ < scope_.maxDepth_) {
                        if (seen_.insert(Serialize(Entry.locator_)).second) pending_.push_back({Entry.locator_, depth_ + 1});
                    } else { partial_ = true; Page.warnings_.push_back({ErrorCategory::BudgetExceeded, "search-depth-limit", provider_->Id()}); }
                }
                if (matcher_(Entry)) Page.matches_.push_back(std::move(Entry));
            }
            if (Read.Value().end_) cursor_.reset();
        }
        Page.visited_ = visited_; Page.end_ = ended_ || (!cursor_ && pending_.empty()); Page.partial_ = partial_;
        if (const auto Failure = Context.Check(provider_->Id())) return *Failure;
        return Page;
    }
    std::shared_ptr<IStorageProvider> provider_;
    SearchScope scope_;
    SearchMatcher matcher_;
    std::deque<std::pair<Locator, std::size_t>> pending_;
    std::set<std::string> seen_;
    std::unique_ptr<IEntryCursor> cursor_;
    std::mutex mutex_;
    std::size_t depth_ = 0, visited_ = 0;
    bool ended_ = false, partial_ = false;
};
class EnumeratingSearch final : public ISearchProvider {
public:
    explicit EnumeratingSearch(std::shared_ptr<IStorageProvider> Provider) : provider_(std::move(Provider)) {}
    Result<std::shared_ptr<ISearchCursor>> Search(SearchScope Scope, SearchMatcher Matcher) override {
        if (!provider_ || !Matcher || Scope.root_.root_.empty() || !Scope.maxVisited_ || Scope.maxVisited_ > 1000000 || Scope.maxDepth_ > 128)
            return Error{ErrorCategory::InvalidRequest, "search-scope"};
        return std::shared_ptr<ISearchCursor>(new SearchCursor(provider_, std::move(Scope), std::move(Matcher)));
    }
private:
    std::shared_ptr<IStorageProvider> provider_;
};
}
std::shared_ptr<ISearchProvider> MakeEnumeratingSearch(std::shared_ptr<IStorageProvider> Provider)
{ return std::make_shared<EnumeratingSearch>(std::move(Provider)); }
}
