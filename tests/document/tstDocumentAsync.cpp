#include "commonLibs/document/documentSession.hpp"
#include "commonLibs/document/documentRecovery.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include <iostream>
#include <stdexcept>
#include <limits>

using namespace hc::document;
using namespace hc::storage;
namespace
{
#define REQUIRE(Condition) do { if (!(Condition)) throw std::runtime_error(#Condition); } while (false)
    constexpr ExternalModelPolicy Writable{true, true, true, true, ExportRevisionPolicy::ImmutableSnapshot};
    SourceIdentity Source()
    {
        SourceIdentity Value; Value.localPath_ = "source.hwp"; Value.originalWritable_ = true; return Value;
    }
    ExternalModelRevision Model(std::uint64_t Sequence, std::string Savepoint, std::uint64_t Epoch = 7)
    { return {Epoch, Sequence, std::move(Savepoint)}; }
    void Load(DocumentSession& Session, SourceIdentity Origin = Source())
    {
        const auto Ticket = Session.BeginLoad(); REQUIRE(Ticket.HasValue());
        REQUIRE(Session.IsBusy()); REQUIRE(!Session.Capabilities().view_);
        REQUIRE(Session.CompleteLoad(Ticket.Value(), LoadedExternalModel{std::move(Origin), Model(0, "base")}));
    }
    void Change(DocumentSession& Session, std::uint64_t Sequence, const char* Savepoint)
    { REQUIRE(Session.ApplyModelChange(Session.Snapshot().id_, Session.Snapshot().generation_, Model(Sequence, Savepoint), true, false)); }
    ExternalModelPayload Payload(const DocumentOperationTicket& Ticket)
    { return {Ticket.model_, std::string("HWP\0snapshot", 12)}; }
    void ReadOnlyDefault()
    {
        DocumentSession Session("read-only", {}, DocumentContentMode::ExternalModel);
        REQUIRE(!Session.CompleteOpen(1, OpenedDocument{Source(), "wrong"}));
        REQUIRE(!Session.BeginOpen().HasValue()); Load(Session);
        REQUIRE(Session.Capabilities().view_); REQUIRE(!Session.Capabilities().edit_);
        REQUIRE(!Session.Capabilities().save_); REQUIRE(!Session.Capabilities().saveAs_);
        REQUIRE(!Session.Save({SaveDisposition::SaveAs, "copy.hwp"}).HasValue());
        REQUIRE(!Session.BeginExport({SaveDisposition::SaveAs, "copy.hwp"}).HasValue());
        REQUIRE(!Session.BeginCheckpoint().HasValue());
        REQUIRE(!Session.Execute("document.setText", "fake").HasValue());
        REQUIRE(!Session.ApplyModelChange("read-only", Session.Snapshot().generation_, Model(1, "changed"), true, false));
        REQUIRE(Session.Text().empty()); REQUIRE(!Session.Snapshot().dirty_);
    }
    void LoadCancellationAndLateCompletion()
    {
        DocumentSession Session("load", {}, DocumentContentMode::ExternalModel, Writable);
        auto Old = Session.BeginLoad().Value(); auto Wrong = Old; Wrong.documentId_ = "other";
        REQUIRE(!Session.CompleteLoad(Wrong, LoadedExternalModel{Source(), Model(0, "base")}));
        REQUIRE(Session.IsBusy()); Session.Cancel(); REQUIRE(Session.IsBusy());
        REQUIRE(!Session.BeginLoad().HasValue()); REQUIRE(Session.CompleteCancellation(Old)); REQUIRE(!Session.IsBusy());
        REQUIRE(Session.Snapshot().error_->category_ == ErrorCategory::Cancelled);
        const auto Current = Session.BeginLoad().Value(); REQUIRE(Current.generation_ != Old.generation_);
        REQUIRE(!Session.CompleteLoad(Old, LoadedExternalModel{Source(), Model(0, "late")}));
        REQUIRE(!Session.Close(CloseDisposition::Discard).HasValue());
        Session.Cancel(); REQUIRE(Session.CompleteCancellation(Current));
        REQUIRE(Session.Close(CloseDisposition::Discard).HasValue());
        REQUIRE(!Session.CompleteLoad(Current, LoadedExternalModel{Source(), Model(0, "late")}));
        REQUIRE(!Session.BeginLoad().HasValue());
    }
    void InvalidLoadAndRetry()
    {
        DocumentSession Session("invalid", {}, DocumentContentMode::ExternalModel, Writable);
        auto Ticket = Session.BeginLoad().Value();
        REQUIRE(Session.CompleteLoad(Ticket, LoadedExternalModel{Source(), Model(0, "base", 0)}));
        REQUIRE(Session.Snapshot().lifecycle_ == DocumentLifecycle::Failed); REQUIRE(!Session.IsBusy());
        Load(Session); REQUIRE(Session.Snapshot().lifecycle_ == DocumentLifecycle::Ready);
    }
    void UndoSavepointAndEventOrdering()
    {
        DocumentSession Session("undo", {}, DocumentContentMode::ExternalModel, Writable); Load(Session);
        const auto OriginalRevision = Session.Snapshot().revision_; Change(Session, 1, "edited");
        REQUIRE(Session.Snapshot().dirty_); Change(Session, 2, "base");
        REQUIRE(!Session.Snapshot().dirty_); REQUIRE(Session.Snapshot().revision_ > OriginalRevision);
        auto State = Session.Snapshot();
        REQUIRE(!Session.ApplyModelChange("other", State.generation_, Model(3, "bad"), false, false));
        REQUIRE(!Session.ApplyModelChange(State.id_, State.generation_ - 1, Model(3, "bad"), false, false));
        REQUIRE(!Session.ApplyModelChange(State.id_, State.generation_, Model(3, "bad", 8), false, false));
        REQUIRE(!Session.ApplyModelChange(State.id_, State.generation_, Model(2, "bad"), false, false));
        REQUIRE(!Session.ApplyModelChange(State.id_, State.generation_, Model(1, "bad"), false, false));
        REQUIRE(Session.Snapshot().revision_ == State.revision_); REQUIRE(!Session.Snapshot().dirty_);
        Change(Session, std::numeric_limits<std::uint64_t>::max(), "last");
        REQUIRE(!Session.ApplyModelChange(State.id_, State.generation_, Model(0, "base"), false, false));
    }
    void ExportAndEditDuringCommit()
    {
        DocumentSession Session("save", {}, DocumentContentMode::ExternalModel, Writable); Load(Session);
        Change(Session, 1, "first"); const auto Export = Session.BeginExport({SaveDisposition::Original, "source.hwp"}).Value();
        REQUIRE(Session.Snapshot().operation_ == DocumentOperation::Exporting);
        REQUIRE(!Session.Save({SaveDisposition::Original, "source.hwp"}).HasValue());
        REQUIRE(!Session.Close(CloseDisposition::Discard).HasValue());
        REQUIRE(!Session.BeginCheckpoint().HasValue()); REQUIRE(!Session.BeginLoad(true).HasValue());
        Change(Session, 2, "second"); auto Save = Session.CompleteExport(Export, Payload(Export)); REQUIRE(Save.HasValue());
        REQUIRE(Save.Value().revision_ == Export.revision_); REQUIRE(Save.Value().model_ == Export.model_);
        REQUIRE(Save.Value().text_ == Payload(Export).bytes_); REQUIRE(Session.Text().empty());
        REQUIRE(!Session.CompleteExport(Export, Payload(Export)).HasValue());
        Session.Cancel(); REQUIRE(Session.Snapshot().cancelRequested_);
        Change(Session, 3, "third"); REQUIRE(Session.CompleteSave(Save.Value(), {true}));
        REQUIRE(Session.Snapshot().dirty_); REQUIRE(Session.Snapshot().savedRevision_ == Export.revision_);
        REQUIRE(!Session.IsBusy()); Change(Session, 4, "first"); REQUIRE(!Session.Snapshot().dirty_);
        REQUIRE(!Session.CompleteSave(Save.Value(), {true}));
    }
    void ExportFailureRetryAndTampering()
    {
        DocumentSession Session("failure", {}, DocumentContentMode::ExternalModel, Writable); Load(Session); Change(Session, 1, "edited");
        auto Ticket = Session.BeginExport({SaveDisposition::SaveAs, "copy.hwp"}).Value();
        auto Wrong = Ticket; ++Wrong.revision_;
        REQUIRE(!Session.CompleteExport(Wrong, Payload(Ticket)).HasValue()); REQUIRE(Session.IsBusy());
        auto Bytes = Payload(Ticket); Bytes.model_.savepoint_ = "forged";
        REQUIRE(!Session.CompleteExport(Ticket, Bytes).HasValue()); REQUIRE(!Session.IsBusy());
        REQUIRE(Session.Snapshot().dirty_); REQUIRE(Session.Snapshot().saveState_ == DocumentSaveState::Failed);
        Ticket = Session.BeginExport({SaveDisposition::SaveAs, "copy.hwp"}).Value();
        REQUIRE(!Session.CompleteExport(Ticket, Error{ErrorCategory::Unsupported, "content-loss"}).HasValue());
        REQUIRE(Session.Snapshot().error_->code_ == "content-loss"); REQUIRE(Session.Snapshot().dirty_);
        Ticket = Session.BeginExport({SaveDisposition::SaveAs, "copy.hwp"}).Value();
        auto Save = Session.CompleteExport(Ticket, Payload(Ticket)).Value();
        REQUIRE(Session.CompleteSave(Save, {false, {}, Error{ErrorCategory::StaleRevision, "conflict"}}));
        REQUIRE(Session.Snapshot().saveState_ == DocumentSaveState::Conflict); REQUIRE(Session.Snapshot().dirty_);
        REQUIRE(Session.Capabilities().saveAs_);
    }
    void CancellationDoesNotConsumeNewRequest()
    {
        DocumentSession Session("cancel", {}, DocumentContentMode::ExternalModel, Writable); Load(Session); Change(Session, 1, "edited");
        auto Old = Session.BeginExport({SaveDisposition::SaveAs, "old.hwp"}).Value();
        Session.Cancel(); REQUIRE(Session.IsBusy()); REQUIRE(Session.Snapshot().dirty_);
        REQUIRE(!Session.BeginExport({SaveDisposition::SaveAs, "new.hwp"}).HasValue());
        REQUIRE(Session.CompleteCancellation(Old)); REQUIRE(!Session.IsBusy());
        const auto New = Session.BeginExport({SaveDisposition::SaveAs, "new.hwp"}).Value();
        REQUIRE(New.token_ != Old.token_); REQUIRE(!Session.CompleteExport(Old, Payload(Old)).HasValue());
        REQUIRE(Session.IsBusy()); auto Save = Session.CompleteExport(New, Payload(New)).Value();
        REQUIRE(Save.destination_ == "new.hwp");
        auto Fake = Save; Fake.text_ = "forged"; Fake.model_ = Model(99, "forged");
        REQUIRE(Session.CompleteSave(Fake, {true})); REQUIRE(!Session.Snapshot().dirty_);
        REQUIRE(Session.Snapshot().savedModel_ == New.model_);
    }
    void CheckpointDoesNotSaveDocument()
    {
        DocumentSession Session("checkpoint", {}, DocumentContentMode::ExternalModel, Writable); Load(Session); Change(Session, 1, "edited");
        const auto Before = Session.Snapshot(); const auto Ticket = Session.BeginCheckpoint().Value();
        REQUIRE(Session.Snapshot().operation_ == DocumentOperation::Checkpointing);
        REQUIRE(!Session.BeginExport({SaveDisposition::SaveAs, "copy.hwp"}).HasValue());
        Change(Session, 2, "newer"); auto Completed = Session.CompleteCheckpoint(Ticket, Payload(Ticket)); REQUIRE(Completed.HasValue());
        REQUIRE(Completed.Value().ticket_ == Ticket); REQUIRE(Completed.Value().bytes_ == Payload(Ticket).bytes_);
        REQUIRE(Session.Snapshot().savedRevision_ == Before.savedRevision_); REQUIRE(Session.Snapshot().dirty_);
        REQUIRE(!Session.IsBusy()); REQUIRE(!Session.CompleteCheckpoint(Ticket, Payload(Ticket)).HasValue());
        const auto Cancelled = Session.BeginCheckpoint().Value(); Session.Cancel();
        REQUIRE(!Session.CompleteCheckpoint(Cancelled, Payload(Cancelled)).HasValue());
    }
    void RendererRestartInvalidatesOldEpoch()
    {
        DocumentSession Session("restart", {}, DocumentContentMode::ExternalModel, Writable); Load(Session); Change(Session, 1, "edited");
        const auto Old = Session.BeginCheckpoint().Value(); Session.Cancel(); REQUIRE(Session.CompleteCancellation(Old));
        REQUIRE(!Session.BeginLoad().HasValue()); auto New = Session.BeginLoad(true).Value();
        REQUIRE(Session.CompleteLoad(New, LoadedExternalModel{Source(), Model(0, "base", 8)}));
        REQUIRE(!Session.CompleteCheckpoint(Old, Payload(Old)).HasValue());
        REQUIRE(!Session.ApplyModelChange("restart", New.generation_, Model(2, "edited", 7), true, false));
        REQUIRE(Session.Snapshot().model_.epoch_ == 8); REQUIRE(!Session.Snapshot().dirty_);
    }
    void PayloadBudgetsAndLeaseLifetime()
    {
        struct Lease final : IMaterializedFileLease
        {
            std::string LocalPath() const override { return "source.hwp"; }
            EntrySnapshot Describe() const override { return {}; }
            std::size_t Depth() const override { return 1; }
        };
        auto Shared = std::make_shared<Lease>(); std::weak_ptr<Lease> Weak = Shared;
        auto Origin = Source(); Origin.lease_ = Shared; Shared.reset();
        DocumentSession Session("lease", {}, DocumentContentMode::ExternalModel, Writable); Load(Session, Origin); Origin.lease_.reset();
        auto Ticket = Session.BeginCheckpoint().Value();
        REQUIRE(!Session.CompleteCheckpoint(Ticket, ExternalModelPayload{Ticket.model_, {}}).HasValue());
        Ticket = Session.BeginExport({SaveDisposition::SaveAs, "copy.hwp"}).Value();
        REQUIRE(!Session.CompleteExport(Ticket, ExternalModelPayload{Ticket.model_, std::string(DocumentByteLimit + 1, 'x')}).HasValue());
        Ticket = Session.BeginCheckpoint().Value(); auto Checkpoint = Session.CompleteCheckpoint(Ticket, Payload(Ticket)).Value();
        REQUIRE(Session.Close(CloseDisposition::KeepChanges).HasValue()); REQUIRE(!Weak.expired());
        Checkpoint.source_.lease_.reset(); REQUIRE(!Weak.expired());
        Checkpoint.ticket_.lease_.reset(); REQUIRE(!Weak.expired()); Ticket.lease_.reset(); REQUIRE(Weak.expired());
    }
    void StrictExportAdmission()
    {
        DocumentSession Session("strict", {}, DocumentContentMode::ExternalModel, {true, true, true, false}); Load(Session);
        for (const auto Kind : {SaveDisposition::Original, SaveDisposition::SaveAs}) {
            const auto Ticket = Session.BeginExport({Kind, "copy.hwp"}).Value();
            Change(Session, Session.Snapshot().model_.changeSequence_ + 1, "newer");
            const auto Result = Session.CompleteExport(Ticket, Payload(Ticket));
            REQUIRE(!Result.HasValue()); REQUIRE(Result.Failure().category_ == ErrorCategory::StaleRevision);
            REQUIRE(Session.Snapshot().dirty_); REQUIRE(!Session.IsBusy());
        }
        REQUIRE(!Session.BeginExport({static_cast<SaveDisposition>(99), "copy.hwp"}).HasValue());
        const auto Ticket = Session.BeginExport({SaveDisposition::Original, "copy.hwp"}).Value();
        const auto Save = Session.CompleteExport(Ticket, Payload(Ticket)).Value();
        REQUIRE(Session.CompleteSave(Save, {true})); REQUIRE(!Session.Snapshot().dirty_);
    }
    void CancelledTerminalResultAndWrongAck()
    {
        DocumentSession Session("terminal", {}, DocumentContentMode::ExternalModel, Writable); Load(Session);
        const auto Ticket = Session.BeginExport({SaveDisposition::SaveAs, "copy.hwp"}).Value();
        REQUIRE(!Session.CompleteCancellation(Ticket)); Session.Cancel();
        auto Wrong = Ticket; ++Wrong.token_; REQUIRE(!Session.CompleteCancellation(Wrong)); REQUIRE(Session.IsBusy());
        const auto Result = Session.CompleteExport(Ticket, Payload(Ticket));
        REQUIRE(!Result.HasValue()); REQUIRE(Result.Failure().category_ == ErrorCategory::Cancelled); REQUIRE(!Session.IsBusy());
        const auto Loading = Session.BeginLoad().Value(); Session.Cancel();
        REQUIRE(Session.CompleteLoad(Loading, LoadedExternalModel{Source(), Model(0, "ignored")}));
        REQUIRE(Session.Snapshot().lifecycle_ == DocumentLifecycle::Failed);
        REQUIRE(!Session.CompleteCancellation(Loading));
    }
    SourceIdentity PreparedSource()
    {
        SourceIdentity Source; Source.localPath_ = "temporary/input.hwp";
        Source.inputSha256_ = std::string(64, 'a'); return Source;
    }
    SourceAuthorityBinding Authority(const DocumentSession& Session, bool Writable = true)
    {
        const auto State = Session.Snapshot();
        SourceAuthorityBinding Binding{State.id_, State.generation_, State.revision_, State.model_,
            State.source_.inputSha256_, std::string(32, 'b')};
        Binding.origin_.id_ = {{"archive-provider"}, "original/entry", IdentityScope::Persistent, ResourceRevision{"r1"}};
        Binding.origin_.locator_ = {LocatorKind::Archive, "original.zip"};
        Binding.originalWritable_ = Writable;
        return Binding;
    }
    void AuthorityPreservesModelAndHistory()
    {
        DocumentSession Session("authority", {}, DocumentContentMode::ExternalModel, Writable);
        Load(Session, PreparedSource());
        Change(Session, 1, "changed"); Change(Session, 2, "base");
        Session.SetHistoryState(true, true);
        const auto Before = Session.Snapshot();
        REQUIRE(!Before.dirty_); REQUIRE(!Session.Capabilities().save_);
        REQUIRE(Session.BindSourceAuthority(Authority(Session)).HasValue());
        const auto After = Session.Snapshot();
        REQUIRE(After.generation_ == Before.generation_ && After.revision_ == Before.revision_);
        REQUIRE(After.savedRevision_ == Before.savedRevision_ && After.model_ == Before.model_);
        REQUIRE(After.savedModel_ == Before.savedModel_ && !After.dirty_);
        REQUIRE(After.source_.localPath_ == Before.source_.localPath_);
        REQUIRE(After.source_.inputSha256_ == Before.source_.inputSha256_);
        REQUIRE(After.source_.entry_->locator_.kind_ == LocatorKind::Archive);
        REQUIRE(Session.Capabilities().undo_ && Session.Capabilities().redo_ && Session.Capabilities().save_);
        REQUIRE(!Session.BindSourceAuthority(Authority(Session)).HasValue());
    }
    void AuthorityRejectsMismatchAndMalformed()
    {
        DocumentSession Session("authority", {}, DocumentContentMode::ExternalModel, Writable);
        Load(Session, PreparedSource()); const auto Good = Authority(Session);
        for (int Case = 0; Case < 10; ++Case) {
            auto Bad = Good;
            switch (Case) {
            case 0: Bad.documentId_ = "another"; break;
            case 1: ++Bad.generation_; break;
            case 2: ++Bad.revision_; break;
            case 3: ++Bad.model_.epoch_; break;
            case 4: ++Bad.model_.changeSequence_; break;
            case 5: Bad.model_.savepoint_ = "another"; break;
            case 6: Bad.inputSha256_ = std::string(64, 'c'); break;
            case 7: Bad.sourceToken_ = std::string(32, 'Z'); break;
            case 8: Bad.origin_.directory_ = true; break;
            case 9: Bad.origin_.id_.revision_.reset(); break;
            }
            REQUIRE(!Session.BindSourceAuthority(Bad).HasValue());
            REQUIRE(Session.Snapshot().source_.sourceToken_.empty());
        }
        REQUIRE(Session.BindSourceAuthority(Good).HasValue());
        DocumentSession Missing("missing", {}, DocumentContentMode::ExternalModel);
        Load(Missing, SourceIdentity{}); REQUIRE(!Missing.BindSourceAuthority(Authority(Missing)).HasValue());
        DocumentSession Text("text"); REQUIRE(Text.CompleteOpen(1, OpenedDocument{{}, "text"}));
        REQUIRE(!Text.BindSourceAuthority(Authority(Text)).HasValue());
    }
    void AuthorityRejectsDirtyBusyAndStale()
    {
        DocumentSession Session("authority", {}, DocumentContentMode::ExternalModel, Writable);
        Load(Session, PreparedSource()); const auto Old = Authority(Session);
        Change(Session, 1, "dirty");
        REQUIRE(!Session.BindSourceAuthority(Authority(Session)).HasValue());
        Change(Session, 2, "base");
        const auto Checkpoint = Session.BeginCheckpoint().Value();
        REQUIRE(!Session.BindSourceAuthority(Authority(Session)).HasValue());
        Session.Cancel(); REQUIRE(!Session.BindSourceAuthority(Authority(Session)).HasValue());
        REQUIRE(Session.CompleteCancellation(Checkpoint));
        REQUIRE(!Session.BindSourceAuthority(Old).HasValue());
        const auto BeforeReload = Authority(Session);
        const auto Loading = Session.BeginLoad().Value();
        REQUIRE(!Session.BindSourceAuthority(Authority(Session)).HasValue());
        REQUIRE(Session.CompleteLoad(Loading, LoadedExternalModel{PreparedSource(), Model(0, "base", 9)}));
        REQUIRE(!Session.BindSourceAuthority(BeforeReload).HasValue());
        REQUIRE(Session.Close(CloseDisposition::KeepChanges).HasValue());
        REQUIRE(!Session.BindSourceAuthority(Authority(Session)).HasValue());
    }
    void AuthorityPreservesLeaseAndPolicy()
    {
        struct Lease final : IMaterializedFileLease
        {
            std::string LocalPath() const override { return "temporary/input.hwp"; }
            EntrySnapshot Describe() const override { return {}; }
            std::size_t Depth() const override { return 1; }
        };
        auto Shared = std::make_shared<Lease>(); std::weak_ptr<Lease> Weak = Shared;
        auto Source = PreparedSource(); Source.lease_ = Shared; Shared.reset();
        DocumentSession Session("readonly", {}, DocumentContentMode::ExternalModel);
        Load(Session, std::move(Source));
        const auto Pointer = Weak.lock().get();
        REQUIRE(Session.BindSourceAuthority(Authority(Session)).HasValue());
        REQUIRE(Session.Snapshot().source_.lease_.get() == Pointer);
        REQUIRE(!Session.Capabilities().save_ && !Session.Capabilities().edit_ && !Session.Capabilities().saveAs_);
        REQUIRE(Session.Close(CloseDisposition::KeepChanges).HasValue()); REQUIRE(Weak.expired());
        DocumentSession View("archive-readonly", {}, DocumentContentMode::ExternalModel, Writable);
        Load(View, PreparedSource()); REQUIRE(View.BindSourceAuthority(Authority(View, false)).HasValue());
        REQUIRE(!View.Capabilities().save_);
    }
    RecoverySnapshot Recovery()
    {
        RecoverySnapshot Value; Value.document_.id_ = "recovered";
        Value.document_.contentMode_ = DocumentContentMode::ExternalModel;
        Value.document_.generation_ = 8; Value.document_.revision_ = 9; Value.document_.savedRevision_ = 3;
        Value.document_.model_ = Model(5, "edited"); Value.document_.savedModel_ = Model(0, "base");
        Value.document_.dirty_ = true; Value.bundleSha256_ = std::string(64, 'a');
        Value.format_ = "hwp"; Value.engineSchema_ = RecoveryEngineSchema; Value.bytes_ = "validated snapshot";
        return Value;
    }
    void RecoveryPreservesDirtyBaseline()
    {
        auto Recovered = Recovery(); DocumentSession Session("recovered", {}, DocumentContentMode::ExternalModel, Writable);
        auto Ticket = Session.BeginLoad().Value();
        REQUIRE(Session.CompleteRestore(Ticket, {{}, Model(0, "edited", 11)}, Recovered, Recovered.bundleSha256_, "hwp"));
        const auto Snapshot = Session.Snapshot(); REQUIRE(Snapshot.dirty_); REQUIRE(Snapshot.revision_ == 10);
        REQUIRE(Snapshot.savedRevision_ == 3); REQUIRE(Snapshot.savedModel_.savepoint_ == "base");
        REQUIRE(Snapshot.model_.epoch_ == 11 && Snapshot.savedModel_.epoch_ == 11);
        REQUIRE(!Snapshot.source_.originalWritable_ && Snapshot.source_.sourceToken_.empty());
        REQUIRE(!Session.Capabilities().save_); REQUIRE(Session.Capabilities().saveAs_);
        REQUIRE(Session.ApplyModelChange("recovered", Snapshot.generation_, Model(1, "base", 11), false, true));
        REQUIRE(!Session.Snapshot().dirty_);
    }
    void RecoveryRejectsIncompatibleAndAuthority()
    {
        for (int Mode = 0; Mode < 5; ++Mode) {
            auto Recovered = Recovery(); DocumentSession Session("recovered", {}, DocumentContentMode::ExternalModel, Writable);
            auto Ticket = Session.BeginLoad().Value(); SourceIdentity Source;
            if (Mode == 0) Recovered.bundleSha256_ = std::string(64, 'b');
            if (Mode == 1) Recovered.format_ = "hwpx";
            if (Mode == 2) Recovered.document_.dirty_ = false;
            if (Mode == 3) Source.originalWritable_ = true;
            if (Mode == 4) Recovered.document_.savedModel_.epoch_ = 99;
            REQUIRE(Session.CompleteRestore(Ticket, {Source, Model(0, "edited", 11)}, Recovered, std::string(64, 'a'), "hwp"));
            REQUIRE(Session.Snapshot().lifecycle_ == DocumentLifecycle::Failed);
            REQUIRE(!Session.IsBusy()); REQUIRE(!Session.Capabilities().save_);
        }
    }
    void InlineBytesRemainCompatible()
    {
        DocumentSession Text("text"); REQUIRE(Text.CompleteOpen(1, OpenedDocument{Source(), "base"}));
        REQUIRE(Text.Execute("document.setText", "edited").HasValue());
        auto Ticket = Text.Save({SaveDisposition::Original, "source.hwp"}).Value();
        REQUIRE(Text.Execute("document.setText", "newer").HasValue()); REQUIRE(Text.CompleteSave(Ticket, {true}));
        REQUIRE(Text.Snapshot().dirty_); REQUIRE(Text.Execute("document.setText", "edited").HasValue()); REQUIRE(!Text.Snapshot().dirty_);
        REQUIRE(!Text.BeginLoad().HasValue()); REQUIRE(!Text.BeginExport({SaveDisposition::Original, "source.hwp"}).HasValue());
        DocumentSession Binary("binary"); Binary.SetBinaryContent(true); const std::string Bytes("PDF\0data", 8);
        REQUIRE(Binary.CompleteOpen(1, OpenedDocument{Source(), Bytes})); REQUIRE(Binary.Text() == Bytes);
        REQUIRE(Binary.Save({SaveDisposition::SaveAs, "copy.pdf"}).Value().text_ == Bytes);
    }
}
int main()
{
    struct Test { const char* name_; void (*run_)(); };
    const Test Tests[] = {
        {"read-only default", ReadOnlyDefault}, {"cancel and late load", LoadCancellationAndLateCompletion},
        {"invalid load retry", InvalidLoadAndRetry}, {"undo savepoint and event ordering", UndoSavepointAndEventOrdering},
        {"edit during export and commit", ExportAndEditDuringCommit}, {"export failures and tampering", ExportFailureRetryAndTampering},
        {"cancel versus new request", CancellationDoesNotConsumeNewRequest}, {"checkpoint is not save", CheckpointDoesNotSaveDocument},
        {"renderer epoch replacement", RendererRestartInvalidatesOldEpoch}, {"payload budget and lease", PayloadBudgetsAndLeaseLifetime},
        {"strict export admission", StrictExportAdmission}, {"cancel ACK and terminal result", CancelledTerminalResultAndWrongAck},
        {"inline bytes regression", InlineBytesRemainCompatible},
        {"recovery preserves baseline", RecoveryPreservesDirtyBaseline},
        {"recovery rejects incompatible authority", RecoveryRejectsIncompatibleAndAuthority},
        {"authority preserves model", AuthorityPreservesModelAndHistory},
        {"authority rejects mismatch", AuthorityRejectsMismatchAndMalformed},
        {"authority rejects busy or stale", AuthorityRejectsDirtyBusyAndStale},
        {"authority preserves lease and policy", AuthorityPreservesLeaseAndPolicy}
    };
    int Failed = 0;
    for (const auto& Test : Tests) {
        try { Test.run_(); std::cout << "PASS " << Test.name_ << '\n'; }
        catch (const std::exception& Error) { ++Failed; std::cerr << "FAIL " << Test.name_ << ": " << Error.what() << '\n'; }
    }
    std::cout << std::size(Tests) - Failed << " passed, " << Failed << " failed\n";
    return Failed ? 1 : 0;
}
