#include "hwpAsyncSave.hpp"
#include <QUuid>
namespace hc::document {
    namespace { QJsonObject Error(const char* Code) { return {{"ok",false},{"error",Code}}; } }
    bool HwpAsyncSave::matches(const DocumentSession& Session) const {
        if(!operation_) return false; const auto State=Session.Snapshot();
        return State.id_==operation_->documentId_ && State.generation_==operation_->generation_
            && State.lifecycle_==DocumentLifecycle::Ready;
    }
    QJsonObject HwpAsyncSave::Begin(DocumentSession& Session,const SaveRequest& Request,
        const QJsonArray& PeerCapabilities,bool NativeWriteAllowed,const QString& Format,
        quint64 BindingOwner,quint64 BindingGeneration,int TimeoutMs) {
        if(!NativeWriteAllowed || !PeerCapabilities.contains("asyncExportV1") || !PeerCapabilities.contains("saveStream"))
            return Error("rhwp-async-export-unsupported");
        if(operation_)return Error("rhwp-async-export-state");
        if((Format!="hwp" && Format!="hwpx") || !BindingOwner || !BindingGeneration || TimeoutMs<1 || TimeoutMs>120000)
            return Error("rhwp-async-export-policy");
        auto Ticket=Session.BeginExport(Request); if(!Ticket.HasValue())return Error("rhwp-async-export-unavailable");
        operation_=Ticket.Value(); transfer_=std::make_unique<HwpExportTransfer>(*operation_,BindingOwner,BindingGeneration,Format,TimeoutMs);
        if(transfer_->CurrentState()==HwpExportTransfer::State::Invalid) {
            Session.CompleteExport(*operation_,storage::Error{storage::ErrorCategory::InvalidRequest,"rhwp-async-export-policy"});
            done_=true;return Error("rhwp-async-export-policy");
        }
        exportToken_=QUuid::createUuid().toString(QUuid::Id128);
        return {{"ok",true},{"state","accepted"},{"exportToken",exportToken_},{"identity",transfer_->Identity()}};
    }
    QJsonObject HwpAsyncSave::Accept(const QJsonObject& Message) {
        if(!transfer_ || done_ || save_ || cancelRequested_)return Error("rhwp-async-export-state");
        return transfer_->Handle(Message);
    }
    QJsonObject HwpAsyncSave::Poll(DocumentSession& Session,const QString& Token) {
        if(Token.isEmpty() || Token!=exportToken_ || !matches(Session))return Error("rhwp-async-export-stale");
        if(done_)return Error("rhwp-async-export-finished");
        if(cancelRequested_)return {{"ok",true},{"state","cancelling"},{"exportToken",exportToken_},{"error",error_}};
        if(!save_) {
            const auto State=transfer_->CurrentState();
            if(State==HwpExportTransfer::State::Waiting || State==HwpExportTransfer::State::Receiving)
                return {{"ok",true},{"state","preparing"},{"exportToken",exportToken_}};
            if(State!=HwpExportTransfer::State::Ready) {
                error_=transfer_->Error();
                if(error_.isEmpty())error_="rhwp-export-state";
                RequestCancel(Session);
                if(!cancelRequested_)return Error("rhwp-async-export-stale");
                return {{"ok",false},{"state","cancelling"},{"exportToken",exportToken_},{"error",error_}};
            }
            auto Saved=transfer_->Admit(Session);
            if(!Saved.HasValue()) {
                error_=QString::fromStdString(Saved.Failure().code_);
                // The deadline may also expire between CurrentState and Take.
                // Only CompleteExport or a retirement ACK may release core busy.
                if(Session.Snapshot().operation_==DocumentOperation::Exporting && Session.IsBusy()) {
                    RequestCancel(Session);
                    if(!cancelRequested_)return Error("rhwp-async-export-stale");
                    return {{"ok",false},{"state","cancelling"},{"exportToken",exportToken_},{"error",error_}};
                }
                done_=true;return {{"ok",false},{"error",error_}};
            }
            save_=std::move(Saved.Value());saveToken_=QUuid::createUuid().toString(QUuid::Id128);
        }
        return {{"ok",true},{"state","streamReady"},{"exportToken",exportToken_},{"saveToken",saveToken_},
            {"byteLength",static_cast<qint64>(save_->text_.size())},{"revision",QString::number(save_->revision_)}};
    }
    QJsonObject HwpAsyncSave::Read(const QString& Token,quint64 Offset) {
        if(!save_ || done_ || cancelRequested_ || Token.isEmpty() || Token!=saveToken_ || Offset!=offset_ || Offset>=save_->text_.size())
            return Error("rhwp-async-save-read");
        const auto Size=qMin<quint64>(192*1024,save_->text_.size()-Offset);
        const auto Bytes=QByteArray(save_->text_.data()+Offset,static_cast<qsizetype>(Size));offset_+=Size;
        return {{"ok",true},{"saveToken",saveToken_},{"offset",static_cast<qint64>(Offset)},
            {"data",QString::fromLatin1(Bytes.toBase64())},{"eof",offset_==save_->text_.size()}};
    }
    bool HwpAsyncSave::Complete(DocumentSession& Session,const QString& Token,DocumentSaveResult Result) {
        if(!save_ || done_ || !matches(Session) || Token.isEmpty() || Token!=saveToken_
            || (Result.committed_ && offset_!=save_->text_.size()))return false;
        // Only a native storage receipt reaches here. A committed write remains
        // authoritative even when cancellation or newer edits arrived during I/O.
        if(!Session.CompleteSave(*save_,std::move(Result)))return false;
        done_=true;save_.reset();return true;
    }
    void HwpAsyncSave::RequestCancel(DocumentSession& Session) {
        if(done_ || !matches(Session))return;
        if(!(save_ ? Session.Cancel(*save_) : Session.Cancel(*operation_)))return;
        cancelRequested_=true;transfer_->Cancel();
    }
    bool HwpAsyncSave::Retired(DocumentSession& Session) {
        if(!cancelRequested_ || done_ || !matches(Session) || save_)return false;
        if(!Session.CompleteCancellation(*operation_))return false;
        done_=true;return true;
    }
}
