#include "hwpInputTransfer.hpp"
#include <QJsonValue>
#include <cmath>
namespace hc::document
{
    namespace
    {
        bool IdentityValid(const QJsonObject& Value)
        {
            if(Value.size() != 6 || !Value["documentId"].isString()
                || Value["documentId"].toString().isEmpty() || Value["documentId"].toString().size() > 128) return false;
            for(const auto* Key : {"generation", "token", "bindingOwner", "bindingGeneration", "epoch"}) {
                const auto Item = Value[Key]; bool Ok = false;
                const auto Number = Item.toString().toULongLong(&Ok);
                if(!Item.isString() || !Ok || !Number || QString::number(Number) != Item.toString()) return false;
            }
            return true;
        }
    }
    HwpInputTransfer::HwpInputTransfer(bool PackageApproved, int TimeoutMs)
        : approved_(PackageApproved), timeoutMs_(TimeoutMs) {}
    QJsonObject HwpInputTransfer::reject(const char* Code) const
    { return {{"version", 1}, {"error", Code}}; }
    QJsonObject HwpInputTransfer::reply(const char* Type) const
    { return {{"version", 1}, {"type", Type}, {"identity", input_.identity_}, {"received", input_.bytes_.size()}}; }
    QJsonObject HwpInputTransfer::fail(const char* Code)
    { state_ = State::Failed; input_.bytes_.clear(); return reject(Code); }
    void HwpInputTransfer::Expire()
    {
        if((state_ == State::Receiving || state_ == State::Verified) && deadline_.hasExpired())
            fail("rhwp-input-timeout");
    }
    QJsonObject HwpInputTransfer::Handle(const QJsonObject& Request)
    {
        if(!approved_) return reject("rhwp-input-package-not-approved");
        Expire();
        if(Request["version"] != QJsonValue(1) || !Request["type"].isString()
            || !Request["identity"].isObject()) return reject("rhwp-input-schema");
        const auto Type = Request["type"].toString();
        const auto Identity = Request["identity"].toObject();
        if(Type == "InputBegin") {
            if(state_ != State::Idle) return reject("rhwp-input-state");
            const auto Size = Request["byteLength"].toDouble(-1);
            const auto Sha = Request["sha256"].toString().toLatin1();
            const auto Format = Request["format"].toString();
            if(Request.size() != 7 || !IdentityValid(Identity) || Request["readOnly"] != QJsonValue(true)
                || !Request["byteLength"].isDouble() || Size < 1 || Size > 9007199254740991.0
                || std::floor(Size) != Size || (Format != "hwp" && Format != "hwpx")
                || !Request["sha256"].isString() || Sha.size() != 64 || QByteArray::fromHex(Sha).toHex() != Sha
                || timeoutMs_ < 1 || timeoutMs_ > 120000) return reject("rhwp-input-admission");
            input_.identity_ = Identity; input_.format_ = Format; input_.sha256_ = Sha;
            length_ = static_cast<qsizetype>(Size); deadline_ = QDeadlineTimer(timeoutMs_, Qt::PreciseTimer);
            state_ = State::Receiving; return reply("InputAccepted");
        }
        if(Identity != input_.identity_) return reject("rhwp-input-stale");
        if(Type == "InputCancel" && Request.size() == 3
            && (state_ == State::Receiving || state_ == State::Verified)) {
            state_ = State::Discarded; input_.bytes_.clear();
            return reply("InputDiscarded"); // Byte discard only, never a Job retirement ACK.
        }
        if(state_ != State::Receiving) return reject("rhwp-input-state");
        if(Type == "InputChunk") {
            const auto Offset = Request["offset"];
            if(Request.size() != 5 || !Offset.isDouble() || Offset.toDouble(-1) != input_.bytes_.size()
                || !Request["data"].isString()) return fail("rhwp-input-chunk-order");
            const auto Text = Request["data"].toString();
            if(Text.isEmpty() || Text.size() > 256 * 1024) return fail("rhwp-input-chunk-budget");
            const auto Encoded = Text.toLatin1();
            const auto Bytes = QByteArray::fromBase64(Encoded, QByteArray::AbortOnBase64DecodingErrors);
            if(Bytes.isEmpty() || Bytes.size() > 192 * 1024 || Bytes.toBase64() != Encoded
                || QString::fromLatin1(Encoded) != Text || Bytes.size() > length_ - input_.bytes_.size())
                return fail("rhwp-input-chunk");
            hash_.addData(Bytes); input_.bytes_.append(Bytes); return reply("InputChunkAccepted");
        }
        if(Type == "InputCommit" && Request.size() == 3) {
            if(input_.bytes_.size() != length_ || hash_.result().toHex() != input_.sha256_)
                return fail("rhwp-input-integrity");
            state_ = State::Verified; return reply("InputVerified");
        }
        return reject("rhwp-input-schema");
    }
    std::optional<HwpTransferredInput> HwpInputTransfer::Take()
    {
        Expire();
        if(state_ != State::Verified) return {};
        state_ = State::Consumed; return std::move(input_);
    }
}
