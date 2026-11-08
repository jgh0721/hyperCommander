#pragma once
#include "wcxMutation.hpp"
#include "storageProviders.hpp"
#include "commonLibs/storage/materializedContent.hpp"
#include "commonLibs/operations/archiveOperation.hpp"

namespace hc::storage
{
    // Captures the navigation snapshot, not a freshly resolved destination.
    // The scheduler must outlive the document session that owns this callback.
    inline ProviderContentSource::ArchiveWriteback MakeArchivePreviewWriteback(
        Locator Root, EntrySnapshot Entry, std::shared_ptr<IStorageProvider> Reader,
        std::shared_ptr<hc::plugins::IArchiveBackend> Backend, std::string Plugin,
        hc::operations::JobScheduler& Scheduler, std::function<void()> Committed = {})
    {
        return [Root, Entry, Reader, Backend, Plugin, &Scheduler, Committed](const std::string& Path, OperationContext Context) -> Result<Unit> {
            if (!Reader || Root.nesting_.empty() || Entry.locator_.nesting_.empty())
                return Error{ErrorCategory::InvalidRequest, "archive-preview-context"};
            auto Verified = Reader->Stat(Entry.id_, Context).get(); if (!Verified.HasValue()) return Verified.Failure();
            auto Native = MakeNativeReadProvider(LocatorKind::Local);
            auto Draft = Native->Resolve({LocatorKind::Local, Path}, Context).get(); if (!Draft.HasValue()) return Draft.Failure();
            if (Draft.Value().name_ != Entry.name_) return Error{ErrorCategory::InvalidRequest, "archive-preview-draft-name"};
            hc::operations::ArchiveOperationRequest Request;
            Request.archive_ = Root; Request.destination_ = Entry.locator_;
            auto& Internal = Request.destination_.nesting_.back().entry_;
            const auto Slash = Internal.find_last_of('/'); Internal = Slash == std::string::npos || Slash == 0 ? "/" : Internal.substr(0, Slash);
            Request.reader_ = Reader; Request.sources_ = {{Draft.Value(), Native, {}}};
            Request.conflict_ = hc::operations::ConflictPolicy::Replace;
            Request.begin_ = [Plugin, Root, Reader, Backend](OperationContext Calls) {
                return BeginWcxMutation(Plugin, Root, Reader, {}, {}, std::move(Calls), {}, Backend);
            };
            auto Job = hc::operations::SubmitArchiveOperation(Scheduler, std::move(Request), Context);
            if (!Job.HasValue()) return Job.Failure();
            const auto Result = Job.Value().completion_.get();
            if (Result.snapshot_.state_ != hc::operations::JobState::Completed || Result.snapshot_.failedItems_ || Result.snapshot_.skippedItems_)
                return Result.snapshot_.error_.value_or(Error{ErrorCategory::Unavailable, "archive-preview-update-failed"});
            if (Committed) Committed();
            return Unit{};
        };
    }
}