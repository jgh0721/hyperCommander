#include "hwpSdiWindow.hpp"
#include <QCloseEvent>
#include <QLabel>
#include <QVBoxLayout>
#include <QTimer>
#include <QPointer>
#include <atomic>
#include <algorithm>
#include <QUuid>
#include <stdexcept>
#include <QCryptographicHash>
#include <QJsonArray>
#include "hwpInputInspection.hpp"

namespace hc::document
{
    namespace
    {
        std::atomic<quint64> NextBinding{0};
    }
    HwpSdiWindow::HwpSdiWindow(DocumentId Id, SourceIdentity Source, std::unique_ptr<HwpSdiBackend> Backend, bool Editable)
        : QWidget(nullptr, Qt::Window), session_(std::move(Id), std::move(Source), DocumentContentMode::ExternalModel, ExternalModelPolicy{Editable, Editable, Editable, Editable, ExportRevisionPolicy::StrictCurrentRevision, true}),
          backend_(Backend.get()), status_(new QLabel(this)), binding_(++NextBinding)
    {
        if (!backend_) throw std::invalid_argument("RHWP backend is required");
        qRegisterMetaType<RecoverySnapshot>();
        qRegisterMetaType<HwpPrintFrames>();
        qRegisterMetaType<DocumentOperationTicket>();
        qRegisterMetaType<HwpLoadResult>();
        qRegisterMetaType<ExternalModelRevision>(); qRegisterMetaType<SaveTicket>();
        connect(backend_, &HwpSdiBackend::ModelChanged, this, [this](quint64 Binding, ExternalModelRevision Model) {
            if (Binding != binding_ || stopped_ || closing_) return;
            const auto State = session_.Snapshot();
            if (session_.ApplyModelChange(State.id_, State.generation_, std::move(Model), false, false)) updateState();
        }, Qt::QueuedConnection);
        connect(backend_, &HwpSdiBackend::EditorReply, this, &HwpSdiWindow::editorReply, Qt::QueuedConnection);
        auto* Deadline = new QTimer(this); Deadline->setInterval(100);
        connect(Deadline, &QTimer::timeout, this, [this] {
            if ((!closeSync_.isEmpty() || discardPending_ || resumePending_ || printCancelling_) && controlDeadline_.hasExpired()) {
                if (!controlTimedOut_) { controlTimedOut_ = true; emit ControlTimeout(); }
                return;
            }
            if (discardPending_ && !discardSent_)
                discardSent_ = backend_->EditorCommand({{"type", "DocumentEditorDiscard"}});
            if (resumePending_ && !resumeSent_ && !discardPending_)
                resumeSent_ = backend_->EditorCommand({{"type", "DocumentEditorResume"}});
            if (print_ && !print_->Error().isEmpty() && !printCancelling_) {
                const auto Code=print_->Error();failPrint(Code);return;
            }
            if (printCancelling_) backend_->EditorCommand({{"type","DocumentPrintCancel"},{"printIdentity",HwpPrintIdentity(*printTicket_)}});
            if (export_ && export_->CurrentState() == HwpExportTransfer::State::Failed) failExport(export_->Error());
        }); Deadline->start();
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(0, 0, 0, 0);
        status_->setAlignment(Qt::AlignCenter);
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        Layout->addWidget(status_);
        Layout->addWidget(backend_, 1);
        Backend.release(); // QWidget parent now owns the GUI port.
        setFocusProxy(backend_);
        resize(960, 720);
        // Queued delivery also protects against synchronous completion/reentrancy
        // from a backend port. QObject receiver lifetime drops obsolete callbacks.
        connect(backend_, &HwpSdiBackend::Loaded, this, &HwpSdiWindow::loaded, Qt::QueuedConnection);
        connect(backend_, &HwpSdiBackend::Cancelled, this, &HwpSdiWindow::cancelled, Qt::QueuedConnection);
        connect(backend_, &HwpSdiBackend::Stopped, this, &HwpSdiWindow::stopped, Qt::QueuedConnection);
        updateState();
    }
    storage::Result<storage::Unit> HwpSdiWindow::BindSourceAuthority(SourceAuthorityBinding Binding)
    {
        if (closing_ || stopped_)
            return storage::Error{storage::ErrorCategory::InvalidRequest, "rhwp-authority-unavailable"};
        auto Result = session_.BindSourceAuthority(std::move(Binding));
        if (Result.HasValue()) updateState();
        return Result;
    }
    bool HwpSdiWindow::Open()
    {
        if (closing_ || stopped_) return false;
        auto Ticket = session_.BeginLoad();
        if (!Ticket.HasValue()) return false;
        pending_ = Ticket.Value();
        // Capture values before any external callback can run.
        const auto Request = *pending_;
        const auto Source = session_.Snapshot().source_;
        backend_->Load(binding_, Request, Source);
        updateState();
        return true;
    }
    void HwpSdiWindow::Cancel()
    {
        if (printTicket_) { CancelPrint(); return; }
        if (!pending_ || session_.Snapshot().cancelRequested_) return;
        session_.Cancel();
        backend_->Cancel(binding_, *pending_);
        updateState();
    }
    DocumentSnapshot HwpSdiWindow::Snapshot() const
    {
        auto State = session_.Snapshot();
        if (stopped_ && !closed_ && State.lifecycle_ == DocumentLifecycle::Ready) {
            State.lifecycle_ = DocumentLifecycle::Failed;
            State.error_ = storage::Error{storage::ErrorCategory::InvalidRequest, "rhwp-backend-stopped"};
        }
        return State;
    }
    DocumentCapabilities HwpSdiWindow::Capabilities() const
    {
        auto Caps = session_.Capabilities();
        if (stopped_ || closing_ || discardPending_ || !closeSync_.isEmpty() || resumePending_) return {};
        return Caps;
    }
    bool HwpSdiWindow::IsBusy() const
    {
        return session_.IsBusy() || discardPending_ || resumePending_ || (stopRequested_ && !stopped_);
    }
    void HwpSdiWindow::loaded(quint64 Binding, const DocumentOperationTicket& Ticket, HwpLoadResult Result)
    {
        if (Binding != binding_ || stopped_ || !pending_ || Ticket != *pending_) return;
        // Source identity/authority belongs to native code, never the renderer.
        storage::Result<LoadedExternalModel> Loaded = Result.HasValue()
            ? storage::Result<LoadedExternalModel>(LoadedExternalModel{session_.Snapshot().source_, Result.Value()})
            : storage::Result<LoadedExternalModel>(Result.Failure());
        bool Applied = false;
        if (recovery_ && Loaded.HasValue()) {
            // Bind the reparsed native model to the verified recovery content.
            // The old saved baseline remains distinct when edits were unsaved.
            auto Record = std::move(*recovery_); recovery_.reset();
            const auto Sha = QCryptographicHash::hash(QByteArray::fromStdString(Record.bytes_), QCryptographicHash::Sha256).toHex().toStdString();
            if (Loaded.Value().source_.inputSha256_ != Sha || Loaded.Value().model_.savepoint_ != Sha)
                Applied = session_.CompleteLoad(Ticket, storage::Error{storage::ErrorCategory::InvalidRequest, "rhwp-recovery-input-mismatch"});
            else {
                Record.document_.model_.savepoint_ = Sha;
                Record.document_.savedModel_.savepoint_ = Record.document_.dirty_ ? "recovery-unsaved-baseline" : Sha;
                Applied = session_.CompleteRestore(Ticket, std::move(Loaded.Value()), Record,
                    recoveryPin_.toStdString(), recoveryFormat_.toStdString());
            }
        } else Applied = session_.CompleteLoad(Ticket, std::move(Loaded));
        if (!Applied) return;
        pending_.reset();
        updateState();
    }
    void HwpSdiWindow::cancelled(quint64 Binding, const DocumentOperationTicket& Ticket)
    {
        if (Binding != binding_ || !pending_ || Ticket != *pending_) return;
        if (!session_.CompleteCancellation(Ticket)) return;
        pending_.reset();
        updateState();
    }
    bool HwpSdiWindow::SynchronizeForClose()
    {
        if (stopped_ || closing_ || IsBusy() || !closeSync_.isEmpty()) return false;
        RetryControl();
        closeSync_ = QUuid::createUuid().toString(QUuid::Id128);
        backend_->setEnabled(false);
        if (!backend_->EditorCommand({{"type", "DocumentEditorSynchronize"}, {"id", closeSync_}})) {
            closeSync_.clear(); backend_->setEnabled(true); return false;
        }
        return true;
    }
    void HwpSdiWindow::ResumeEditing()
    {
        closeSync_.clear();
        if (!resumePending_) { RetryControl(); resumePending_ = true; resumeSent_ = resumeAccepted_ = false; }
        updateState();
    }
    void HwpSdiWindow::RetryControl()
    { controlTimedOut_ = false; controlDeadline_ = QDeadlineTimer(30000, Qt::PreciseTimer); }
    void HwpSdiWindow::DiscardAndClose()
    {
        // Explicit user discard authorizes retirement, not a fabricated command ACK.
        closeSync_.clear(); discardPending_ = resumePending_ = false; controlTimedOut_ = false;
        RequestClose(CloseDisposition::Discard);
    }
    bool HwpSdiWindow::SetRecovery(RecoverySnapshot Record, const QByteArray& Pin, const QString& Format)
    {
        const auto State = session_.Snapshot();
        const auto ValidModel = [](const ExternalModelRevision& Model) {
            return Model.epoch_ && !Model.savepoint_.empty() && Model.savepoint_.size() <= 128
                && std::all_of(Model.savepoint_.begin(), Model.savepoint_.end(), [](unsigned char Ch) { return Ch >= 0x21 && Ch <= 0x7e; });
        };
        if (Record.bytes_.empty() || Record.bytes_.size() > 64 * 1024 * 1024 || Record.viewId_ != "rhwp"
            || !ValidModel(Record.document_.model_) || !ValidModel(Record.document_.savedModel_)
            || Record.document_.savedModel_.changeSequence_ > Record.document_.model_.changeSequence_) return false;
        const auto Bytes = QByteArray::fromStdString(Record.bytes_);
        const auto Sha = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex();
        if (pending_ || State.revision_ || State.lifecycle_ != DocumentLifecycle::Opening || recovery_
            || !Record.document_.dirty_ || Record.document_.model_.epoch_ != Record.document_.savedModel_.epoch_
            || Record.document_.model_.savepoint_ == Record.document_.savedModel_.savepoint_
            || Record.document_.id_ != State.id_ || Pin.size() != 64 || QByteArray::fromHex(Pin).toHex() != Pin
            || Record.bundleSha256_ != Pin.toStdString() || Record.engineSchema_ != RecoveryEngineSchema
            || Record.format_ != Format.toStdString() || Record.checksum_ != Sha.toStdString()
            || State.source_.inputSha256_ != Sha.toStdString() || State.source_.originalWritable_
            || !State.source_.sourceToken_.empty()) return false;
        const auto Inspection = InspectHwpInput(Bytes, Format);
        if (!Inspection.accepted_ || Inspection.restricted_) return false;
        recovery_ = std::move(Record); recoveryPin_ = Pin; recoveryFormat_ = Format; return true;
    }
    bool HwpSdiWindow::Checkpoint(const QString& Format, const QByteArray& Pin)
    {
        if (closing_ || stopped_ || IsBusy() || !closeSync_.isEmpty() || export_
            || (Format != "hwp" && Format != "hwpx") || Pin.size() != 64 || QByteArray::fromHex(Pin).toHex() != Pin) return false;
        const auto Captured = session_.Snapshot();
        auto Operation = session_.BeginCheckpoint(); if (!Operation.HasValue()) return false;
        checkpoint_.emplace(); checkpoint_->document_ = Captured;
        checkpoint_->document_.source_.lease_.reset();
        checkpoint_->viewId_ = "rhwp"; checkpoint_->format_ = Format.toStdString();
        checkpoint_->bundleSha256_ = Pin.toStdString(); checkpoint_->engineSchema_ = RecoveryEngineSchema;
        exportTicket_ = Operation.Value();
        export_ = std::make_unique<HwpExportTransfer>(*exportTicket_, exportTicket_->model_.epoch_, 1, Format);
        if (!backend_->EditorCommand({{"type", "DocumentEditorExport"}, {"exportIdentity", export_->Identity()}, {"format", Format}})) {
            failExport("rhwp-checkpoint-command-unavailable"); return false;
        }
        updateState(); return true;
    }
    bool HwpSdiWindow::Save(SaveRequest Request, const QString& Format)
    {
        if (closing_ || stopped_ || !closeSync_.isEmpty() || discardPending_ || resumePending_ || export_ || (Format != "hwp" && Format != "hwpx")) return false;
        auto Operation = session_.BeginExport(std::move(Request));
        if (!Operation.HasValue()) return false;
        exportTicket_ = Operation.Value();
        // Epoch and binding owner are the native process binding identity.
        export_ = std::make_unique<HwpExportTransfer>(*exportTicket_, exportTicket_->model_.epoch_, 1, Format);
        if (!backend_->EditorCommand({{"type", "DocumentEditorExport"}, {"exportIdentity", export_->Identity()}, {"format", Format}})) {
            failExport("rhwp-editor-command-unavailable"); return false;
        }
        updateState(); return true;
    }
    void HwpSdiWindow::failExport(const QString& Error)
    {
        if (!exportTicket_) return;
        const bool Checkpoint = checkpoint_.has_value();
        const storage::Error Failure{storage::ErrorCategory::InvalidRequest, Error.toStdString()};
        if (Checkpoint) session_.CompleteCheckpoint(*exportTicket_, Failure);
        else session_.CompleteExport(*exportTicket_, Failure);
        checkpoint_.reset();
        export_.reset(); exportTicket_.reset();
        RetryControl();
        discardPending_ = !stopped_; discardSent_ = discardAccepted_ = false;
        const QPointer<HwpSdiWindow> Guard(this);
        updateState();
        if (Guard) { if (Checkpoint) emit CheckpointFailed(Error); else emit SaveFailed(Error); }
    }
    bool HwpSdiWindow::Print(const std::vector<int>& Pages)
    {
        if(stopped_||closing_||IsBusy()||!closeSync_.isEmpty())return false;
        int Previous=-1;QJsonArray Selection;
        for(const auto Page:Pages){if(Page<=Previous)return false;Previous=Page;Selection.append(Page);}
        auto Ticket=session_.BeginPrint();if(!Ticket.HasValue())return false;
        printTicket_=Ticket.Value();print_=std::make_unique<HwpPrintTransfer>(*printTicket_,Pages);
        if(!backend_->EditorCommand({{"type","DocumentPrint"},{"printIdentity",HwpPrintIdentity(*printTicket_)},{"pages",Selection}})) {
            CompletePrint(storage::Error{storage::ErrorCategory::InvalidRequest,"rhwp-print-command-unavailable"});return false;
        }
        updateState();return true;
    }
    void HwpSdiWindow::CompletePrint(storage::Result<storage::Unit> Result)
    {
        if(!printTicket_)return;
        session_.CompletePrint(*printTicket_,std::move(Result));print_.reset();printTicket_.reset();printCancelling_=false;updateState();
    }
    void HwpSdiWindow::failPrint(QString Code)
    {
        const QPointer<HwpSdiWindow> Guard(this);CancelPrint();if(Guard)emit PrintFailed(Code);
    }
    void HwpSdiWindow::CancelPrint()
    {
        if(!printTicket_||printCancelling_)return;
        printCancelling_=true;session_.Cancel(*printTicket_);RetryControl();
        backend_->EditorCommand({{"type","DocumentPrintCancel"},{"printIdentity",HwpPrintIdentity(*printTicket_)}});updateState();
    }
    void HwpSdiWindow::editorReply(quint64 Binding, const QJsonObject& Reply)
    {
        if (Binding != binding_ || stopped_) return;
        if(printTicket_) {
            if(printCancelling_) {
                if(Reply["printIdle"]==QJsonValue(true)&&Reply["printRetired"]==HwpPrintIdentity(*printTicket_))CompletePrint(storage::Unit{});
                return;
            }
            if(Reply["requestType"]=="DocumentPrint" && Reply["ok"]!=QJsonValue(true)) {
                failPrint("rhwp-print-rejected");return;
            }
            if(print_ && Reply["printMessage"].isObject()) {
                if(!print_->Handle(Reply["printMessage"].toObject())) {const auto Code=print_->Error();failPrint(Code);return;}
                if(print_->Ready()) {auto Frames=print_->Frames();print_.reset();emit PrintPrepared(std::move(Frames));return;}
            }
        }
        if(Reply["action"]=="print") {if(!IsBusy())emit PrintRequested();return;}
        const auto SyncState = Reply["editor"].toObject();
        if (discardPending_ && Reply["requestType"] == "DocumentEditorDiscard") {
            discardAccepted_ = Reply["ok"] == QJsonValue(true); if (!discardAccepted_) discardSent_ = false;
        }
        if (resumePending_ && Reply["requestType"] == "DocumentEditorResume") {
            resumeAccepted_ = Reply["ok"] == QJsonValue(true); if (!resumeAccepted_) resumeSent_ = false;
        }
        if (resumePending_ && resumeAccepted_ && SyncState["resuming"] == QJsonValue(false)) {
            resumePending_ = resumeSent_ = resumeAccepted_ = false; updateState(); return;
        }
        if (discardPending_ && discardAccepted_ && SyncState["retiring"] == QJsonValue(false) && SyncState["error"].toString().isEmpty()) {
            discardPending_ = discardSent_ = false; updateState(); return;
        }
        if (!closeSync_.isEmpty() && SyncState["synchronized"] == closeSync_) {
            closeSync_.clear(); emit CloseSynchronized(); return;
        }
        if (Reply["requestType"] == "DocumentEditorSynchronize" && Reply["ok"] != QJsonValue(true)) {
            const QPointer<HwpSdiWindow> Guard(this);
            closeSync_.clear(); ResumeEditing(); if (Guard) emit CloseSynchronizationFailed(); return;
        }
        if (!exportTicket_) {
            const auto Action = Reply["action"].toString();
            if (Action == "save" || Action == "saveAs") emit SaveRequested(Action == "saveAs");
            return;
        }
        const auto Editor = Reply["editor"].toObject();
        if (!Editor["error"].toString().isEmpty()) { failExport(Editor["error"].toString()); return; }
        if (Reply["requestType"] == "DocumentEditorExport" && Reply["ok"] != QJsonValue(true)) {
            failExport("rhwp-export-rejected"); return;
        }
        if (!Reply["message"].isObject()) return;
        const auto Result = export_->Handle(Reply["message"].toObject());
        if (Result.contains("error")) { failExport(Result["error"].toString()); return; }
        if (export_->CurrentState() != HwpExportTransfer::State::Ready) return;
        if (checkpoint_) {
            auto Prepared = export_->AdmitCheckpoint(session_);
            if (!Prepared.HasValue()) { failExport(QString::fromStdString(Prepared.Failure().code_)); return; }
            auto Record = std::move(*checkpoint_); checkpoint_.reset(); export_.reset(); exportTicket_.reset();
            const QPointer<HwpSdiWindow> Guard(this); updateState(); if (!Guard) return;
            Record.bytes_ = std::move(Prepared.Value().bytes_);
            Record.checksum_ = QCryptographicHash::hash(QByteArray::fromStdString(Record.bytes_), QCryptographicHash::Sha256).toHex().toStdString();
            emit CheckpointPrepared(std::move(Record)); return;
        }
        auto Prepared = export_->Admit(session_);
        if (!Prepared.HasValue()) { failExport(QString::fromStdString(Prepared.Failure().code_)); return; }
        export_.reset(); exportTicket_.reset();
        const QPointer<HwpSdiWindow> Guard(this);
        updateState();
        if (!Guard) return;
        emit SavePrepared(std::move(Prepared.Value()));
    }
    void HwpSdiWindow::CompleteSave(const SaveTicket& Ticket, DocumentSaveResult Result)
    {
        const auto Committed = Result.committed_;
        if (!session_.CompleteSave(Ticket, std::move(Result))) return;
        if (Committed && session_.Snapshot().model_ == Ticket.model_)
            backend_->EditorCommand({{"type", "DocumentEditorSaved"}, {"sequence", QString::number(Ticket.model_.changeSequence_)},
                {"savepoint", QString::fromStdString(Ticket.model_.savepoint_)}});
        updateState();
    }
    void HwpSdiWindow::RequestClose(CloseDisposition Disposition)
    {
        if (closed_) return;
        if (!closing_ && session_.Snapshot().dirty_ && Disposition == CloseDisposition::KeepChanges) return;
        if (printTicket_ && Disposition != CloseDisposition::Discard) {CancelPrint();return;}
        if (IsBusy() && !pending_ && !closing_ && Disposition != CloseDisposition::Discard) return;
        closeDisposition_ = Disposition; closing_ = true;
        if (pending_ && !session_.Snapshot().cancelRequested_) {
            session_.Cancel();
            backend_->Cancel(binding_, *pending_);
        }
        if (!stopRequested_ && !stopped_) {
            stopRequested_ = true;
            // Stop retires even a hung parser; no GUI wait or fabricated timeout ACK.
            backend_->Stop(binding_);
        }
        if (stopped_) {
            if (!session_.Close(closeDisposition_).HasValue()) return;
            closed_ = true;
            close();
            const QPointer<HwpSdiWindow> Guard(this);
            updateState();
            if (Guard) emit Closed();
            return;
        }
        updateState();
    }
    void HwpSdiWindow::stopped(quint64 Binding)
    {
        if (Binding != binding_ || stopped_) return;
        stopped_ = true; closeSync_.clear();
        discardPending_ = discardSent_ = discardAccepted_ = resumePending_ = resumeSent_ = resumeAccepted_ = false;
        const QPointer<HwpSdiWindow> Guard(this);
        if (printTicket_) {CompletePrint(storage::Error{storage::ErrorCategory::InvalidRequest,"rhwp-backend-stopped"}); if(!Guard)return; emit PrintFailed("rhwp-backend-stopped");if(!Guard)return;}
        if (exportTicket_) failExport("rhwp-backend-stopped");
        if (!Guard) return;
        if (pending_) {
            // Backend-wide retirement proves this operation can no longer touch
            // its lease. Unsolicited process death is a terminal load failure.
            if (session_.Snapshot().cancelRequested_) session_.CompleteCancellation(*pending_);
            else session_.CompleteLoad(*pending_, storage::Error{storage::ErrorCategory::InvalidRequest, "rhwp-backend-stopped"});
            pending_.reset();
        }
        if (closing_) RequestClose(closeDisposition_);
        else updateState();
    }
    void HwpSdiWindow::closeEvent(QCloseEvent* Event)
    {
        if (closed_) { Event->accept(); return; }
        Event->ignore();
        // Never recurse through closeEvent while close() is still dispatching.
        QTimer::singleShot(0, this, [this] { RequestClose(); });
    }
    void HwpSdiWindow::updateState()
    {
        const auto State = session_.Snapshot();
        const bool Ready = !closing_ && !stopped_ && State.lifecycle_ == DocumentLifecycle::Ready;
        // A hidden WebEngine/native child may defer first paint. Keep the
        // loading surface laid out and visible; block input until load completes.
        const bool Surface = !stopped_ && !closed_
            && (State.lifecycle_ == DocumentLifecycle::Opening || State.lifecycle_ == DocumentLifecycle::Ready);
        backend_->setVisible(Surface);
        backend_->setEnabled(Ready && closeSync_.isEmpty() && !resumePending_);
        status_->setVisible(!Ready);
        status_->setText(closing_ ? tr("Closing document…")
            : stopped_ ? tr("Document viewer stopped.")
            : State.cancelRequested_ ? tr("Cancelling…")
            : State.lifecycle_ == DocumentLifecycle::Failed ? tr("Unable to open document.")
            : tr("Opening document…"));
        setWindowTitle(tr("HWP Viewer"));
        emit StateChanged();
    }
}
