#pragma once
#include "hwpExportTransfer.hpp"
#include "commonLibs/document/documentSession.hpp"
#include <QJsonArray>
#include <memory>
namespace hc::document {
    // Owner-thread protocol state. Long WASM export is driven separately; no
    // method waits for it. The existing v2 saveStream protocol remains unchanged.
    class HwpAsyncSave final {
    public:
        QJsonObject Begin(DocumentSession& Session, const SaveRequest& Request,
            const QJsonArray& PeerCapabilities, bool NativeWriteAllowed, const QString& Format,
            quint64 BindingOwner, quint64 BindingGeneration, int TimeoutMs = 30000);
        QJsonObject Accept(const QJsonObject& RendererMessage);
        QJsonObject Poll(DocumentSession& Session, const QString& ExportToken);
        QJsonObject Read(const QString& SaveToken, quint64 Offset);
        bool Complete(DocumentSession& Session, const QString& SaveToken, DocumentSaveResult Result);
        void RequestCancel(DocumentSession& Session);
        // Native owner only, after the renderer/worker actually retires.
        bool Retired(DocumentSession& Session);
        QJsonObject Identity() const { return transfer_ ? transfer_->Identity() : QJsonObject{}; }
    private:
        bool matches(const DocumentSession& Session) const;
        std::unique_ptr<HwpExportTransfer> transfer_;
        std::optional<DocumentOperationTicket> operation_;
        std::optional<SaveTicket> save_;
        QString exportToken_, saveToken_, error_;
        quint64 offset_ = 0;
        bool cancelRequested_ = false, done_ = false;
    };
}
