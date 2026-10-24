#include "stdafx.h"
#include "inputTransfer.hpp"

/// Platform/framework headers
#include <cmath>
#include <utility>

namespace hc::epub::poc
{
    namespace
    {
        bool hashValid(const QString& Text)
        { const auto Bytes = Text.toLatin1(); return Text.size() == 64 && QString::fromLatin1(Bytes) == Text && QByteArray::fromHex(Bytes).toHex() == Bytes; }
        QJsonObject reject(const char* Code) { return {{"protocolVersion", 1}, {"error", Code}}; }
    }
    InputTransfer::InputTransfer(int TimeoutMs) : timeoutMs_(TimeoutMs) {}
    bool InputTransfer::Matches(const QJsonObject& Request) const
    {
        return state_ != State::Idle && Request["protocolVersion"] == QJsonValue(1)
            && Request["documentId"] == input_.documentId && Request["generation"] == QJsonValue(input_.generation);
    }
    QJsonObject InputTransfer::fail(const char* Code)
    { state_ = State::Failed; failure_ = QString::fromLatin1(Code); input_.bytes = {}; hash_.reset(); return reject(Code); }
    void InputTransfer::Expire()
    {
        if((state_ == State::Receiving || state_ == State::Verified) && deadline_.hasExpired()) fail("epub-input-timeout");
    }
    QJsonObject InputTransfer::Status()
    {
        Expire();
        const char* Names[]{"idle", "receiving", "verified", "consumed", "cancelled", "failed"};
        QJsonObject Reply{{"protocolVersion", 1}, {"documentId", input_.documentId}, {"generation", input_.generation},
            {"status", Names[int(state_)]}, {"received", received_}};
        if(!failure_.isEmpty()) Reply["error"] = failure_;
        return Reply;
    }
    QJsonObject InputTransfer::Handle(const QJsonObject& Request)
    {
        Expire();
        if(Request["protocolVersion"] != QJsonValue(1) || !Request["kind"].isString()) return reject("epub-input-schema");
        const auto Kind = Request["kind"].toString();
        if(Kind == "input-begin") {
            if(state_ != State::Idle) return reject("epub-input-state");
            const auto Size = Request["byteLength"].toDouble(-1), Generation = Request["generation"].toDouble(-1);
            const auto Engine = Request["engine"].toString();
            if(Request.size() != 8 || !hashValid(Request["documentId"].toString()) || !hashValid(Request["sha256"].toString())
                || !Request["byteLength"].isDouble() || Size < 1 || Size > MaxBytes || std::floor(Size) != Size
                || !Request["generation"].isDouble() || Generation < 1 || Generation > 9007199254740991.0 || std::floor(Generation) != Generation
                || (Engine != "epubjs" && Engine != "readium") || Request["readOnly"] != QJsonValue(true)
                || timeoutMs_ < 1 || timeoutMs_ > 120000) return reject("epub-input-admission");
            input_.documentId = Request["documentId"].toString(); input_.generation = qint64(Generation); input_.engine = Engine;
            sha256_ = Request["sha256"].toString().toLatin1(); length_ = qsizetype(Size);
            deadline_ = QDeadlineTimer(timeoutMs_, Qt::PreciseTimer); state_ = State::Receiving;
        } else {
            if(!Matches(Request)) return reject("epub-input-stale");
            if(Kind == "input-status" && Request.size() == 4) return Status();
            if(Kind == "input-cancel" && Request.size() == 4 && (state_ == State::Receiving || state_ == State::Verified)) {
                state_ = State::Cancelled; input_.bytes = {}; hash_.reset();
            } else if(state_ != State::Receiving) return reject("epub-input-state");
            else if(Kind == "input-chunk") {
                if(Request.size() != 6 || !Request["offset"].isDouble() || Request["offset"].toDouble(-1) != received_
                    || !Request["data"].isString()) return fail("epub-input-order");
                const auto Text = Request["data"].toString();
                if(Text.isEmpty() || Text.size() > ChunkBytes * 4 / 3) return fail("epub-input-chunk-budget");
                const auto Encoded = Text.toLatin1();
                const auto Data = QByteArray::fromBase64(Encoded, QByteArray::AbortOnBase64DecodingErrors);
                if(Data.isEmpty() || Data.size() > length_ - received_ || Data.toBase64() != Encoded || QString::fromLatin1(Encoded) != Text)
                    return fail("epub-input-chunk");
                input_.bytes.append(Data); hash_.addData(Data); received_ += Data.size();
            } else if(Kind == "input-commit" && Request.size() == 4) {
                if(received_ != length_ || hash_.result().toHex() != sha256_) return fail("epub-input-integrity");
                state_ = State::Verified;
            } else return reject("epub-input-schema");
        }
        auto Reply = Status(); Reply["accepted"] = true; return Reply;
    }
    std::optional<TransferredPublication> InputTransfer::Take()
    {
        Expire(); if(state_ != State::Verified) return {};
        state_ = State::Consumed;
        // Retain identity for stale-request rejection after handing bytes to the parser.
        return TransferredPublication{input_.documentId, input_.engine, input_.generation, std::exchange(input_.bytes, {})};
    }
}
