#include "hwpEditBridge.hpp"
#include <QJsonDocument>
#include <QJsonArray>
#include <QUuid>
#include <limits>
#include <utility>
#include <cmath>
namespace hc::document
{
    namespace
    {
        bool Decimal(const QJsonValue& Value, quint64& Number)
        {
            if (!Value.isString()) return false;
            bool Ok = false; const auto Text = Value.toString(); Number = Text.toULongLong(&Ok);
            return Ok && QString::number(Number) == Text;
        }
    }
    HwpEditEndpoint::HwpEditEndpoint(HwpEditBridge* Bridge, QObject* Parent)
        : QObject(Parent), bridge_(Bridge)
    {
        // Revocation is native-only. Losing the remote facade must not leave a
        // live editor whose changes can no longer reach the native owner.
        if (Bridge) connect(this, &QObject::destroyed, Bridge, &HwpEditBridge::Retire);
    }
    QVariantMap HwpEditEndpoint::State() const { return bridge_ ? bridge_->State().toVariantMap() : QVariantMap{}; }
    QVariantMap HwpEditEndpoint::PendingCommand() const { return bridge_ ? bridge_->PendingCommand().toVariantMap() : QVariantMap{}; }
    bool HwpEditEndpoint::Report(const QString& Json) { return bridge_ && bridge_->Report(Json); }
    HwpEditBridge::HwpEditBridge(QObject* Parent) : QObject(Parent) {}
    bool HwpEditBridge::Activate(const DocumentOperationTicket& Load, quint64 Owner,
        quint64 Generation, const QByteArray& Sha, bool Allowed)
    {
        if (used_ || !Allowed || Load.operation_ != DocumentOperation::Loading || Load.documentId_.empty()
            || Load.documentId_.size() > 128 || !Load.generation_ || !Load.token_ || !Owner || !Generation
            || Sha.size() != 64 || QByteArray::fromHex(Sha).toHex() != Sha) return false;
        used_ = active_ = true;
        binding_ = {{"documentId", QString::fromStdString(Load.documentId_)},
            {"generation", QString::number(Load.generation_)}, {"bindingOwner", QString::number(Owner)},
            {"bindingGeneration", QString::number(Generation)}, {"epoch", QString::number(Owner)}};
        model_ = {Owner, 0, Sha.toStdString()}; return true;
    }
    void HwpEditBridge::Retire()
    {
        active_ = exporting_ = false; binding_ = {}; command_ = {}; message_ = {}; userAction_.clear();
    }
    QString HwpEditBridge::TakeUserAction() { return std::exchange(userAction_, {}); }
    void HwpEditBridge::DiscardExport()
    {
        if (retiring_) return;
        message_ = {}; error_.clear(); exporting_ = exportBegun_ = false; retiring_ = true;
        command_ = {{"version", 1}, {"type", "Discard"}, {"id", QUuid::createUuid().toString(QUuid::Id128)}};
    }
    bool HwpEditBridge::Synchronize(const QString& Id)
    {
        if (!active_ || retiring_ || exporting_ || !Error().isEmpty() || Id.size() != 32
            || (!command_.isEmpty() && command_["type"] != "Saved")) return false;
        synchronized_.clear();
        command_ = {{"version", 1}, {"type", "Synchronize"}, {"id", Id}}; return true;
    }
    bool HwpEditBridge::Resume()
    {
        if (!active_ || retiring_ || exporting_) return false;
        synchronized_.clear(); resuming_ = true;
        command_ = {{"version", 1}, {"type", "Resume"}, {"id", QUuid::createUuid().toString(QUuid::Id128)}}; return true;
    }
    QString HwpEditBridge::Error() const
    { return exporting_ && deadline_.hasExpired() ? QString("rhwp-edit-export-timeout") : error_; }
    QJsonObject HwpEditBridge::State() const
    {
        if (!active_) return {};
        return {{"version", 1}, {"binding", binding_}, {"sequence", QString::number(model_.changeSequence_)},
            {"savepoint", QString::fromStdString(model_.savepoint_)}, {"error", Error()},
            {"retiring", retiring_}, {"resuming", resuming_}, {"synchronized", synchronized_}};
    }
    QJsonObject HwpEditBridge::PendingCommand() const
    { return active_ && Error().isEmpty() ? command_ : QJsonObject{}; }
    bool HwpEditBridge::sameBinding(const QJsonObject& Identity) const
    {
        for (auto At = binding_.begin(); At != binding_.end(); ++At)
            if (Identity[At.key()] != At.value()) return false;
        return !binding_.isEmpty();
    }
    bool HwpEditBridge::BeginExport(const QJsonObject& Identity, const QString& Format, int TimeoutMs)
    {
        quint64 Sequence = 0, Revision = 0, Token = 0;
        if (!active_ || !Error().isEmpty() || !command_.isEmpty() || exporting_ || !message_.isEmpty()
            || !sameBinding(Identity) || Identity.size() != 9
            || !Decimal(Identity["changeSequence"], Sequence) || Sequence != model_.changeSequence_
            || !Decimal(Identity["revision"], Revision) || !Revision
            || !Decimal(Identity["token"], Token) || !Token
            || Identity["savepoint"].toString().toStdString() != model_.savepoint_
            || (Format != "hwp" && Format != "hwpx") || TimeoutMs < 1 || TimeoutMs > 120000) return false;
        command_ = {{"version", 1}, {"type", "Export"}, {"id", QUuid::createUuid().toString(QUuid::Id128)},
            {"identity", Identity}, {"format", Format}};
        deadline_ = QDeadlineTimer(TimeoutMs, Qt::PreciseTimer);
        exporting_ = true; exportBegun_ = false; exportOffset_ = exportLength_ = 0; return true;
    }
    bool HwpEditBridge::NotifySaved(const ExternalModelRevision& Model)
    {
        if (!active_ || !Error().isEmpty() || Model != model_ || !command_.isEmpty()
            || exporting_ || !message_.isEmpty()) return false;
        command_ = {{"version", 1}, {"type", "Saved"}, {"id", QUuid::createUuid().toString(QUuid::Id128)},
            {"sequence", QString::number(Model.changeSequence_)}}; return true;
    }
    std::optional<QJsonObject> HwpEditBridge::TakeExportMessage()
    {
        if (!active_ || message_.isEmpty() || !Error().isEmpty()) return {};
        auto Out = std::exchange(message_, {});
        if (Out["type"] == "ExportCommit") { exporting_ = false; command_ = {}; }
        return Out;
    }
    bool HwpEditBridge::Report(const QString& Text)
    {
        if (!active_ || Text.size() > 270 * 1024) return false;
        QJsonParseError Parse; const auto Json = QJsonDocument::fromJson(Text.toUtf8(), &Parse);
        if (Parse.error != QJsonParseError::NoError || !Json.isObject()) return false;
        const auto Object = Json.object();
        if (Object["version"] != QJsonValue(1) || Object["binding"] != QJsonValue(binding_)) return false;
        const auto Type = Object["type"].toString();
        if (Type == "UserAction" && Object.size() == 4 && userAction_.isEmpty()) {
            const auto Action = Object["action"].toString();
            if (Action != "save" && Action != "saveAs") return false;
            userAction_ = Action; return true;
        }
        if (Type == "Changed") {
            quint64 Sequence = 0;
            if (Object.size() != 4 || !Decimal(Object["sequence"], Sequence)
                || model_.changeSequence_ == std::numeric_limits<quint64>::max()
                || Sequence != model_.changeSequence_ + 1) return false;
            model_.changeSequence_ = Sequence;
            // Conservatively dirty after undo as well: the renderer cannot claim
            // equality with a native committed baseline through a boolean flag.
            model_.savepoint_ = std::to_string(model_.epoch_) + ":" + std::to_string(Sequence);
            emit ModelChanged(); return true;
        }
        if (command_.isEmpty() || Object["id"] != command_["id"]) return false;
        if (Type == "Synchronized" && command_["type"] == "Synchronize" && Object.size() == 4) {
            synchronized_ = Object["id"].toString(); command_ = {}; return true;
        }
        if (Type == "CommandObserved" && Object.size() == 4
            && (command_["type"] == "Discard" || command_["type"] == "Resume")) {
            retiring_ = resuming_ = false; command_ = {}; return true;
        }
        if (!Error().isEmpty()) return false;
        if (Type == "SavedObserved" && command_["type"] == "Saved" && Object.size() == 4) {
            command_ = {}; return true;
        }
        if (Type == "Failed" && (Object.size() == 4 || Object.size() == 5)) {
            const auto Phase = Object["phase"].toString();
            if (Object.size() == 5 && Phase != "command" && Phase != "serialize" && Phase != "bytes"
                && Phase != "hash" && Phase != "transfer") return false;
            error_ = "rhwp-edit-renderer-failure" + (Phase.isEmpty() ? QString{} : ':' + Phase); return true;
        }
        if (Type != "ExportMessage" || !exporting_ || !message_.isEmpty() || Object.size() != 5
            || !Object["message"].isObject()) return false;
        const auto Message = Object["message"].toObject();
        if (Message["version"] != QJsonValue(1) || Message["identity"] != command_["identity"]) return false;
        const auto Identity = command_["identity"].toObject();
        if (Identity["changeSequence"] != QString::number(model_.changeSequence_)
            || Identity["savepoint"].toString().toStdString() != model_.savepoint_) {
            error_ = "rhwp-edit-export-stale"; return false;
        }
        const auto Kind = Message["type"].toString();
        if (Kind == "ExportBegin" && !exportBegun_ && Message.size() == 7) {
            const auto Size = Message["byteLength"].toDouble(-1);
            const auto Sha = Message["sha256"].toString().toLatin1();
            const auto Loss = Message["loss"].toObject();
            if (!Message["byteLength"].isDouble() || Size < 1 || Size > 64 * 1024 * 1024
                || !std::isfinite(Size) || std::floor(Size) != Size || Sha.size() != 64
                || QByteArray::fromHex(Sha).toHex() != Sha || Message["format"] != command_["format"]
                || Loss.size() != 4 || Loss["schemaVersion"] != QJsonValue(1)
                || Loss["outputFormat"] != command_["format"] || Loss["count"] != QJsonValue(0)
                || !Loss["losses"].isArray() || !Loss["losses"].toArray().isEmpty()) {
                error_ = "rhwp-edit-export-metadata-or-loss"; return false;
            }
            exportLength_ = quint64(Size); exportBegun_ = true;
        } else if (Kind == "ExportChunk" && exportBegun_ && Message.size() == 5) {
            const auto Encoded = Message["data"].toString().toLatin1();
            const auto Bytes = QByteArray::fromBase64(Encoded, QByteArray::AbortOnBase64DecodingErrors);
            if (!Message["offset"].isDouble() || Message["offset"].toDouble(-1) != double(exportOffset_)
                || Encoded.size() > 256 * 1024 || Bytes.isEmpty() || Bytes.toBase64() != Encoded
                || Message["data"].toString() != QString::fromLatin1(Encoded)
                || quint64(Bytes.size()) > exportLength_ - exportOffset_) return false;
            exportOffset_ += Bytes.size();
        } else if (Kind != "ExportCommit" || !exportBegun_ || Message.size() != 3
            || exportOffset_ != exportLength_) return false;
        // A single bounded chunk waits for the native consumer. Renderer retries
        // after false; it can never queue a whole 64 MiB export in WebChannel.
        message_ = Message; return true;
    }
}
