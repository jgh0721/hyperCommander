#pragma once
#include "commonLibs/system/environmentAnalysis.hpp"
#include <chrono>
#include <mutex>
namespace hc::environment {
// Each analysis/search has one total budget; slow paths cannot multiply the
// per-directory timeout into minutes. Unvisited paths remain explicitly unknown.
class BudgetedPathProbe final : public IPathProbe
{
public:
    explicit BudgetedPathProbe(IPathProbe& Inner, const std::atomic_bool* Cancel = nullptr,
        std::chrono::milliseconds Budget = std::chrono::seconds(2))
        : inner_(Inner), cancel_(Cancel), deadline_(std::chrono::steady_clock::now() + Budget) {}
    DirectoryProbe Inspect(const std::wstring& Path) override {
        if (cancel_ && cancel_->load()) return {ProbeStatus::Cancelled};
        if (std::chrono::steady_clock::now() >= deadline_) return {ProbeStatus::Timeout};
        return inner_.Inspect(Path);
    }
private:
    IPathProbe& inner_;
    const std::atomic_bool* cancel_;
    std::chrono::steady_clock::time_point deadline_;
};
// Native probes own their inputs and never call a model from a worker thread.
// At most four timed-out I/O workers may remain outstanding process-wide.
class NativePathProbe final : public IPathProbe
{
public:
    DirectoryProbe Inspect(const std::wstring& Path) override;
    void Invalidate();
private:
    struct CacheEntry { DirectoryProbe value_; std::chrono::steady_clock::time_point time_; };
    std::map<std::wstring, CacheEntry, NameLess> cache_;
    std::mutex mutex_;
};
}
