#include "wcxHostService.hpp"
#include "UniqueLibs/wcxBackend.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

namespace hc::plugins
{
    using namespace storage;
    namespace { Error Invalid(const char* Code = "host-archive-payload") { return {ErrorCategory::InvalidRequest, Code, {"wcx"}}; } }
    WcxHostService::WcxHostService(std::string Binary, bool Write, std::string WriteFailure) : backend_(MakeInProcessWcxBackend(std::move(Binary), false)), write_(Write), writeFailure_(std::move(WriteFailure)) {}
    Result<QJsonObject> WcxHostService::Handle(WireKind Kind, const QJsonObject& Request)
    {
        if (!Request["context"].isObject()) return Invalid(); auto Context = DecodeContext(Request["context"].toObject()); if (!Context.HasValue()) return Context.Failure();
        if (const auto Stop = Context.Value().Check({"wcx"})) return *Stop;
        if (Kind == WireKind::ArchiveOpen)
        {
            if (Request.size() != 3 || !Request["archive"].isString() || !Request["test"].isBool()) return Invalid();
            if (listings_.size() >= 16) return Error{ErrorCategory::BudgetExceeded, "host-archive-cursor-budget", {"wcx"}};
            auto Listing = backend_->Enumerate(Request["archive"].toString().toStdString(), Request["test"].toBool(), Context.Value()); if (!Listing.HasValue()) return Listing.Failure();
            const auto Id = ++serial_; listings_[Id] = std::move(Listing.Value()); return QJsonObject{{"cursor", QString::number(Id)}};
        }
        if (Kind == WireKind::ArchiveNext)
        {
            auto Id = WireUnsigned(Request["cursor"]), Count = WireUnsigned(Request["count"]);
            if (Request.size() != 3 || !Id.HasValue() || !Count.HasValue() || !Count.Value() || Count.Value() > 128 || !listings_.contains(Id.Value())) return Invalid();
            auto Page = listings_.at(Id.Value())->Next(Count.Value(), Context.Value());
            if (!Page.HasValue()) { listings_.erase(Id.Value()); return Page.Failure(); }
            QJsonArray Entries; for (const auto& Entry : Page.Value().entries_) Entries.append(EncodeArchiveHeader(Entry));
            if (Page.Value().end_) listings_.erase(Id.Value());
            return QJsonObject{{"entries", Entries}, {"end", Page.Value().end_}};
        }
        if (Kind == WireKind::ArchiveRelease)
        {
            if (Request.size() != 2) return Invalid();
            const auto Key = Request.contains("cursor") ? "cursor" : "upload"; auto Id = WireUnsigned(Request[Key]); if (!Id.HasValue()) return Id.Failure();
            if (Request.contains("cursor")) listings_.erase(Id.Value());
            else if (Id.Value() == upload_) { upload_ = 0; data_.clear(); hash_.clear(); }
            return QJsonObject{{"released", true}};
        }
        if (Kind == WireKind::ArchiveExtract)
        {
            if (Request.size() != 4 || !Request["archive"].isString() || !Request["output"].isString() || !Request["expected"].isObject()) return Invalid();
            auto Expected = DecodeArchiveHeader(Request["expected"].toObject()); if (!Expected.HasValue()) return Expected.Failure();
            auto Extracted = backend_->Extract(Request["archive"].toString().toStdString(), Expected.Value(), Request["output"].toString().toStdString(), Context.Value());
            if (!Extracted.HasValue()) return Extracted.Failure(); return QJsonObject{{"complete", true}};
        }
        if (!write_) return Error{ErrorCategory::AccessDenied, writeFailure_, {"wcx"}};
        if (Kind == WireKind::ArchiveUpdateBegin)
        {
            auto Size = WireUnsigned(Request["size"]);
            if (Request.size() != 3 || !Size.HasValue() || !Size.Value() || Size.Value() > 128ULL * 1024 * 1024 || !Request["sha256"].isString() || Request["sha256"].toString().size() != 64) return Invalid();
            if (upload_) return Error{ErrorCategory::Unavailable, "host-archive-update-busy", {"wcx"}, {}, true};
            upload_ = ++serial_; uploadSize_ = Size.Value(); hash_ = Request["sha256"].toString().toLatin1(); data_.clear();
            return QJsonObject{{"upload", QString::number(upload_)}};
        }
        auto Id = WireUnsigned(Request["upload"]); if (!Id.HasValue() || !upload_ || Id.Value() != upload_) return Invalid();
        if (Kind == WireKind::ArchiveUpdateData)
        {
            auto Offset = WireUnsigned(Request["offset"]);
            if (Request.size() != 4 || !Offset.HasValue() || Offset.Value() != std::uint64_t(data_.size()) || !Request["data"].isString() || Request["data"].toString().size() > 65536) return Invalid();
            const auto Encoded = Request["data"].toString().toLatin1(); const auto Bytes = QByteArray::fromBase64(Encoded, QByteArray::AbortOnBase64DecodingErrors);
            if (Bytes.isEmpty() || Bytes.toBase64() != Encoded || std::uint64_t(data_.size() + Bytes.size()) > uploadSize_) return Invalid();
            data_.append(Bytes); return QJsonObject{{"received", QString::number(data_.size())}};
        }
        if (Kind != WireKind::ArchiveUpdateApply || Request.size() != 2) return Invalid();
        upload_ = 0; auto Bytes = std::exchange(data_, {});
        if (std::uint64_t(Bytes.size()) != uploadSize_ || QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != hash_) return Invalid("host-archive-update-integrity");
        QJsonParseError Error; const auto Document = QJsonDocument::fromJson(Bytes, &Error);
        if (Error.error != QJsonParseError::NoError || !Document.isObject()) return Invalid();
        auto Update = DecodeArchiveUpdate(Document.object()); if (!Update.HasValue()) return Update.Failure();
        auto Applied = backend_->Update(std::move(Update.Value()), Context.Value()); if (!Applied.HasValue()) return Applied.Failure();
        return QJsonObject{{"complete", true}};
    }
}
