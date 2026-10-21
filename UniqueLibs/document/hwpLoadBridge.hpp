#pragma once
#include <QObject>
#include <QJsonObject>
#include <QVariantMap>
#include <QPointer>
#include <QDeadlineTimer>
#include <QTimer>
#include <QSet>

namespace hc::document
{
    struct HwpNativeLoad
    {
        QString documentId_;
        quint64 documentGeneration_, bindingOwner_, bindingGeneration_, epoch_;
        QString format_;
        QByteArray sha256_;
    };

    class HwpLoadBridge;
    // Publish only this facade. Native completion signals must never enter the
    // remote metaobject: a raw WebChannel invocation must not forge Ready/Cancelled.
    class HwpLoadEndpoint final : public QObject
    {
        Q_OBJECT
    public:
        HwpLoadEndpoint(HwpLoadBridge* Bridge, QObject* Parent);
        Q_INVOKABLE QVariantMap PendingOpen() const;
        Q_INVOKABLE QString ReadChunk(const QString& Operation, qint64 Offset) const;
        Q_INVOKABLE bool Report(const QString& Json);
    signals:
        void CancelRequested();
    private:
        QPointer<HwpLoadBridge> bridge_;
    };

    // One-shot, GUI-thread bridge. Only Begin admits native-owned input bytes.
    // Renderer notifications do not prove worker retirement or release source leases.
    class HwpLoadBridge final : public QObject
    {
        Q_OBJECT
    public:
        enum class State { Idle, Loading, Parsed, Ready, CancelPending, Failed, Retired };
        explicit HwpLoadBridge(QObject* Parent = nullptr);
        bool Begin(const HwpNativeLoad& Request, QByteArray Bytes, QString& Error, int TimeoutMs = 30000, bool DeferredPages = false);
        void RequestCancel();
        void FailLoad();
        void Retire();
        State CurrentState() const { return state_; }
        QVariantMap PendingOpen() const;
        QString ReadChunk(const QString& Operation, qint64 Offset) const;
        bool Report(const QString& Json);
    signals:
        // Native owner must retire the entire process tree before calling Retire.
        // This signal is not present on the remote endpoint.
        void RetirementRequested();
        void Ready();
        void Failed(const QString& Code);
        void ViewFailed(const QString& Code);
        void CancelRequested();
        void Cancelled();
    private:
        void expire();
        void fail(const QString& Code);
        void requestRetirement();
        void failView(const QString& Code);
        bool expired() const;
        QTimer timer_;
        QDeadlineTimer deadline_;
        bool retirementRequested_ = false;
        bool deferredPages_ = false, rendering_ = false;
        int renderTimeoutMs_ = 30000, pageCount_ = 0, activePage_ = -1;
        quint64 renderSequence_ = 0;
        QSet<int> rendered_;
        QDeadlineTimer renderDeadline_;
        State state_ = State::Idle;
        QJsonObject identity_;
        QByteArray bytes_;
        QByteArray sha256_;
    };
}
