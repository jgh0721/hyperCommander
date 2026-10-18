#pragma once
#include "hwpOpenWindow.hpp"
#include <QJsonObject>
#include <QPointer>
#include <functional>
namespace hc::document
{
    // Authenticated host transport calls this GUI-thread SDI owner. Protocol
    // document IDs never convey write authority or expose materialized paths.
    class HwpHostSession final : public QObject
    {
    public:
        HwpHostSession(QString CacheRoot, WebProcessRuntime Runtime,
            HwpOpenWindow::BackendFactory Factory = {}, QObject* Parent = nullptr);
        ~HwpHostSession() override;
        QJsonObject Handle(const QJsonObject& Request);
        // Call only after transport authentication; NativeDispatch validates
        // legacy requests before their result can cancel the RHWP selection.
        QJsonObject Dispatch(const QJsonObject& Request,
            const std::function<QJsonObject(const QJsonObject&)>& NativeDispatch);
        QJsonObject OpenHost(const QString& Path);
        void CancelHost();
        void Shutdown();
        bool IsIdle() const { return !window_ && pendingPath_.isEmpty(); }
    private:
        QJsonObject snapshot() const;
        QString cacheRoot_, documentId_, terminalError_, pendingPath_;
        WebProcessRuntime runtime_;
        HwpOpenWindow::BackendFactory factory_;
        QPointer<HwpOpenWindow> window_;
        bool closing_ = false, stopping_ = false;
    };
}
