#include "stdafx.h"
#include "archiveExtractJob.hpp"
#include "commonLibs/operations/providerInteractionTask.hpp"
#include "commonLibs/operations/directoryPreparation.hpp"
#include "commonLibs/plugins/archiveVolumeNames.hpp"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

namespace hc::operations {
using namespace storage;
namespace {
bool CanCheckArchiveSiblings(const Locator& At) {
    if (At.kind_ != LocatorKind::Local && At.kind_ != LocatorKind::Smb) return false;
    const QFileInfo File(QString::fromStdString(At.root_));
    const auto Extension = File.suffix().toLower();
    if (Extension == "zip" || Extension == "zipx") return true;
    // RAR also supports volume names that increment a number in the stem.
    return Extension == "rar" && !hc::plugins::IsArchiveVolumeName(File.fileName())
        && !File.completeBaseName().isEmpty() && !File.completeBaseName().back().isDigit();
}
Locator ArchiveReadScope(const Locator& At) {
    if (At.kind_ != LocatorKind::Local && At.kind_ != LocatorKind::Smb) return At;
    const QFileInfo File(QString::fromStdString(At.root_));
    const auto Extension = File.suffix().toLower();
    if (Extension == "7z" || Extension == "iso") return At;
    return DirectoryPreparation::ParentOf(At);
}
std::optional<QSet<QString>> ArchiveVolumeAnchors(const QDir& Directory, const OperationContext& Context) {
    if (!Directory.isReadable()) return std::nullopt;
    QSet<QString> Anchors;
    QDirIterator Entries(Directory.path(), QDir::Files | QDir::Hidden | QDir::System);
    while (Entries.hasNext()) {
        if (Context.Check({"archive-extract"})) return std::nullopt;
        const auto Volume = hc::plugins::ParseArchiveVolumeName(QFileInfo(Entries.next()).fileName());
        if (!Volume.firstName_.isEmpty()) Anchors.insert(Volume.firstName_.toCaseFolded());
        if (!Volume.alternateName_.isEmpty()) Anchors.insert(Volume.alternateName_.toCaseFolded());
    }
    return Anchors;
}
Result<Locator> ExtractTarget(const ExtractRequest& Request, const EntrySnapshot& Source) {
    auto Target = Request.destination_;
    if (Request.separateFolders_) {
        const auto Name = QFileInfo(QString::fromStdString(Source.name_)).completeBaseName();
        if (Name.isEmpty() || Name == "." || Name == ".." || Name.contains('/') || Name.contains('\\'))
            return Error{ErrorCategory::InvalidRequest, "extract-folder-name"};
        Target.root_ = QDir(QString::fromStdString(Target.root_)).filePath(Name).toStdString();
    }
    return Target;
}
class EmptyExtract final : public IJobTask {
public:
    JobStep Advance(JobStepContext) override { return JobStep{JobState::Completed}; }
    std::vector<Error> Finalize(JobState) override { return {}; }
};
class ExtractTask final : public IJobTask {
public:
    ExtractTask(ExtractRequest Request, bool RefineInputs, bool RefineOutputs)
        : request_(std::move(Request)), refineInputs_(RefineInputs), refineOutputs_(RefineOutputs) {}
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
            if (refineInputs_) {
                const auto& At = Source.entry_.locator_;
                auto Read = ArchiveReadScope(At);
                if (CanCheckArchiveSiblings(At)) {
                    const QFileInfo File(QString::fromStdString(At.root_));
                    const auto Directory = File.dir();
                    auto [Anchors, Inserted] = volumeAnchors_.try_emplace(FileOperationPath(Directory.path().toStdString()));
                    if (Inserted) Anchors->second = ArchiveVolumeAnchors(Directory, Context.operation_);
                    if (Anchors->second && !Anchors->second->contains(File.fileName().toCaseFolded())) Read = At;
                }
                readLanes_.push_back(FileOperationLane(Read));
            }
            if (Source.entry_.hints_.contains("virtualArchiveRoot")) {
                if (!Source.reader_) return failed({ErrorCategory::InvalidRequest, "extract-reader-required"});
                auto Fresh = Source.reader_->Stat(Source.entry_.id_, Context.operation_).get(); if (!Fresh.HasValue()) return failed(Fresh.Failure());
                ++pinned_; return JobStep{JobState::Preparing};
            }
            if (Source.entry_.directory_ || (Source.entry_.locator_.kind_ != LocatorKind::Local && Source.entry_.locator_.kind_ != LocatorKind::Smb) || !Source.reader_)
                return failed({ErrorCategory::Unsupported, "extract-native-archive-file-required"});
            if (request_.readers_(Source.entry_).empty()) return failed({ErrorCategory::Unsupported, "extract-no-configured-plugin: " + Source.entry_.name_});
            auto PinnedContext = Context.operation_; PinnedContext.denyConcurrentMutation_ = true;
            auto Pin = Source.reader_->OpenRead(Source.entry_.id_, PinnedContext).get(); if (!Pin.HasValue()) return failed(Pin.Failure());
            pins_.push_back(std::move(Pin.Value())); ++pinned_; return JobStep{JobState::Preparing};
        }
        if (refineInputs_) {
            refineInputs_ = false;
            volumeAnchors_.clear();
            JobStep Step{JobState::Preparing};
            Step.narrowedReads_ = std::move(readLanes_);
            return Step;
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
            root_ = Source.entry_.hints_.contains("virtualArchiveRoot") ? Source.entry_.locator_ : Locator{LocatorKind::Archive, Source.entry_.locator_.root_, {{"wcx", "/"}}};
            for (const auto& Candidate : Candidates) {
                auto Opened = Candidate->Enumerate(root_, Context.operation_).get();
                if (Opened.HasValue()) { reader_ = Candidate; cursor_ = std::move(Opened.Value()); break; }
                Last = Opened.Failure();
                if (Last.category_ == ErrorCategory::Cancelled || Last.category_ == ErrorCategory::AccessDenied) break;
            }
            if (!reader_) return failed(Last);
            auto Target = ExtractTarget(request_, Source.entry_);
            if (!Target.HasValue()) return failed(Target.Failure());
            target_ = std::move(Target.Value());
            return JobStep{JobState::Preparing};
        }
        if (!listed_) {
            auto Page = cursor_->Next(256, Context.operation_).get(); if (!Page.HasValue()) return failed(Page.Failure());
            if (entries_.size() + Page.Value().entries_.size() > Context.operation_.EnumerationLimit()) return failed({ErrorCategory::BudgetExceeded, "extract-entry-budget"});
            if (Page.Value().entries_.empty() && !Page.Value().end_) return failed({ErrorCategory::Unavailable, "extract-list-no-progress"});
            for (const auto& Entry : Page.Value().entries_)
                if (!Entry.hints_.contains("virtualArchiveRoot")) entries_.push_back({Entry, reader_, {}});
            listed_ = Page.Value().end_; return JobStep{JobState::Preparing};
        }
        JobStep Step{JobState::Running};
        if (entries_.empty()) inner_ = std::make_unique<DirectoryPreparation>(target_, request_.writer_, std::make_unique<EmptyExtract>());
        else {
            FileOperationRequest Copy; Copy.sources_ = std::move(entries_); Copy.destination_ = target_; Copy.destinationWriter_ = request_.writer_;
            Copy.preservePaths_ = request_.preservePaths_; Copy.includePattern_ = request_.includePattern_;
            Copy.conflict_ = request_.conflict_; Copy.createParents_ = true; Copy.elevation_ = request_.elevation_; Copy.maxEntries_ = Context.operation_.EnumerationLimit();
            auto Prepared = PrepareFileOperation(std::move(Copy), Context.operation_); if (!Prepared.HasValue()) return failed(Prepared.Failure());
            if (refineOutputs_) Step.narrowedWrites_ = std::move(Prepared.Value().request_.resourceLanes_);
            inner_ = std::move(Prepared.Value().task_);
        }
        listed_ = false; return Step;
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
    std::vector<std::string> readLanes_;
    std::map<std::string, std::optional<QSet<QString>>> volumeAnchors_;
    std::vector<Error> cleanup_;
    std::size_t pinned_ = 0, archive_ = 0, itemOffset_ = 0, currentItems_ = 0;
    std::uint64_t selectionOffset_ = 0, bytesOffset_ = 0, currentBytes_ = 0;
    bool listed_ = false;
    bool refineInputs_;
    bool refineOutputs_;
};
}
Result<PreparedJob> PrepareExtract(ExtractRequest Request, OperationContext Context) {
    if (!IsValidCopyPattern(Request.includePattern_) || !Request.writer_ || !Request.readers_ || (Request.archives_.empty() && !Request.selection_)
        || (Request.selection_ && (!Request.selection_->Provider() || !Request.selection_->Count() || !Request.archives_.empty()))
        || (Request.destination_.kind_ != LocatorKind::Local && Request.destination_.kind_ != LocatorKind::Smb)
        || !QDir::isAbsolutePath(QString::fromStdString(Request.destination_.root_))) return Error{ErrorCategory::InvalidRequest, "invalid-extract-request"};
    PreparedJob Out; Out.request_.operation_ = Context; Out.request_.maxItemResults_ = Context.EnumerationLimit();
    Out.request_.metadata_ = {"파일 압축 해제", Request.sourceRoot_.root_, Request.destination_.root_, Request.separateFolders_ ? "extract; separate-folders" : "extract; shared-folder"};
    Out.request_.metadata_.kind_ = JobKind::Extract;
    // Unknown selections and formats with companion files need an initial
    // directory claim. Standalone archives only need their own input file.
    const bool Explicit = !Request.selection_ && Request.sourceRoot_.kind_ != LocatorKind::Plugin;
    const bool RefineInputs = Request.sourceRoot_.kind_ != LocatorKind::Plugin
        && (Request.selection_ || std::ranges::any_of(Request.archives_, [](const auto& Source) { return CanCheckArchiveSiblings(Source.entry_.locator_); }));
    if (Explicit) {
        for (const auto& Source : Request.archives_)
            Out.request_.readLanes_.push_back(FileOperationLane(ArchiveReadScope(Source.entry_.locator_)));
    } else {
        auto& Sources = Request.sourceRoot_.kind_ == LocatorKind::Plugin ? Out.request_.resourceLanes_ : Out.request_.readLanes_;
        Sources.push_back(FileOperationLane(Request.sourceRoot_));
    }
    if (Request.separateFolders_ && !Request.selection_) {
        for (const auto& Source : Request.archives_) {
            auto Target = ExtractTarget(Request, Source.entry_);
            if (!Target.HasValue()) return Target.Failure();
            Out.request_.resourceLanes_.push_back(FileOperationLane(Target.Value()));
        }
    } else Out.request_.resourceLanes_.push_back(FileOperationLane(Request.destination_));
    const bool RefineOutputs = Request.sourceRoot_.kind_ != LocatorKind::Plugin
        && (Request.selection_ ? Request.selection_->Count() == 1 : Request.archives_.size() == 1);
    Out.task_ = std::make_unique<ProviderInteractionTask>(std::make_unique<ExtractTask>(std::move(Request), RefineInputs, RefineOutputs), "압축 내용을 준비·해독하는 중…"); return Out;
}
}
