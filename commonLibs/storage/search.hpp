#pragma once
#include "storage.hpp"
#include <functional>

namespace hc::storage {
enum class SearchFreshness { Enumerated, IndexUnverified };
struct SearchScope {
    Locator root_;
    bool recursive_ = true;
    std::size_t maxDepth_ = 32;
    std::size_t maxVisited_ = 100000;
};
struct SearchPage {
    std::vector<EntrySnapshot> matches_;
    std::vector<Error> warnings_;
    std::size_t visited_ = 0;
    bool end_ = false;
    bool partial_ = false;
    SearchFreshness freshness_ = SearchFreshness::Enumerated;
    std::chrono::system_clock::time_point observedAt_;
};
using SearchMatcher = std::function<bool(const EntrySnapshot&)>;
class ISearchCursor {
public:
    virtual ~ISearchCursor() = default;
    // One outstanding Next. An empty nonterminal page means the scan budget
    // was consumed; callers can cancel or request the next bounded scan.
    virtual Task<SearchPage> Next(std::size_t MaxItems, OperationContext Context) = 0;
};
class ISearchProvider {
public:
    virtual ~ISearchProvider() = default;
    virtual Result<std::shared_ptr<ISearchCursor>> Search(SearchScope Scope, SearchMatcher Matcher) = 0;
};
std::shared_ptr<ISearchProvider> MakeEnumeratingSearch(std::shared_ptr<IStorageProvider> Provider);
}
