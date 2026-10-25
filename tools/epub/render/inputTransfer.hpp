#pragma once
#include <QByteArray>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <optional>

namespace hc::epub::poc
{
    struct TransferredPublication
    {
        QString documentId, engine;
        qint64 generation = 0;
        QByteArray bytes;
    };
    // One publication per authenticated native helper. Never accepts filesystem paths.
    class InputTransfer final
    {
    public:
        static constexpr qsizetype MaxBytes = 48 * 1024 * 1024;
        static constexpr qsizetype ChunkBytes = 512 * 1024;
        explicit InputTransfer(int TimeoutMs = 30000);
        QJsonObject Handle(const QJsonObject& Request);
        QJsonObject Status();
        bool Matches(const QJsonObject& Request) const;
        void Expire();
        std::optional<TransferredPublication> Take();
    private:
        QJsonObject fail(const char* Code);
        enum class State { Idle, Receiving, Verified, Consumed, Cancelled, Failed };
        State state_ = State::Idle;
        int timeoutMs_;
        qsizetype length_ = 0, received_ = 0;
        QString failure_;
        QByteArray sha256_;
        QDeadlineTimer deadline_;
        QCryptographicHash hash_{QCryptographicHash::Sha256};
        TransferredPublication input_;
    };
}
