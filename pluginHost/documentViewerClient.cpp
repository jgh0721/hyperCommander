#include "documentViewerClient.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <thread>
namespace hc::plugins
{
    using namespace storage;
    Result<QJsonObject> DocumentViewerClient::Call(QString Type, QJsonObject Payload, QString Document, QString Generation, OperationContext Context)
    {
        const auto Id = QString::number(++sequence_);
        QJsonObject Envelope{{"protocolVersion", int(document::HostVersion)}, {"requestId", Id}, {"workspaceId", workspace_},
            {"documentId", Document}, {"generation", Generation}, {"messageType", Type},
            {"payloadLength", QJsonDocument(Payload).toJson(QJsonDocument::Compact).size()}, {"payload", Payload}};
        auto Reply = host_.ViewerCall(WireKind::DocumentCommand, Envelope, Context);
        if (!Reply.HasValue()) return Reply.Failure();
        const auto& Message = Reply.Value();
        if (Message["protocolVersion"].toInt() != document::HostVersion || Message["requestId"].toString() != Id
            || !Message["payload"].isObject() || Message["payloadLength"].toInteger(-1) != QJsonDocument(Message["payload"].toObject()).toJson(QJsonDocument::Compact).size()
            || (!workspace_.isEmpty() && Message["workspaceId"].toString() != workspace_))
            return Error{ErrorCategory::Unavailable, "document-host-envelope"};
        const auto Body = Message["payload"].toObject();
        if (Message["messageType"].toString() == "Error") return Error{ErrorCategory::InvalidRequest, Body["code"].toString("document-host-error").toStdString()};
        if (Type == "Hello") {
            if (Message["messageType"].toString() != "Ready" || Message["workspaceId"].toString().size() != 32
                || !Body["capabilities"].toArray().contains("saveStream")) return Error{ErrorCategory::Unsupported, "document-host-capabilities"};
            workspace_ = Message["workspaceId"].toString();
        }
        if (Type == "OpenDocuments") for (const auto& Item : Body["documents"].toArray())
            if (Item.toObject().contains("error")) return Error{ErrorCategory::InvalidRequest, Item.toObject()["error"].toString().toStdString()};
        return Body;
    }
    Result<Unit> DocumentViewerClient::Hello(OperationContext Context)
    {
        const auto Deadline = std::min(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(10));
        do {
            if (auto Stop = Context.Check({"document"})) return *Stop;
            auto Ready = Call("Hello", {}, {}, {}, Context);
            if (Ready.HasValue()) return Unit{};
            if (Ready.Failure().code_ != "document-host-envelope" && Ready.Failure().code_ != "host-document-not-ready") return Ready.Failure();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < Deadline);
        return Error{ErrorCategory::Unavailable, "document-helper-startup-timeout"};
    }
    QString DocumentViewerClient::Bind(EntrySnapshot Source, std::shared_ptr<IWritableStorage> Writer,
        std::shared_ptr<IMaterializedFileLease> Lease, QByteArray Hash)
    { return broker_.Bind(std::move(Source), std::move(Writer), std::move(Lease), std::move(Hash)); }
    Result<QJsonObject> DocumentViewerClient::Execute(QJsonObject Request, OperationContext Context)
    {
        const auto Id = Request.take("documentId").toString();
        auto Generation = Request.take("generation").toString();
        if (!Id.isEmpty() && Generation.isEmpty()) for (const auto& Item : state_["documents"].toArray())
            if (Item.toObject()["documentId"].toString() == Id) Generation = Item.toObject()["generation"].toString();
        auto Result = Call("Execute", std::move(Request), Id, Generation, Context);
        if (Result.HasValue() && Result.Value().contains("error")) return Error{ErrorCategory::InvalidRequest, Result.Value()["error"].toString().toStdString()};
        return Result;
    }
    Result<QJsonObject> DocumentViewerClient::Poll(OperationContext Context)
    {
        auto Current = Call("DocumentState", {}, {}, {}, Context); if (!Current.HasValue()) return Current.Failure();
        state_ = Current.Value();
        std::set<QString> Tokens;
        for (const auto& Item : state_["documents"].toArray()) {
            const auto Token = Item.toObject()["sourceToken"].toString(); if (!Token.isEmpty()) Tokens.insert(Token);
        }
        broker_.Retain(Tokens);
        for (const auto& Item : state_["pendingSaves"].toArray()) {
            const auto Prepared = Item.toObject(); bool Valid = false;
            const auto Length = Prepared["byteLength"].toString().toULongLong(&Valid);
            const auto Id = Prepared["documentId"].toString(), Generation = Prepared["generation"].toString();
            QByteArray Bytes; std::optional<Error> Failure;
            if (!Valid || Length > document::DocumentByteLimit) Failure = Error{ErrorCategory::BudgetExceeded, "document-save-stream-length"};
            while (!Failure && quint64(Bytes.size()) < Length) {
                auto Chunk = Call("SaveRead", {{"saveToken", Prepared["saveToken"]}, {"offset", QString::number(Bytes.size())},
                    {"count", int(document::HostChunkLimit)}}, Id, Generation, Context);
                if (!Chunk.HasValue()) { Failure = Chunk.Failure(); break; }
                const auto Encoded = Chunk.Value()["bytes"].toString().toLatin1();
                if (Encoded.size() > ((document::HostChunkLimit + 2) / 3) * 4) { Failure = Error{ErrorCategory::BudgetExceeded, "document-save-stream-chunk"}; break; }
                const auto Decoded = QByteArray::fromBase64(Encoded, QByteArray::AbortOnBase64DecodingErrors);
                if (Decoded.isEmpty() || Decoded.toBase64() != Encoded || quint64(Decoded.size()) > Length - quint64(Bytes.size())
                    || Chunk.Value()["offset"].toString() != QString::number(Bytes.size())
                    || Chunk.Value()["eof"].toBool() != (quint64(Bytes.size() + Decoded.size()) == Length)) {
                    Failure = Error{ErrorCategory::InvalidRequest, "document-save-stream-corrupt"}; break;
                }
                Bytes += Decoded;
            }
            QJsonObject Result;
            if (Failure) Result = {{"action", "commitSave"}, {"documentId", Id}, {"saveToken", Prepared["saveToken"]},
                {"revision", Prepared["revision"]}, {"committed", false}, {"error", QString::fromStdString(Failure->code_)}, {"errorCategory", int(Failure->category_)}};
            else Result = broker_.CommitBytes(Prepared, Bytes, Context);
            auto Completed = Call("SaveResult", Result, Id, Generation, Context); if (!Completed.HasValue()) return Completed.Failure();
        }
        return state_;
    }
}
