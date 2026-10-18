#include "archiveExtractJob.hpp"
#include "commonLibs/operations/providerInteractionTask.hpp"
#include "commonLibs/operations/directoryPreparation.hpp"
#include <QDir>
#include <QFileInfo>

namespace hc::operations {
using namespace storage;
namespace {
class EmptyExtract final : public IJobTask {
public:
    JobStep Advance(JobStepContext) override { return JobStep{JobState::Completed}; }
    std::vector<Error> Finalize(JobState) override { return {}; }
};
class ExtractTask final : public IJobTask {
public:
    explicit ExtractTask(ExtractRequest Request) : request_(std::move(Request)) {}
    JobStep Advance(JobStepContext Context) override {
        if (const auto Error = Context.operation_.Check({"archive-extract"})) return failed(*Error);
        if (request_.selection_) {
            const auto Count = request_.selection_->Count();
            if (selectionOffset_ < Count) {
                auto Page = request_.selection_->Read(selectionOffset_, 256, Context.operation_);
                if (!Page.HasValue()) return failed(Page.Failure());
                if (Page.Value().entries_.empty() || Page.Value().entries_.size() > 256 || Page.Value().entries_.size() > Count-selectionOffset_
                    || (Page.Value().end_ && selectionOffset_ + Page.Value().entries_.size() != Count)) return failed({ErrorCategory::StaleRevision, "extract-selection-incomplete"});
                for (const auto& Entry : Page.Value().entries_) request_.archives_.push_back({Entry, request_.selection_->Provider(), {}});
                selectionOffset_ += Page.Value().entries_.size(); return JobStep{JobState::Preparing};
            }
            request_.selection_.reset();
        }
        if (pinned_ < request_.archives_.size()) {
            const auto& Source = request_.archives_[pinned_];
            if (Source.entry_.directory_ || (Source.entry_.locator_.kind_ != LocatorKind::Local && Source.entry_.locator_.kind_ != LocatorKind::Smb) || !Source.reader_)
                return failed({ErrorCategory::Unsupported, "extract-native-archive-file-required"});
            if (request_.readers_(Source.entry_).empty()) return failed({ErrorCategory::Unsupported, "extract-no-configured-plugin: " + Source.entry_.name_});
            auto PinnedContext = Context.operation_; PinnedContext.denyConcurrentMutation_ = true;
            auto Pin = Source.reader_->OpenRead(Source.entry_.id_, PinnedContext).get(); if (!Pin.HasValue()) return failed(Pin.Failure());
            pins_.push_back(std::move(Pin.Value())); ++pinned_; return JobStep{JobState::Preparing};
        }
        if (inner_) {
            auto Step = inner_->Advance(Context);
            if (Step.replaceItem_) *Step.replaceItem_ += itemOffset_;
            else if (Step.item_) ++currentItems_;
            if (Step.progress_) {
                currentBytes_ = Step.progress_->bytesDone_; Step.progress_->bytesDone_ += bytesOffset_;
                // Each archive is planned independently; the final batch total
                // is unknown until all archive contents have been enumerated.
                if (request_.archives_.size()>1) { Step.progress_->bytesTotal_.reset(); Step.progress_->plannedItems_.reset(); }
            }
            if (Step.next_ == JobState::Completed) {
                auto Errors = inner_->Finalize(JobState::Completed); inner_.reset();
                if (!Errors.empty()) { cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); return failed({ErrorCategory::Unavailable, "extract-cleanup-incomplete"}); }
                itemOffset_ += currentItems_; currentItems_ = 0; bytesOffset_ += currentBytes_; currentBytes_ = 0;
                ++archive_; cursor_.reset(); reader_.reset(); entries_.clear(); Step.next_ = JobState::Preparing;
            }
            return Step;
        }
        if (archive_ == request_.archives_.size()) return JobStep{JobState::Completed};
        const auto& Source = request_.archives_[archive_];
        if (!reader_) {
            const auto Candidates = request_.readers_(Source.entry_);
            if (Candidates.empty()) return failed({ErrorCategory::Unsupported, "extract-no-configured-plugin: " + Source.entry_.name_});
            Error Last{ErrorCategory::Unsupported, "extract-cannot-open"};
            root_ = {LocatorKind::Archive, Source.entry_.locator_.root_, {{"wcx", "/"}}};
            for (const auto& Candidate : Candidates) {
                auto Opened = Candidate->Enumerate(root_, Context.operation_).get();
                if (Opened.HasValue()) { reader_ = Candidate; cursor_ = std::move(Opened.Value()); break; }
                Last = Opened.Failure();
                if (Last.category_ == ErrorCategory::Cancelled || Last.category_ == ErrorCategory::AccessDenied) break;
            }
            if (!reader_) return failed(Last);
            target_ = request_.destination_;
            if (request_.separateFolders_) {
                const auto Name = QFileInfo(QString::fromStdString(Source.entry_.name_)).completeBaseName();
                if (Name.isEmpty() || Name == "." || Name == ".." || Name.contains('/') || Name.contains('\\')) return failed({ErrorCategory::InvalidRequest, "extract-folder-name"});
                target_.root_ = QDir(QString::fromStdString(target_.root_)).filePath(Name).toStdString();
            }
            return JobStep{JobState::Preparing};
        }
        if (!listed_) {
            auto Page = cursor_->Next(256, Context.operation_).get(); if (!Page.HasValue()) return failed(Page.Failure());
            if (entries_.size() + Page.Value().entries_.size() > Context.operation_.EnumerationLimit()) return failed({ErrorCategory::BudgetExceeded, "extract-entry-budget"});
            if (Page.Value().entries_.empty() && !Page.Value().end_) return failed({ErrorCategory::Unavailable, "extract-list-no-progress"});
            for (const auto& Entry : Page.Value().entries_) entries_.push_back({Entry, reader_, {}});
            listed_ = Page.Value().end_; return JobStep{JobState::Preparing};
        }
        if (entries_.empty()) inner_ = std::make_unique<DirectoryPreparation>(target_, request_.writer_, std::make_unique<EmptyExtract>());
        else {
            FileOperationRequest Copy; Copy.sources_ = std::move(entries_); Copy.destination_ = target_; Copy.destinationWriter_ = request_.writer_;
            Copy.conflict_ = request_.conflict_; Copy.createParents_ = true; Copy.elevation_ = request_.elevation_; Copy.maxEntries_ = Context.operation_.EnumerationLimit();
            auto Prepared = PrepareFileOperation(std::move(Copy), Context.operation_); if (!Prepared.HasValue()) return failed(Prepared.Failure());
            inner_ = std::move(Prepared.Value().task_);
        }
        listed_ = false; return JobStep{JobState::Running};
    }
    std::vector<Error> Finalize(JobState State) override {
        if (inner_) { auto Errors = inner_->Finalize(State); cleanup_.insert(cleanup_.end(), Errors.begin(), Errors.end()); inner_.reset(); }
        pins_.clear(); cursor_.reset(); reader_.reset(); return cleanup_;
    }
private:
    JobStep failed(Error Error) { JobStep Step{JobState::Failed}; Step.error_ = std::move(Error); return Step; }
    ExtractRequest request_;
    std::vector<std::shared_ptr<IReadStream>> pins_;
    std::shared_ptr<IStorageProvider> reader_;
    std::unique_ptr<IEntryCursor> cursor_;
    std::vector<OperationSource> entries_;
    std::unique_ptr<IJobTask> inner_;
    Locator root_, target_;
    std::vector<Error> cleanup_;
    std::size_t pinned_ = 0, archive_ = 0, itemOffset_ = 0, currentItems_ = 0;
    std::uint64_t selectionOffset_ = 0, bytesOffset_ = 0, currentBytes_ = 0;
    bool listed_ = false;
};
}
Result<PreparedJob> PrepareExtract(ExtractRequest Request, OperationContext Context) {
    if (!Request.writer_ || !Request.readers_ || (Request.archives_.empty() && !Request.selection_)
        || (Request.selection_ && (!Request.selection_->Provider() || !Request.selection_->Count() || !Request.archives_.empty()))
        || (Request.destination_.kind_ != LocatorKind::Local && Request.destination_.kind_ != LocatorKind::Smb)
        || !QDir::isAbsolutePath(QString::fromStdString(Request.destination_.root_))) return Error{ErrorCategory::InvalidRequest, "invalid-extract-request"};
    PreparedJob Out; Out.request_.operation_ = Context; Out.request_.maxItemResults_ = Context.EnumerationLimit();
    Out.request_.metadata_ = {"파일 압축 해제", Request.sourceRoot_.root_, Request.destination_.root_, Request.separateFolders_ ? "extract; separate-folders" : "extract; shared-folder"};
    Out.request_.resourceLanes_ = {FileOperationLane(Request.sourceRoot_), FileOperationLane(Request.destination_)};
    Out.task_ = std::make_unique<ProviderInteractionTask>(std::make_unique<ExtractTask>(std::move(Request)), "압축 내용을 준비·해독하는 중…"); return Out;
}
}
