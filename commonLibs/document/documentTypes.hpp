#pragma once
#include "commonLibs/storage/materializedContent.hpp"

namespace hc::document
{
    using DocumentId = std::string;
    enum class DocumentLifecycle { Opening, Ready, Failed, Closed };
    enum class DocumentSaveState { Idle, Saving, Conflict, Failed, Exporting };
    enum class OpenIntent { View, Edit };
    enum class SaveDisposition { Original, SaveAs };
    enum class CloseDisposition { KeepChanges, Discard };
    enum class DocumentContentMode { InlineBytes, ExternalModel };
    enum class DocumentOperation { Idle, Loading, Exporting, Checkpointing, Printing };
    // A savepoint identifies complete content within one backend epoch, including
    // formatting. Undo may return to a savepoint, but never rewinds changeSequence.
    struct ExternalModelRevision
    {
        std::uint64_t epoch_ = 0, changeSequence_ = 0;
        std::string savepoint_;
        bool operator==(const ExternalModelRevision&) const = default;
    };
    enum class ExportRevisionPolicy { StrictCurrentRevision, ImmutableSnapshot };
    struct ExternalModelPolicy
    {
        bool edit_ = false, save_ = false, saveAs_ = false, checkpoint_ = false;
        ExportRevisionPolicy exportRevision_ = ExportRevisionPolicy::StrictCurrentRevision;
        bool print_ = false;
    };
    struct DocumentOperationTicket
    {
        DocumentId documentId_;
        std::uint64_t generation_ = 0, token_ = 0, revision_ = 0;
        DocumentOperation operation_ = DocumentOperation::Idle;
        ExternalModelRevision model_;
        // Workers retain this ticket until completion/ACK, including after close.
        std::shared_ptr<storage::IMaterializedFileLease> lease_;
        bool operator==(const DocumentOperationTicket&) const = default;
    };
    struct SourceIdentity
    {
        std::optional<storage::EntrySnapshot> entry_;
        std::shared_ptr<storage::IMaterializedFileLease> lease_;
        std::string localPath_, sourceToken_;
        bool originalWritable_ = false;
        // SHA-256 of admitted input bytes, computed by native acquisition.
        std::string inputSha256_;
    };
    // Trusted native broker request, never a renderer-provided grant. The broker
    // must validate token ownership, provider revision and the acquired byte hash.
    struct SourceAuthorityBinding
    {
        DocumentId documentId_;
        std::uint64_t generation_ = 0, revision_ = 0;
        ExternalModelRevision model_;
        std::string inputSha256_, sourceToken_;
        storage::EntrySnapshot origin_;
        bool originalWritable_ = false;
    };
    struct DocumentCapabilities
    {
        bool view_ = false, edit_ = false, copy_ = false, save_ = false, saveAs_ = false;
        bool undo_ = false, redo_ = false, cancel_ = false, close_ = false, print_ = false;
    };
    struct DocumentSnapshot
    {
        DocumentId id_;
        SourceIdentity source_;
        DocumentLifecycle lifecycle_ = DocumentLifecycle::Opening;
        DocumentSaveState saveState_ = DocumentSaveState::Idle;
        std::uint64_t generation_ = 1, revision_ = 0, savedRevision_ = 0;
        bool dirty_ = false, cancelRequested_ = false;
        std::optional<storage::Error> error_;
        std::vector<storage::Error> cleanupErrors_;
        DocumentContentMode contentMode_ = DocumentContentMode::InlineBytes;
        DocumentOperation operation_ = DocumentOperation::Idle;
        ExternalModelRevision model_, savedModel_;
    };
    struct OpenedDocument
    {
        SourceIdentity source_;
        std::string text_;
    };
    struct SaveRequest
    {
        SaveDisposition disposition_ = SaveDisposition::Original;
        std::string destination_;
    };
    struct SaveTicket
    {
        DocumentId documentId_;
        std::uint64_t generation_ = 0, token_ = 0, revision_ = 0;
        std::string text_, destination_;
        SaveDisposition disposition_ = SaveDisposition::Original;
        SourceIdentity source_;
        DocumentContentMode contentMode_ = DocumentContentMode::InlineBytes;
        ExternalModelRevision model_;
    };
    struct LoadedExternalModel
    {
        SourceIdentity source_;
        ExternalModelRevision model_;
    };
    // Native-validated output only. The future typed bridge must bind content-loss
    // reports, format and byte hash to this same model transaction before delivery.
    struct ExternalModelPayload
    {
        ExternalModelRevision model_;
        std::string bytes_;
    };
    struct DocumentCheckpoint
    {
        DocumentOperationTicket ticket_;
        SourceIdentity source_;
        std::string bytes_;
    };
    struct DocumentSaveResult
    {
        bool committed_ = false;
        std::optional<SourceIdentity> source_;
        std::optional<storage::Error> error_;
        std::vector<storage::Error> cleanupErrors_;
    };
}
