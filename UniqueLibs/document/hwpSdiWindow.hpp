#pragma once
#include "commonLibs/document/documentSession.hpp"
#include <QWidget>
#include <QJsonObject>
#include <memory>
#include "hwpExportTransfer.hpp"
#include "hwpPrintBridge.hpp"
#include "commonLibs/document/documentRecovery.hpp"

class QLabel;
namespace hc::document
{
    using HwpLoadResult = storage::Result<ExternalModelRevision>;

    // GUI-thread port for a future admitted RHWP runtime. All methods must return
    // immediately; worker/process ownership must survive widget destruction.
    // Loaded means parser AND first render finished, not navigation completion.
    // Cancelled means the exact operation has retired. Stopped means ALL backend
    // activity and child processes have retired. A timeout alone is not an ACK.
    class HwpSdiBackend : public QWidget
    {
        Q_OBJECT
    public:
        using QWidget::QWidget;
        virtual void Load(quint64 Binding, DocumentOperationTicket Ticket, SourceIdentity Source) = 0;
        virtual void Cancel(quint64 Binding, DocumentOperationTicket Ticket) = 0;
        virtual void Stop(quint64 Binding) = 0;
        virtual bool EditorCommand(const QJsonObject&) { return false; }
    signals:
        void ModelChanged(quint64 Binding, hc::document::ExternalModelRevision Model);
        void EditorReply(quint64 Binding, QJsonObject Reply);
        void Loaded(quint64 Binding, hc::document::DocumentOperationTicket Ticket, hc::document::HwpLoadResult Result);
        void Cancelled(quint64 Binding, hc::document::DocumentOperationTicket Ticket);
        void Stopped(quint64 Binding);
    };

    // Read-only SDI shell. No native workspace chrome and no hidden text editor.
    // The host must close through RequestClose/close(), keep the event loop alive
    // until Closed, and never delete a live window to implement normal shutdown.
    class HwpSdiWindow final : public QWidget
    {
        Q_OBJECT
    public:
        HwpSdiWindow(DocumentId Id, SourceIdentity Source, std::unique_ptr<HwpSdiBackend> Backend, bool Editable = false);
        bool Open();
        bool Checkpoint(const QString& Format, const QByteArray& BundleSha256);
        bool SetRecovery(RecoverySnapshot Snapshot, const QByteArray& BundleSha256, const QString& Format);
        storage::Result<storage::Unit> BindSourceAuthority(SourceAuthorityBinding Binding);
        void Cancel();
        bool Print(const std::vector<int>& Pages = {});
        void CompletePrint(storage::Result<storage::Unit> Result);
        void CancelPrint();
        bool IsPrinting() const { return printTicket_.has_value(); }
        bool Save(SaveRequest Request, const QString& Format);
        void CompleteSave(const SaveTicket& Ticket, DocumentSaveResult Result);
        void RequestClose(CloseDisposition Disposition = CloseDisposition::KeepChanges);
        bool IsBusy() const;
        bool SynchronizeForClose();
        void ResumeEditing();
        bool ControlTimedOut() const { return controlTimedOut_; }
        void RetryControl();
        void DiscardAndClose();
        bool IsClosing() const { return closing_; }
        DocumentSnapshot Snapshot() const;
        DocumentCapabilities Capabilities() const;
    signals:
        void CloseSynchronized();
        void CloseSynchronizationFailed();
        void ControlTimeout();
        void CheckpointPrepared(hc::document::RecoverySnapshot Snapshot);
        void CheckpointFailed(QString Code);
        void SavePrepared(hc::document::SaveTicket Ticket);
        void SaveRequested(bool SaveAs);
        void PrintRequested();
        void PrintPrepared(hc::document::HwpPrintFrames Frames);
        void PrintFailed(QString Code);
        void SaveFailed(QString Code);
        void StateChanged();
        void Closed();
    protected:
        void closeEvent(QCloseEvent* Event) override;
    private:
        void loaded(quint64 Binding, const DocumentOperationTicket& Ticket, HwpLoadResult Result);
        void cancelled(quint64 Binding, const DocumentOperationTicket& Ticket);
        void stopped(quint64 Binding);
        void updateState();
        void editorReply(quint64 Binding, const QJsonObject& Reply);
        void failExport(const QString& Error);
        void failPrint(QString Code);
        DocumentSession session_;
        HwpSdiBackend* backend_;
        QLabel* status_;
        const quint64 binding_;
        std::optional<DocumentOperationTicket> pending_;
        std::optional<DocumentOperationTicket> printTicket_;
        std::unique_ptr<HwpPrintTransfer> print_;
        bool printCancelling_ = false;
        std::optional<DocumentOperationTicket> exportTicket_;
        std::unique_ptr<HwpExportTransfer> export_;
        std::optional<RecoverySnapshot> checkpoint_, recovery_;
        QByteArray recoveryPin_;
        QString recoveryFormat_;
        QString closeSync_;
        QDeadlineTimer controlDeadline_{QDeadlineTimer::Forever};
        bool controlTimedOut_ = false;
        bool discardPending_ = false, discardSent_ = false, discardAccepted_ = false;
        bool resumePending_ = false, resumeSent_ = false, resumeAccepted_ = false;
        CloseDisposition closeDisposition_ = CloseDisposition::KeepChanges;
        bool closing_ = false, stopRequested_ = false, stopped_ = false, closed_ = false;
    };
}
Q_DECLARE_METATYPE(hc::document::DocumentOperationTicket)
Q_DECLARE_METATYPE(hc::document::HwpLoadResult)

Q_DECLARE_METATYPE(hc::document::ExternalModelRevision)

Q_DECLARE_METATYPE(hc::document::SaveTicket)
Q_DECLARE_METATYPE(hc::document::RecoverySnapshot)
