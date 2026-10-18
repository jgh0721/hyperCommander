#pragma once
#include "jobScheduler.hpp"
#include "../storage/writableStorage.hpp"

namespace hc::operations {
// Creates only missing parents, one scheduler commit per directory. Every
// created directory has an outcome; cancellation retains completed parents.
class DirectoryPreparation final : public IJobTask {
public:
    DirectoryPreparation(storage::Locator Parent, std::shared_ptr<storage::IWritableStorage> Writer, std::unique_ptr<IJobTask> Inner)
        : current_(std::move(Parent)), writer_(std::move(Writer)), inner_(std::move(Inner)) {}
    JobStep Advance(JobStepContext Context) override {
        if (const auto Error = Context.operation_.Check(writer_->Id())) return failed(*Error);
        if (planning_) {
            if (missing_.size() >= 128) return failed({storage::ErrorCategory::BudgetExceeded, "directory-depth-limit"});
            const auto Entry = writer_->Inspect(current_, Context.operation_); if (!Entry.HasValue()) return failed(Entry.Failure());
            if (Entry.Value()) {
                if (!Entry.Value()->directory_) return failed({storage::ErrorCategory::InvalidRequest, "parent-is-not-directory", writer_->Id()});
                planning_ = false; return JobStep{JobState::Running};
            }
            missing_.push_back(current_); const auto Parent = ParentOf(current_);
            if (Parent == current_ || Parent.root_.empty()) return failed({storage::ErrorCategory::NotFound, "directory-root-missing", writer_->Id()});
            current_ = Parent; return JobStep{JobState::Preparing};
        }
        if (!missing_.empty()) {
            const auto At = missing_.back();
            if (!Context.committing_) return JobStep{JobState::Committing};
            const auto Existing = writer_->Inspect(At, Context.operation_); if (!Existing.HasValue()) return failed(Existing.Failure());
            if (Existing.Value() && !Existing.Value()->directory_) return failed({storage::ErrorCategory::InvalidRequest, "parent-is-not-directory", writer_->Id()});
            if (!Existing.Value()) {
                const auto Made = writer_->MakeDirectory(At, Context.operation_); if (!Made.HasValue()) return failed(Made.Failure());
            }
            missing_.pop_back(); ++parentOutcomes_; JobStep Step{JobState::Running};
            Step.item_ = ItemOutcome{storage::Serialize(At), Existing.Value() ? ItemOutcome::Status::Skipped : ItemOutcome::Status::Completed, {}, true, false};
            return Step;
        }
        auto Step = inner_->Advance(Context);
        if (Step.progress_ && Step.progress_->plannedItems_) *Step.progress_->plannedItems_ += parentOutcomes_;
        return Step;
    }
    std::vector<storage::Error> Finalize(JobState State) override { return inner_->Finalize(State); }
    static storage::Locator ParentOf(storage::Locator At) {
        auto& Path = At.root_;
        while (Path.size() > 3 && (Path.back() == '/' || Path.back() == '\\')) Path.pop_back();
        const auto End = Path.find_last_of("/\\");
        if (End == std::string::npos || (End == 2 && Path[1] == ':')) { if (End == 2) Path.resize(3); return At; }
        Path.resize(End == 0 ? 1 : End); return At;
    }
private:
    static JobStep failed(storage::Error Error) { JobStep Step{JobState::Failed}; Step.error_ = std::move(Error); return Step; }
    storage::Locator current_; std::shared_ptr<storage::IWritableStorage> writer_; std::unique_ptr<IJobTask> inner_;
    std::vector<storage::Locator> missing_; bool planning_ = true;
    std::uint64_t parentOutcomes_ = 0;
};
}
