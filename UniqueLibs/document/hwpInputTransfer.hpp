#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QDeadlineTimer>
#include <QCryptographicHash>
#include <optional>
namespace hc::document
{
    struct HwpTransferredInput
    {
        QJsonObject identity_;
        QString format_;
        QByteArray bytes_, sha256_;
    };
    // Single native-authenticated pipe operation. Not a renderer endpoint and
    // never a file opener. Package approval comes from the native helper policy.
    class HwpInputTransfer final
    {
    public:
        explicit HwpInputTransfer(bool PackageApproved, int TimeoutMs = 30000);
        QJsonObject Handle(const QJsonObject& Request);
        void Expire();
        std::optional<HwpTransferredInput> Take();
    private:
        enum class State { Idle, Receiving, Verified, Consumed, Discarded, Failed };
        QJsonObject reject(const char* Code) const;
        QJsonObject reply(const char* Type) const;
        QJsonObject fail(const char* Code);
        State state_ = State::Idle;
        bool approved_;
        int timeoutMs_, length_ = 0;
        QDeadlineTimer deadline_;
        QCryptographicHash hash_{QCryptographicHash::Sha256};
        HwpTransferredInput input_;
    };
}
