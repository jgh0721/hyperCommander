#include "hwpExportTransfer.hpp"
#include "hwpInputInspection.hpp"
#include "commonLibs/document/documentSession.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <cmath>

namespace hc::document
{
    HwpExportTransfer::HwpExportTransfer(DocumentOperationTicket Ticket, quint64 BindingOwner,
        quint64 BindingGeneration, QString Format, int TimeoutMs)
        : ticket_(std::move(Ticket)), format_(std::move(Format))
    {
        const auto Id = QString::fromStdString(ticket_.documentId_);
        const auto Savepoint = QString::fromStdString(ticket_.model_.savepoint_);
        if (Id.isEmpty() || Id.size() > 128 || Id.toStdString() != ticket_.documentId_
            || !ticket_.generation_ || !ticket_.token_ || !ticket_.revision_ || !ticket_.model_.epoch_
            || Savepoint.isEmpty() || Savepoint.size() > 128 || Savepoint.toStdString() != ticket_.model_.savepoint_
            || !BindingOwner || !BindingGeneration || TimeoutMs < 1 || TimeoutMs > 120000
            || (ticket_.operation_ != DocumentOperation::Exporting && ticket_.operation_ != DocumentOperation::Checkpointing)
            || (format_ != "hwp" && format_ != "hwpx")) { error_ = "rhwp-export-admission"; return; }
        identity_ = {{"documentId", Id}, {"generation", QString::number(ticket_.generation_)},
            {"token", QString::number(ticket_.token_)}, {"revision", QString::number(ticket_.revision_)},
            {"epoch", QString::number(ticket_.model_.epoch_)},
            {"changeSequence", QString::number(ticket_.model_.changeSequence_)}, {"savepoint", Savepoint},
            {"bindingOwner", QString::number(BindingOwner)}, {"bindingGeneration", QString::number(BindingGeneration)}};
        deadline_ = QDeadlineTimer(TimeoutMs, Qt::PreciseTimer); state_ = State::Waiting;
    }
    QJsonObject HwpExportTransfer::reply(const char* Type) const
    { return {{"version", 1}, {"type", Type}, {"identity", identity_}, {"received", bytes_.size()}}; }
    QJsonObject HwpExportTransfer::fail(const char* Code)
    { state_ = State::Failed; error_ = QString::fromLatin1(Code); bytes_.clear(); return {{"version", 1}, {"error", error_}}; }
    void HwpExportTransfer::expire()
    {
        if ((state_ == State::Waiting || state_ == State::Receiving || state_ == State::Ready) && deadline_.hasExpired())
            fail("rhwp-export-timeout");
    }
    HwpExportTransfer::State HwpExportTransfer::CurrentState() { expire(); return state_; }
    void HwpExportTransfer::Cancel()
    { if (state_ == State::Waiting || state_ == State::Receiving || state_ == State::Ready) fail("rhwp-export-cancelled"); }
    QJsonObject HwpExportTransfer::Handle(const QJsonObject& Request)
    {
        expire();
        if (state_ != State::Waiting && state_ != State::Receiving)
            return {{"version", 1}, {"error", "rhwp-export-state"}};
        // A foreign reply must not destroy an unrelated in-flight operation.
        if (Request["identity"].toObject() != identity_) return {{"version", 1}, {"error", "rhwp-export-stale"}};
        if (Request["version"] != QJsonValue(1) || !Request["type"].isString()) return fail("rhwp-export-schema");
        const auto Type = Request["type"].toString();
        if (Type == "ExportBegin" && state_ == State::Waiting) {
            const auto Size = Request["byteLength"].toDouble(-1);
            const auto Sha = Request["sha256"].toString().toLatin1();
            const auto Loss = Request["loss"].toObject();
            if (Request.size() != 7 || Request["format"] != QJsonValue(format_)
                || !Request["byteLength"].isDouble() || !std::isfinite(Size) || Size < 1 || Size > 64 * 1024 * 1024
                || std::floor(Size) != Size || !Request["sha256"].isString() || Sha.size() != 64
                || QByteArray::fromHex(Sha).toHex() != Sha) return fail("rhwp-export-metadata");
            if (!Request["loss"].isObject() || Loss.size() != 4 || Loss["schemaVersion"] != QJsonValue(1)
                || Loss["outputFormat"] != QJsonValue(format_) || Loss["count"] != QJsonValue(0)
                || !Loss["losses"].isArray() || !Loss["losses"].toArray().isEmpty()) return fail("rhwp-export-content-loss");
            length_ = static_cast<qsizetype>(Size); sha256_ = Sha; state_ = State::Receiving;
            return reply("ExportAccepted");
        }
        if (Type == "ExportChunk" && state_ == State::Receiving) {
            if (Request.size() != 5 || !Request["offset"].isDouble()
                || Request["offset"].toDouble(-1) != bytes_.size() || !Request["data"].isString()) return fail("rhwp-export-chunk-order");
            const auto Text = Request["data"].toString();
            if (Text.isEmpty() || Text.size() > 256 * 1024) return fail("rhwp-export-chunk-budget");
            const auto Encoded = Text.toLatin1();
            const auto Bytes = QByteArray::fromBase64(Encoded, QByteArray::AbortOnBase64DecodingErrors);
            if (Bytes.isEmpty() || Bytes.size() > 192 * 1024 || Bytes.toBase64() != Encoded
                || QString::fromLatin1(Encoded) != Text || Bytes.size() > length_ - bytes_.size()) return fail("rhwp-export-chunk");
            bytes_.append(Bytes); return reply("ExportChunkAccepted");
        }
        if (Type == "ExportCommit" && Request.size() == 3 && state_ == State::Receiving) {
            if (bytes_.size() != length_ || QCryptographicHash::hash(bytes_, QCryptographicHash::Sha256).toHex() != sha256_)
                return fail("rhwp-export-integrity");
            // Container sanity is necessary, not feature-fidelity approval.
            if (format_ == "hwp" ? !bytes_.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1"))
                : !bytes_.startsWith(QByteArray::fromHex("504b0304"))) return fail("rhwp-export-container");
            state_ = State::Ready; return reply("ExportReady");
        }
        return fail("rhwp-export-schema");
    }
    std::optional<ExternalModelPayload> HwpExportTransfer::Take()
    {
        expire(); if (state_ != State::Ready) return {};
        state_ = State::Consumed;
        ExternalModelPayload Payload{ticket_.model_, bytes_.toStdString()}; bytes_.clear(); return Payload;
    }
    storage::Result<DocumentCheckpoint> HwpExportTransfer::AdmitCheckpoint(DocumentSession& Session)
    {
        auto Payload = Take();
        if (!Payload) return storage::Error{storage::ErrorCategory::Unavailable,
            error_.isEmpty() ? "rhwp-checkpoint-not-ready" : error_.toStdString()};
        const auto Inspection = InspectHwpInput(QByteArray::fromStdString(Payload->bytes_), format_);
        if (!Inspection.accepted_ || Inspection.restricted_)
            return Session.CompleteCheckpoint(ticket_, storage::Error{storage::ErrorCategory::InvalidRequest, "rhwp-checkpoint-container-policy"});
        return Session.CompleteCheckpoint(ticket_, std::move(*Payload));
    }
    storage::Result<SaveTicket> HwpExportTransfer::Admit(DocumentSession& Session)
    {
        auto Payload = Take();
        // A failed/incomplete transport is not proof that the renderer retired.
        if (!Payload) return storage::Error{storage::ErrorCategory::Unavailable,
            error_.isEmpty() ? "rhwp-export-not-ready" : error_.toStdString()};
        const auto Inspection = InspectHwpInput(QByteArray::fromStdString(Payload->bytes_), format_);
        if (!Inspection.accepted_ || Inspection.restricted_)
            return Session.CompleteExport(ticket_, storage::Error{storage::ErrorCategory::InvalidRequest, "rhwp-export-container-policy"});
        return Session.CompleteExport(ticket_, std::move(*Payload));
    }

}
