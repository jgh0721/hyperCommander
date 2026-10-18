#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QTimer>
#include <functional>
#include "commonLibs/document/documentHostProtocol.hpp"

namespace hc::document
{
    // GUI-thread transport for a dedicated web helper. Handler must return
    // promptly; asynchronous document work is represented by typed job tokens.
    class DocumentWebPeer final : public QObject
    {
    public:
        using Handler = std::function<QByteArray(const QByteArray&)>;
        explicit DocumentWebPeer(QObject* Parent = nullptr);
        ~DocumentWebPeer() override;
        bool Connect(const QString& Pipe, const QByteArray& ConnectionHex,
            std::uint32_t OwnerPid, std::uintptr_t ParentWindow);
        bool Ready(std::uintptr_t ChildWindow, Handler Request, std::function<void()> Disconnected);
    private:
        void receive();
        void fail();
        QLocalSocket socket_;
        QTimer ownerTimer_;
        QByteArray buffered_;
        std::array<unsigned char,16> connection_{};
        std::uintptr_t parentWindow_ = 0;
        void* owner_ = nullptr;
        std::uint64_t lastRequest_ = 0;
        bool connected_ = false, ready_ = false, failed_ = false;
        Handler request_;
        std::function<void()> disconnected_;
    };
}
