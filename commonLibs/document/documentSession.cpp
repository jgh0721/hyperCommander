#include "documentSession.hpp"
#include "documentHostProtocol.hpp"

namespace hc::document
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"document"}}; }
        bool ValidText(std::string_view Text)
        {
            for (std::size_t At = 0; At < Text.size();) {
                const auto First = static_cast<unsigned char>(Text[At++]);
                if (!First) return false;
                if (First < 0x80) continue;
                const unsigned Count = First >= 0xc2 && First <= 0xdf ? 1 : First >= 0xe0 && First <= 0xef ? 2 : First >= 0xf0 && First <= 0xf4 ? 3 : 0;
                if (!Count || Text.size() - At < Count) return false;
                unsigned Value = First & (Count == 1 ? 0x1f : Count == 2 ? 0x0f : 0x07);
                for (unsigned Index = 0; Index < Count; ++Index) {
                    const auto Byte = static_cast<unsigned char>(Text[At++]);
                    if ((Byte & 0xc0) != 0x80) return false;
                    Value = (Value << 6) | (Byte & 0x3f);
                }
                if (Value < (Count == 1 ? 0x80u : Count == 2 ? 0x800u : 0x10000u) || Value > 0x10ffff || (Value >= 0xd800 && Value <= 0xdfff)) return false;
            }
            return true;
        }
    }
    DocumentSession::DocumentSession(DocumentId Id, SourceIdentity Source, DocumentContentMode Mode, ExternalModelPolicy Policy)
        : modelPolicy_(Policy)
    { snapshot_.id_ = std::move(Id); snapshot_.source_ = std::move(Source); snapshot_.contentMode_ = Mode; }
    DocumentCapabilities DocumentSession::Capabilities() const
    {
        const bool Ready = snapshot_.lifecycle_ == DocumentLifecycle::Ready;
        const bool Busy = IsBusy();
        if (snapshot_.contentMode_ == DocumentContentMode::ExternalModel)
            return {Ready, Ready && modelPolicy_.edit_, Ready,
                Ready && !Busy && modelPolicy_.save_ && snapshot_.source_.originalWritable_,
                Ready && !Busy && modelPolicy_.saveAs_, Ready && modelPolicy_.edit_ && undo_,
                Ready && modelPolicy_.edit_ && redo_, Busy,
                snapshot_.lifecycle_ != DocumentLifecycle::Closed && !Busy, Ready && !Busy && modelPolicy_.print_};
        return {Ready, Ready, Ready, Ready && !Busy && snapshot_.source_.originalWritable_,
            Ready && !Busy, Ready && undo_, Ready && redo_,
            Busy || snapshot_.lifecycle_ == DocumentLifecycle::Opening,
            snapshot_.lifecycle_ != DocumentLifecycle::Closed && !Busy};
    }
    Result<Unit> DocumentSession::Execute(std::string_view Command, std::string Text)
    {
        if (snapshot_.contentMode_ != DocumentContentMode::InlineBytes) return Invalid("document-external-model-command");
        if (Command != "document.setText") return Invalid("document-command-unsupported");
        if (!Capabilities().edit_) return Invalid("document-edit-unavailable");
        if (Text.size() > (binary_ ? DocumentByteLimit : TextByteLimit) || (!binary_ && !ValidText(Text))) return Invalid("document-text-budget-or-nul");
        if (Text != text_) { text_ = std::move(Text); ++snapshot_.revision_; }
        snapshot_.dirty_ = text_ != savedText_;
        return Unit{};
    }
    Result<std::uint64_t> DocumentSession::BeginOpen(bool Discard)
    {
        if (snapshot_.contentMode_ != DocumentContentMode::InlineBytes) return Invalid("document-external-model-load-required");
        if (snapshot_.lifecycle_ == DocumentLifecycle::Closed || saving_) return Invalid("document-open-busy");
        if (snapshot_.dirty_ && !Discard) return Invalid("document-reload-dirty");
        ++snapshot_.generation_; snapshot_.lifecycle_ = DocumentLifecycle::Opening;
        snapshot_.cancelRequested_ = false; snapshot_.error_.reset(); return snapshot_.generation_;
    }
    bool DocumentSession::CompleteOpen(std::uint64_t Generation, Result<OpenedDocument> Result)
    {
        if (snapshot_.contentMode_ != DocumentContentMode::InlineBytes) return false;
        if (Generation != snapshot_.generation_ || snapshot_.lifecycle_ != DocumentLifecycle::Opening) return false;
        if (Result.HasValue() && (Result.Value().text_.size() > (binary_ ? DocumentByteLimit : TextByteLimit) || (!binary_ && !ValidText(Result.Value().text_))))
            Result = Invalid("document-text-budget-or-nul");
        if (!Result.HasValue()) { snapshot_.lifecycle_ = DocumentLifecycle::Failed; snapshot_.error_ = Result.Failure(); return true; }
        snapshot_.source_ = std::move(Result.Value().source_);
        text_ = savedText_ = std::move(Result.Value().text_);
        ++snapshot_.revision_; snapshot_.savedRevision_ = snapshot_.revision_;
        snapshot_.dirty_ = false; snapshot_.error_.reset(); snapshot_.cleanupErrors_.clear();
        snapshot_.saveState_ = DocumentSaveState::Idle; snapshot_.lifecycle_ = DocumentLifecycle::Ready;
        snapshot_.cancelRequested_ = false; undo_ = redo_ = false; return true;
    }
    Result<SaveTicket> DocumentSession::Save(SaveRequest Request)
    {
        if (snapshot_.contentMode_ != DocumentContentMode::InlineBytes) return Invalid("document-export-required");
        const auto Capability = Capabilities();
        if (Request.destination_.empty() || !(Request.disposition_ == SaveDisposition::Original ? Capability.save_ : Capability.saveAs_))
            return Invalid("document-save-unavailable");
        SaveTicket Ticket{snapshot_.id_, snapshot_.generation_, ++nextSave_, snapshot_.revision_,
            text_, std::move(Request.destination_), Request.disposition_, snapshot_.source_};
        saving_ = Ticket; snapshot_.saveState_ = DocumentSaveState::Saving;
        snapshot_.cancelRequested_ = false; snapshot_.error_.reset(); snapshot_.cleanupErrors_.clear(); return Ticket;
    }
    bool DocumentSession::CompleteSave(const SaveTicket& Ticket, DocumentSaveResult Result)
    {
        if (!saving_ || snapshot_.lifecycle_ == DocumentLifecycle::Closed || Ticket.documentId_ != snapshot_.id_
            || Ticket.generation_ != snapshot_.generation_ || Ticket.token_ != saving_->token_) return false;
        // Commit is authoritative even if cancellation or cleanup follows it.
        if (Result.committed_) {
            if (snapshot_.contentMode_ == DocumentContentMode::ExternalModel) snapshot_.savedModel_ = saving_->model_;
            else savedText_ = saving_->text_;
            snapshot_.savedRevision_ = saving_->revision_;
            if (Result.source_) snapshot_.source_ = std::move(*Result.source_);
            if (snapshot_.contentMode_ == DocumentContentMode::ExternalModel) updateModelDirty();
            else snapshot_.dirty_ = text_ != savedText_;
            snapshot_.saveState_ = DocumentSaveState::Idle;
            snapshot_.error_.reset(); snapshot_.cleanupErrors_ = std::move(Result.cleanupErrors_);
        }
        else {
            snapshot_.error_ = Result.error_.value_or(Error{ErrorCategory::Unavailable, "document-not-committed"});
            snapshot_.saveState_ = snapshot_.error_->category_ == ErrorCategory::StaleRevision ? DocumentSaveState::Conflict : DocumentSaveState::Failed;
        }
        saving_.reset(); snapshot_.cancelRequested_ = false; return true;
    }
    void DocumentSession::Cancel()
    {
        if (pending_) { snapshot_.cancelRequested_ = true; return; }
        // Once bytes reached the writer, cancellation remains cooperative: a
        // committed result must still advance the saved content savepoint.
        if (Capabilities().cancel_) snapshot_.cancelRequested_ = true;
    }
    Result<Unit> DocumentSession::Close(CloseDisposition Disposition)
    {
        if (IsBusy())
            return Invalid("document-close-busy");
        if (snapshot_.dirty_ && Disposition != CloseDisposition::Discard) return Invalid("document-close-dirty");
        finishOperation();
        ++snapshot_.generation_; snapshot_.lifecycle_ = DocumentLifecycle::Closed;
        snapshot_.source_.lease_.reset(); return Unit{};
    }
    void DocumentSession::SetHistoryState(bool Undo, bool Redo) { undo_ = Undo; redo_ = Redo; }
}
