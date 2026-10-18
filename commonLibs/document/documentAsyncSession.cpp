#include "documentSession.hpp"
#include "documentRecovery.hpp"
#include "documentHostProtocol.hpp"
#include <limits>

namespace hc::document
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"document"}}; }
        bool ValidModel(const ExternalModelRevision& Model)
        {
            if (!Model.epoch_ || Model.savepoint_.empty() || Model.savepoint_.size() > 128) return false;
            for (const unsigned char Ch : Model.savepoint_) if (Ch < 0x21 || Ch > 0x7e) return false;
            return true;
        }
        bool LowerHex(std::string_view Value, std::size_t Size)
        {
            return Value.size() == Size && std::all_of(Value.begin(), Value.end(),
                [](unsigned char Ch) { return (Ch >= '0' && Ch <= '9') || (Ch >= 'a' && Ch <= 'f'); });
        }
        bool CanAdvance(std::uint64_t Value) { return Value != std::numeric_limits<std::uint64_t>::max(); }
    }
    Result<Unit> DocumentSession::BindSourceAuthority(SourceAuthorityBinding Binding)
    {
        if (snapshot_.contentMode_ != DocumentContentMode::ExternalModel
            || snapshot_.lifecycle_ != DocumentLifecycle::Ready || IsBusy() || snapshot_.dirty_
            || snapshot_.cancelRequested_ || !snapshot_.source_.sourceToken_.empty()
            || snapshot_.source_.originalWritable_)
            return Invalid("document-authority-unavailable");
        if (Binding.documentId_ != snapshot_.id_ || Binding.generation_ != snapshot_.generation_
            || Binding.revision_ != snapshot_.revision_ || Binding.model_ != snapshot_.model_)
            return Error{ErrorCategory::StaleRevision, "document-authority-stale"};
        if (!LowerHex(snapshot_.source_.inputSha256_, 64)
            || Binding.inputSha256_ != snapshot_.source_.inputSha256_)
            return Error{ErrorCategory::StaleRevision, "document-authority-input-mismatch"};
        if (!LowerHex(Binding.sourceToken_, 32) || Binding.origin_.directory_
            || Binding.origin_.id_.providerId_.value_.empty() || Binding.origin_.id_.key_.empty()
            || !Binding.origin_.id_.revision_ || Binding.origin_.id_.revision_->token_.empty())
            return Invalid("document-authority-invalid");
        // The original may be an archive/provider entry while localPath/lease
        // names only its temporary materialization. Never promote that temp path
        // to an original file or widen the external-model feature policy.
        snapshot_.source_.entry_ = std::move(Binding.origin_);
        snapshot_.source_.sourceToken_ = std::move(Binding.sourceToken_);
        snapshot_.source_.originalWritable_ = Binding.originalWritable_;
        return Unit{};
    }
    bool DocumentSession::matches(const DocumentOperationTicket& Ticket, DocumentOperation Operation) const
    {
        return snapshot_.contentMode_ == DocumentContentMode::ExternalModel && pending_
            && Ticket == pending_->ticket_ && Ticket.operation_ == Operation
            && Ticket.documentId_ == snapshot_.id_ && Ticket.generation_ == snapshot_.generation_
            && snapshot_.lifecycle_ != DocumentLifecycle::Closed;
    }
    void DocumentSession::finishOperation()
    {
        pending_.reset(); snapshot_.operation_ = DocumentOperation::Idle;
        snapshot_.cancelRequested_ = false;
    }
    bool DocumentSession::Cancel(const DocumentOperationTicket& Ticket)
    {
        if (!matches(Ticket, Ticket.operation_)) return false;
        Cancel(); return true;
    }
    bool DocumentSession::Cancel(const SaveTicket& Ticket)
    {
        if (!saving_ || snapshot_.lifecycle_ == DocumentLifecycle::Closed
            || Ticket.documentId_ != snapshot_.id_ || Ticket.generation_ != snapshot_.generation_
            || Ticket.token_ != saving_->token_ || Ticket.revision_ != saving_->revision_
            || Ticket.model_ != saving_->model_ || Ticket.contentMode_ != saving_->contentMode_
            || Ticket.destination_ != saving_->destination_ || Ticket.disposition_ != saving_->disposition_
            || Ticket.source_.lease_ != saving_->source_.lease_) return false;
        Cancel(); return true;
    }
    bool DocumentSession::CompleteCancellation(const DocumentOperationTicket& Ticket)
    {
        if (!snapshot_.cancelRequested_ || !matches(Ticket, Ticket.operation_)) return false;
        const bool Loading = Ticket.operation_ == DocumentOperation::Loading;
        finishOperation(); snapshot_.saveState_ = DocumentSaveState::Idle;
        snapshot_.error_ = Error{ErrorCategory::Cancelled, "document-operation-cancelled"};
        if (Loading) snapshot_.lifecycle_ = DocumentLifecycle::Failed;
        return true;
    }
    void DocumentSession::updateModelDirty()
    {
        // Native revision is monotonic even through undo. Content identity, not
        // revision equality or a backend 'dirty=false' flag, defines cleanliness.
        snapshot_.dirty_ = snapshot_.model_.epoch_ != snapshot_.savedModel_.epoch_
            || snapshot_.model_.savepoint_ != snapshot_.savedModel_.savepoint_;
    }
    Result<DocumentOperationTicket> DocumentSession::BeginLoad(bool Discard)
    {
        if (snapshot_.contentMode_ != DocumentContentMode::ExternalModel || IsBusy()
            || snapshot_.lifecycle_ == DocumentLifecycle::Closed) return Invalid("document-load-unavailable");
        if (snapshot_.dirty_ && !Discard) return Invalid("document-reload-dirty");
        if (!CanAdvance(snapshot_.generation_) || !CanAdvance(snapshot_.revision_) || !CanAdvance(nextOperation_))
            return Invalid("document-counter-exhausted");
        ++snapshot_.generation_; snapshot_.lifecycle_ = DocumentLifecycle::Opening;
        snapshot_.operation_ = DocumentOperation::Loading; snapshot_.cancelRequested_ = false;
        snapshot_.error_.reset(); snapshot_.saveState_ = DocumentSaveState::Idle;
        DocumentOperationTicket Ticket{snapshot_.id_, snapshot_.generation_, ++nextOperation_,
            snapshot_.revision_, DocumentOperation::Loading, {}, snapshot_.source_.lease_};
        pending_ = PendingOperation{Ticket, {}, snapshot_.source_};
        return Ticket;
    }
    bool DocumentSession::CompleteLoad(const DocumentOperationTicket& Ticket, Result<LoadedExternalModel> Result)
    {
        if (!matches(Ticket, DocumentOperation::Loading) || snapshot_.lifecycle_ != DocumentLifecycle::Opening) return false;
        if (snapshot_.cancelRequested_) { CompleteCancellation(Ticket); return true; }
        if (Result.HasValue() && !ValidModel(Result.Value().model_)) Result = Invalid("document-model-identity");
        finishOperation();
        if (!Result.HasValue()) {
            snapshot_.lifecycle_ = DocumentLifecycle::Failed; snapshot_.error_ = Result.Failure(); return true;
        }
        snapshot_.source_ = std::move(Result.Value().source_);
        snapshot_.model_ = snapshot_.savedModel_ = std::move(Result.Value().model_);
        ++snapshot_.revision_; snapshot_.savedRevision_ = snapshot_.revision_;
        snapshot_.dirty_ = false; snapshot_.error_.reset(); snapshot_.cleanupErrors_.clear();
        snapshot_.lifecycle_ = DocumentLifecycle::Ready; undo_ = redo_ = false;
        // Rich model content never enters the inline byte/hidden editor path.
        text_.clear(); savedText_.clear(); return true;
    }
    bool DocumentSession::CompleteRestore(const DocumentOperationTicket& Ticket, LoadedExternalModel Loaded,
        const RecoverySnapshot& Recovery, const std::string& BundleSha256, const std::string& Format)
    {
        if (!matches(Ticket, DocumentOperation::Loading) || snapshot_.lifecycle_ != DocumentLifecycle::Opening) return false;
        if (snapshot_.cancelRequested_) { CompleteCancellation(Ticket); return true; }
        const auto& Saved = Recovery.document_;
        const bool Dirty = Saved.model_.epoch_ != Saved.savedModel_.epoch_ || Saved.model_.savepoint_ != Saved.savedModel_.savepoint_;
        if (Saved.id_ != snapshot_.id_ || Saved.contentMode_ != DocumentContentMode::ExternalModel
            || !Saved.generation_ || !Saved.revision_ || Saved.savedRevision_ > Saved.revision_
            || !ValidModel(Saved.model_) || !ValidModel(Saved.savedModel_) || Saved.dirty_ != Dirty
            || Saved.model_.epoch_ != Saved.savedModel_.epoch_
            || !ValidModel(Loaded.model_) || Loaded.model_.savepoint_ != Saved.model_.savepoint_
            || !LowerHex(BundleSha256, 64) || Recovery.bundleSha256_ != BundleSha256
            || Recovery.engineSchema_ != RecoveryEngineSchema || Recovery.format_ != Format
            || (Format != "hwp" && Format != "hwpx") || Recovery.bytes_.empty() || Recovery.bytes_.size() > DocumentByteLimit
            || Loaded.source_.originalWritable_ || !Loaded.source_.sourceToken_.empty()
            || Loaded.source_.lease_ != Ticket.lease_ || !CanAdvance(Saved.revision_) || !CanAdvance(snapshot_.revision_))
            return CompleteLoad(Ticket, Invalid("document-recovery-incompatible"));
        // Snapshot bytes and the new parsed model have been validated by native
        // admission; restore never uses CompleteLoad's clean-baseline behavior.
        finishOperation(); snapshot_.source_ = std::move(Loaded.source_);
        snapshot_.model_ = std::move(Loaded.model_); snapshot_.savedModel_ = Saved.savedModel_;
        // Savepoints describe content. Translate the saved baseline into the new
        // backend epoch without treating recovered edits as a successful save.
        snapshot_.savedModel_.epoch_ = snapshot_.model_.epoch_; snapshot_.savedModel_.changeSequence_ = 0;
        snapshot_.revision_ = std::max(snapshot_.revision_, Saved.revision_) + 1;
        snapshot_.savedRevision_ = Saved.savedRevision_; updateModelDirty();
        snapshot_.lifecycle_ = DocumentLifecycle::Ready; snapshot_.saveState_ = DocumentSaveState::Idle;
        snapshot_.error_.reset(); snapshot_.cleanupErrors_.clear(); undo_ = redo_ = false;
        text_.clear(); savedText_.clear(); return true;
    }
    bool DocumentSession::ApplyModelChange(const DocumentId& Id, std::uint64_t Generation,
        ExternalModelRevision Model, bool Undo, bool Redo)
    {
        if (Id != snapshot_.id_ || Generation != snapshot_.generation_
            || snapshot_.contentMode_ != DocumentContentMode::ExternalModel
            || snapshot_.lifecycle_ != DocumentLifecycle::Ready || !modelPolicy_.edit_
            || !ValidModel(Model) || Model.epoch_ != snapshot_.model_.epoch_
            || Model.changeSequence_ <= snapshot_.model_.changeSequence_ || !CanAdvance(snapshot_.revision_)) return false;
        snapshot_.model_ = std::move(Model); ++snapshot_.revision_;
        undo_ = Undo; redo_ = Redo; updateModelDirty(); return true;
    }
    Result<DocumentOperationTicket> DocumentSession::beginModelOperation(DocumentOperation Operation)
    {
        if (snapshot_.contentMode_ != DocumentContentMode::ExternalModel
            || snapshot_.lifecycle_ != DocumentLifecycle::Ready || IsBusy()) return Invalid("document-operation-unavailable");
        if (!CanAdvance(nextOperation_)) return Invalid("document-counter-exhausted");
        DocumentOperationTicket Ticket{snapshot_.id_, snapshot_.generation_, ++nextOperation_,
            snapshot_.revision_, Operation, snapshot_.model_, snapshot_.source_.lease_};
        pending_ = PendingOperation{Ticket, {}, snapshot_.source_}; snapshot_.operation_ = Operation;
        snapshot_.cancelRequested_ = false; snapshot_.error_.reset(); return Ticket;
    }
    Result<DocumentOperationTicket> DocumentSession::BeginExport(SaveRequest Request)
    {
        if (Request.disposition_ != SaveDisposition::Original && Request.disposition_ != SaveDisposition::SaveAs)
            return Invalid("document-save-disposition");
        const auto Capability = Capabilities();
        if (Request.destination_.empty() || !(Request.disposition_ == SaveDisposition::Original ? Capability.save_ : Capability.saveAs_))
            return Invalid("document-save-unavailable");
        if (!CanAdvance(nextSave_)) return Invalid("document-counter-exhausted");
        auto Ticket = beginModelOperation(DocumentOperation::Exporting);
        if (Ticket.HasValue()) {
            pending_->save_ = std::move(Request); snapshot_.saveState_ = DocumentSaveState::Exporting;
            snapshot_.cleanupErrors_.clear();
        }
        return Ticket;
    }
    Result<SaveTicket> DocumentSession::CompleteExport(const DocumentOperationTicket& Ticket, Result<ExternalModelPayload> Result)
    {
        if (!matches(Ticket, DocumentOperation::Exporting)) return Invalid("document-export-stale");
        if (snapshot_.cancelRequested_) { CompleteCancellation(Ticket); return *snapshot_.error_; }
        if (Result.HasValue() && modelPolicy_.exportRevision_ == ExportRevisionPolicy::StrictCurrentRevision
            && snapshot_.revision_ != pending_->ticket_.revision_)
            Result = Error{ErrorCategory::StaleRevision, "document-export-revision-changed"};
        if (Result.HasValue() && (Result.Value().model_ != pending_->ticket_.model_
            || Result.Value().bytes_.empty() || Result.Value().bytes_.size() > DocumentByteLimit))
            Result = Invalid("document-export-payload");
        if (!Result.HasValue()) {
            snapshot_.error_ = Result.Failure();
            snapshot_.saveState_ = Result.Failure().category_ == ErrorCategory::StaleRevision
                ? DocumentSaveState::Conflict : DocumentSaveState::Failed;
            finishOperation(); return Result.Failure();
        }
        // The stored ticket owns destination, source lease and captured revision.
        // A newer edit is allowed; successful commit saves only this snapshot.
        SaveTicket Save{snapshot_.id_, Ticket.generation_, ++nextSave_, pending_->ticket_.revision_,
            std::move(Result.Value().bytes_), pending_->save_.destination_, pending_->save_.disposition_,
            pending_->source_, DocumentContentMode::ExternalModel, pending_->ticket_.model_};
        finishOperation(); saving_ = Save; snapshot_.saveState_ = DocumentSaveState::Saving;
        return Save;
    }
    Result<DocumentOperationTicket> DocumentSession::BeginPrint()
    {
        if (!Capabilities().print_) return Invalid("document-print-unavailable");
        return beginModelOperation(DocumentOperation::Printing);
    }
    bool DocumentSession::CompletePrint(const DocumentOperationTicket& Ticket, Result<Unit> Result)
    {
        if (!matches(Ticket, DocumentOperation::Printing)) return false;
        if (snapshot_.cancelRequested_) { CompleteCancellation(Ticket); return true; }
        finishOperation();
        if (!Result.HasValue()) snapshot_.error_ = Result.Failure();
        // Printing never changes savedRevision, savedModel or dirty state.
        return true;
    }
    Result<DocumentOperationTicket> DocumentSession::BeginCheckpoint()
    {
        if (!modelPolicy_.checkpoint_) return Invalid("document-checkpoint-unavailable");
        return beginModelOperation(DocumentOperation::Checkpointing);
    }
    Result<DocumentCheckpoint> DocumentSession::CompleteCheckpoint(const DocumentOperationTicket& Ticket, Result<ExternalModelPayload> Result)
    {
        if (!matches(Ticket, DocumentOperation::Checkpointing)) return Invalid("document-checkpoint-stale");
        if (snapshot_.cancelRequested_) { CompleteCancellation(Ticket); return *snapshot_.error_; }
        if (Result.HasValue() && (Result.Value().model_ != pending_->ticket_.model_
            || Result.Value().bytes_.empty() || Result.Value().bytes_.size() > DocumentByteLimit))
            Result = Invalid("document-checkpoint-payload");
        if (!Result.HasValue()) {
            snapshot_.error_ = Result.Failure(); finishOperation(); return Result.Failure();
        }
        DocumentCheckpoint Checkpoint{pending_->ticket_, pending_->source_, std::move(Result.Value().bytes_)};
        // Recovery storage has its own completion/lifetime. A checkpoint never
        // advances the document's savepoint or grants original write authority.
        finishOperation(); return Checkpoint;
    }
}
