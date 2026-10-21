#pragma once
#include "documentTypes.hpp"

namespace hc::document
{
    struct RecoverySnapshot;
    // Owner-serialized state. Workers receive immutable tickets, never sessions
    // or GUI objects, and complete on the owner after generation validation.
    class IDocumentSession
    {
    public:
        virtual ~IDocumentSession() = default;
        virtual DocumentSnapshot Snapshot() const = 0;
        virtual DocumentCapabilities Capabilities() const = 0;
        virtual storage::Result<storage::Unit> Execute(std::string_view Command, std::string Text) = 0;
        virtual storage::Result<SaveTicket> Save(SaveRequest Request) = 0;
        virtual void Cancel() = 0;
        virtual storage::Result<storage::Unit> Close(CloseDisposition Disposition) = 0;
    };
    class DocumentSession final : public IDocumentSession
    {
    public:
        explicit DocumentSession(DocumentId Id, SourceIdentity Source = {},
            DocumentContentMode Mode = DocumentContentMode::InlineBytes, ExternalModelPolicy Policy = {});
        DocumentSnapshot Snapshot() const override { return snapshot_; }
        DocumentCapabilities Capabilities() const override;
        storage::Result<storage::Unit> Execute(std::string_view Command, std::string Text) override;
        storage::Result<SaveTicket> Save(SaveRequest Request) override;
        void Cancel() override;
        bool Cancel(const DocumentOperationTicket& Ticket);
        bool Cancel(const SaveTicket& Ticket);
        storage::Result<storage::Unit> Close(CloseDisposition Disposition) override;
        storage::Result<std::uint64_t> BeginOpen(bool Discard = false);
        bool CompleteOpen(std::uint64_t Generation, storage::Result<OpenedDocument> Result);
        bool CompleteSave(const SaveTicket& Ticket, DocumentSaveResult Result);
        storage::Result<DocumentOperationTicket> BeginLoad(bool Discard = false);
        bool CompleteLoad(const DocumentOperationTicket& Ticket, storage::Result<LoadedExternalModel> Result);
        // Restore validated recovery with its original saved baseline. The native
        // owner supplies freshly acquired read authority and the current bundle pin.
        bool CompleteRestore(const DocumentOperationTicket& Ticket, LoadedExternalModel Loaded,
            const RecoverySnapshot& Recovery, const std::string& BundleSha256, const std::string& Format);
        // Full-state notifications, not deltas. Sequence gaps are permitted.
        bool ApplyModelChange(const DocumentId& Id, std::uint64_t Generation,
            ExternalModelRevision Model, bool Undo, bool Redo);
        storage::Result<DocumentOperationTicket> BeginExport(SaveRequest Request);
        storage::Result<SaveTicket> CompleteExport(const DocumentOperationTicket& Ticket,
            storage::Result<ExternalModelPayload> Result);
        storage::Result<DocumentOperationTicket> BeginPrint();
        bool CompletePrint(const DocumentOperationTicket& Ticket, storage::Result<storage::Unit> Result);
        storage::Result<DocumentOperationTicket> BeginCheckpoint();
        storage::Result<DocumentCheckpoint> CompleteCheckpoint(const DocumentOperationTicket& Ticket,
            storage::Result<ExternalModelPayload> Result);
        // Cancel requests retirement. The exact operation ACK (or a terminal
        // result) releases busy; the worker ticket retains its source lease.
        bool CompleteCancellation(const DocumentOperationTicket& Ticket);
        // One-time authority adoption on a clean external model. Does not reload,
        // change model/history/generation or replace the materialized input lease.
        storage::Result<storage::Unit> BindSourceAuthority(SourceAuthorityBinding Binding);
        bool IsBusy() const { return saving_.has_value() || pending_.has_value(); }
        void SetHistoryState(bool Undo, bool Redo);
        void SetBinaryContent(bool Binary) { binary_ = Binary; }
        void SetUnlimitedBytes(bool Enabled) { unlimitedBytes_ = Enabled; }
        const std::string& Text() const { return text_; }
    private:
        struct PendingOperation
        {
            DocumentOperationTicket ticket_;
            SaveRequest save_;
            SourceIdentity source_;
        };
        bool matches(const DocumentOperationTicket& Ticket, DocumentOperation Operation) const;
        void finishOperation();
        void updateModelDirty();
        storage::Result<DocumentOperationTicket> beginModelOperation(DocumentOperation Operation);
        DocumentSnapshot snapshot_;
        ExternalModelPolicy modelPolicy_;
        std::optional<PendingOperation> pending_;
        std::uint64_t nextOperation_ = 0;
        std::string text_, savedText_;
        std::optional<SaveTicket> saving_;
        std::uint64_t nextSave_ = 0;
        bool undo_ = false, redo_ = false, binary_ = false;
        bool unlimitedBytes_ = false;
    };
}
