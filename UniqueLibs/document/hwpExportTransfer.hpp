#pragma once
#include "commonLibs/document/documentTypes.hpp"
#include <QByteArray>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <optional>

namespace hc::document
{
    class DocumentSession;
    // Owner-serialized native admission. No destination/path or writer capability
    // comes from the renderer. Failure/discard is not a worker retirement ACK.
    class HwpExportTransfer final
    {
    public:
        enum class State { Invalid, Waiting, Receiving, Ready, Consumed, Failed };
        HwpExportTransfer(DocumentOperationTicket Ticket, quint64 BindingOwner,
            quint64 BindingGeneration, QString Format, int TimeoutMs = 30000);
        QJsonObject Identity() const { return identity_; }
        QJsonObject Handle(const QJsonObject& Request);
        State CurrentState();
        void Cancel();
        // Raw transport result; Admit additionally checks the full container and
        // current native session before producing a storage SaveTicket.
        std::optional<ExternalModelPayload> Take();
        storage::Result<SaveTicket> Admit(DocumentSession& Session);
        storage::Result<DocumentCheckpoint> AdmitCheckpoint(DocumentSession& Session);
        QString Error() const { return error_; }
    private:
        void expire();
        QJsonObject fail(const char* Code);
        QJsonObject reply(const char* Type) const;
        DocumentOperationTicket ticket_;
        QJsonObject identity_;
        QString format_, error_;
        State state_ = State::Invalid;
        QDeadlineTimer deadline_;
        QByteArray bytes_, sha256_;
        qsizetype length_ = 0;
    };
}
