#pragma once
#include "commonLibs/document/documentTypes.hpp"
#include "documentWebProcess.hpp"
#include <QJsonObject>
#include <functional>

namespace hc::document
{
    // Blocking native acquisition: call on an owner worker, never the GUI thread.
    // Materializer and request are trusted native services; renderer paths/tokens
    // cannot construct this request. Retain the materializer until cleanup drains.
    // Returns a read-only copy; original write authority requires a separate grant.
    storage::Result<SourceIdentity> PrepareHwpSource(storage::IMaterializer& Materializer,
        storage::MaterializationRequest Request, storage::OperationContext Context);

    // Worker-owned read snapshot. Retain alongside the process owner through
    // proven Job retirement, including failed/cancelled transfers.
    class HwpAdmittedInput final
    {
    public:
        static std::unique_ptr<HwpAdmittedInput> Read(const DocumentOperationTicket& Ticket,
            const SourceIdentity& Source, quint64 BindingOwner, quint64 BindingGeneration,
            quint64 Epoch, const QString& Format, QString& Error);
        QJsonObject Identity() const { return identity_; }
        // Classification only: even unrestricted input is transmitted read-only.
        // This value never grants edit, export or original-source write authority.
        bool IsRestricted() const { return restricted_; }
        using Exchange = std::function<bool(const QByteArray&, QByteArray&, quint32, WebProcessError&)>;
        // True means helper input hash verified, never parsed/first-frame Ready.
        bool Send(const Exchange& Transport, const std::function<bool()>& Cancelled,
            QString& Error, int TimeoutMs = 30000) const;
        bool Send(DocumentWebProcess& Process, const std::function<bool()>& Cancelled,
            QString& Error, int TimeoutMs = 30000) const;
    private:
        HwpAdmittedInput() = default;
        DocumentOperationTicket ticket_;
        std::shared_ptr<void> file_;
        QJsonObject identity_;
        QString format_;
        QByteArray bytes_, sha256_;
        bool restricted_ = false;
    };
}
