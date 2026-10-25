#include "stdafx.h"
#include "epubInputSender.hpp"

/// Platform/framework headers
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>

namespace hc::document::epub
{
    InputSendResult SendInput(const QByteArray& Publication, const QString& DocumentId, qint64 Generation,
        const QString& Engine, const InputExchange& Exchange, const std::function<bool()>& Cancelled, int TimeoutMs)
    {
        constexpr qsizetype MaxChunkBytes = 512 * 1024, MaxBytes = 48 * 1024 * 1024;
        qsizetype ChunkBytes = 192 * 1024; // Older helpers retain the original transfer size.
        const auto Id = DocumentId.toLatin1();
        if(!Exchange || Publication.isEmpty() || Publication.size() > MaxBytes || DocumentId.size() != 64
            || QString::fromLatin1(Id) != DocumentId || QByteArray::fromHex(Id).toHex() != Id
            || Generation < 1 || Generation > 9007199254740991LL || (Engine != "epubjs" && Engine != "readium")
            || TimeoutMs < 1 || TimeoutMs > 120000) return {false, false, "epub-send-admission"};
        QDeadlineTimer Deadline(TimeoutMs, Qt::PreciseTimer);
        bool Attempted = false;
        QJsonObject Identity{{"protocolVersion", 1}, {"documentId", DocumentId}, {"generation", Generation}};
        const auto Stop = [&]() -> QString {
            if(Cancelled && Cancelled()) return "epub-send-cancelled";
            return Deadline.hasExpired() ? QString("epub-send-timeout") : QString();
        };
        const auto Fail = [&](const QString& Error) -> InputSendResult {
            if(Attempted && Error == "epub-send-cancelled" && !Deadline.hasExpired()) {
                auto Cancel = Identity; Cancel["kind"] = "input-cancel"; QByteArray Reply;
                Exchange(QJsonDocument(Cancel).toJson(QJsonDocument::Compact), Reply, int(qBound(qint64(1), Deadline.remainingTime(), qint64(1000))));
            }
            return {false, Attempted, Error};
        };
        const auto Send = [&](const QJsonObject& Request, const char* Status, qsizetype Received) -> QString {
            if(const auto Error = Stop(); !Error.isEmpty()) return Error;
            QByteArray Reply; Attempted = true;
            if(!Exchange(QJsonDocument(Request).toJson(QJsonDocument::Compact), Reply,
                int(qBound(qint64(1), Deadline.remainingTime(), qint64(5000))))) return "epub-send-transport";
            if(const auto Error = Stop(); !Error.isEmpty()) return Error;
            if(Reply.size() > 4096) return "epub-send-reply-budget";
            QJsonParseError Parse;
            const auto Document = QJsonDocument::fromJson(Reply, &Parse);
            if(Parse.error != QJsonParseError::NoError || !Document.isObject()) return "epub-send-reply-schema";
            const auto Value = Document.object();
            if(Value["protocolVersion"] != QJsonValue(1) || Value["documentId"] != DocumentId
                || Value["generation"] != QJsonValue(Generation)) return "epub-send-reply-identity";
            const bool ExpectedStatus = Value["status"] == QJsonValue(Status)
                || (Request["kind"] == "input-commit" && Value["status"] == "preparing");
            if(Value.contains("error") || Value["accepted"] != QJsonValue(true)
                || !ExpectedStatus || Value["received"] != QJsonValue(Received)) return "epub-send-reply-state";
            if(Request["kind"]=="input-begin" && Value.contains("chunkBytes")) {
                const auto Limit=Value["chunkBytes"].toInteger(-1);
                if(!Value["chunkBytes"].isDouble() || Value["chunkBytes"].toDouble()!=double(Limit) || Limit<192*1024 || Limit>MaxChunkBytes)
                    return "epub-send-chunk-budget";
                ChunkBytes=qsizetype(Limit);
            }
            return {};
        };
        if(const auto Error = Stop(); !Error.isEmpty()) return Fail(Error);
        auto Begin = Identity; Begin["kind"] = "input-begin"; Begin["byteLength"] = Publication.size();
        Begin["readOnly"] = true; Begin["engine"] = Engine;
        Begin["sha256"] = QString::fromLatin1(QCryptographicHash::hash(Publication, QCryptographicHash::Sha256).toHex());
        if(const auto Error = Send(Begin, "receiving", 0); !Error.isEmpty()) return Fail(Error);
        for(qsizetype Offset = 0; Offset < Publication.size(); Offset += ChunkBytes) {
            const auto Chunk = Publication.mid(Offset, ChunkBytes);
            auto Request = Identity; Request["kind"] = "input-chunk"; Request["offset"] = Offset;
            Request["data"] = QString::fromLatin1(Chunk.toBase64());
            if(const auto Error = Send(Request, "receiving", Offset + Chunk.size()); !Error.isEmpty()) return Fail(Error);
        }
        auto Commit = Identity; Commit["kind"] = "input-commit";
        if(const auto Error = Send(Commit, "verified", Publication.size()); !Error.isEmpty()) return Fail(Error);
        return {true, false, {}};
    }
}
