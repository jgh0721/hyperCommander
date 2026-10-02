#pragma once
#include "fileOperation.hpp"

namespace hc::operations {
inline std::string ResourceLane(storage::Locator At)
{
    auto Path = At.root_; std::replace(Path.begin(), Path.end(), '/', '\\');
    if (Path.starts_with("\\\\?\\UNC\\")) Path = "\\\\" + Path.substr(8);
    else if (Path.starts_with("\\\\?\\")) Path.erase(0, 4);
    while (!Path.empty() && Path.back() == '\\') Path.pop_back();
    for (auto& Letter : Path) if (Letter >= 'A' && Letter <= 'Z') Letter += 'a' - 'A';
    if (At.kind_ == storage::LocatorKind::Archive) return "archive:" + Path;
    if (At.kind_ == storage::LocatorKind::Plugin) return "plugin:" + Path.substr(0, Path.find('\\'));
    if (Path.starts_with("\\\\")) {
        const auto Server = Path.find('\\', 2); const auto Share = Server == std::string::npos ? Server : Path.find('\\', Server + 1);
        return "share:" + Path.substr(0, Share);
    }
    return "volume:" + Path.substr(0, 2);
}
// Legacy atomic archive APIs need a full change vector. Read the immutable
// selection on the worker in cancellable pages while retaining its lease.
class SelectionPlan final : public IJobTask {
public:
    using Prepare = std::function<storage::Result<PreparedJob>(std::vector<storage::EntrySnapshot>, storage::OperationContext)>;
    SelectionPlan(std::shared_ptr<storage::ISelectionManifest> Selection, storage::Locator Root, Prepare Build, uint64_t Limit)
        : selection_(std::move(Selection)), root_(std::move(Root)), build_(std::move(Build)), limit_(Limit) {}
    JobStep Advance(JobStepContext Context) override
    {
        if (inner_) return inner_->Advance(std::move(Context));
        if (const auto Stop = Context.operation_.Check({})) return failed(*Stop);
        if (entries_.size() < selection_->Count()) {
            auto Page = selection_->Read(entries_.size(), 256, Context.operation_); if (!Page.HasValue()) return failed(Page.Failure());
            if (Page.Value().entries_.empty() || Page.Value().entries_.size() > selection_->Count() - entries_.size())
                return failed({storage::ErrorCategory::InvalidRequest, "selection-page-size"});
            if (Page.Value().entries_.size() > limit_ - entries_.size()) return failed({storage::ErrorCategory::BudgetExceeded, "selection-entry-budget"});
            for (auto& Entry : Page.Value().entries_) {
                auto Expected = root_;
                auto& Path = Expected.kind_ == storage::LocatorKind::Archive ? Expected.nesting_.back().entry_ : Expected.root_;
                if (!Path.ends_with('/') && !Path.ends_with('\\')) Path += Expected.kind_ == storage::LocatorKind::Archive ? '/' : '\\';
                Path += Entry.name_;
                auto Actual = Entry.locator_;
                if (Expected.kind_ == storage::LocatorKind::Local || Expected.kind_ == storage::LocatorKind::Smb) {
                    std::replace(Expected.root_.begin(), Expected.root_.end(), '/', '\\');
                    std::replace(Actual.root_.begin(), Actual.root_.end(), '/', '\\');
                }
                if (Entry.name_.empty() || Entry.name_ == "." || Entry.name_ == ".." || Entry.name_.find_first_of("/\\:") != std::string::npos
                    || Entry.name_.find('\0') != std::string::npos || Entry.id_.providerId_ != selection_->Provider()->Id() || Expected != Actual)
                    return failed({storage::ErrorCategory::InvalidRequest, "selection-container-mismatch"});
                entries_.push_back(std::move(Entry));
            }
            return JobStep{JobState::Preparing};
        }
        auto Prepared = build_(std::move(entries_), Context.operation_); if (!Prepared.HasValue()) return failed(Prepared.Failure());
        inner_ = std::move(Prepared.Value().task_); build_ = {}; return JobStep{JobState::Preparing};
    }
    std::vector<storage::Error> Finalize(JobState State) override
    {
        auto Errors = inner_ ? inner_->Finalize(State) : std::vector<storage::Error>{};
        inner_.reset(); selection_.reset(); entries_.clear(); build_ = {}; return Errors;
    }
private:
    static JobStep failed(storage::Error Failure) { JobStep Step{JobState::Failed}; Step.error_ = std::move(Failure); return Step; }
    std::shared_ptr<storage::ISelectionManifest> selection_;
    storage::Locator root_;
    Prepare build_;
    uint64_t limit_;
    std::vector<storage::EntrySnapshot> entries_;
    std::unique_ptr<IJobTask> inner_;
};
}
