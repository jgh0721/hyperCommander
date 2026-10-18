#pragma once
#include "hwpPrintSnapshot.hpp"
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QDeadlineTimer>
#include <QVariantMap>
#include <QPointer>
namespace hc::document {
    QJsonObject HwpPrintIdentity(const DocumentOperationTicket& Ticket, quint64 BindingGeneration = 1);
    // Native owner validates streamed page bytes before any output device sees them.
    class HwpPrintTransfer final {
    public:
        explicit HwpPrintTransfer(DocumentOperationTicket Ticket, std::vector<int> Pages = {}, int TimeoutMs = 30000, quint64 BindingGeneration = 1);
        bool Handle(const QJsonObject& Message);
        bool Ready() const { return ready_ && Error().isEmpty(); }
        QString Error() const;
        const HwpPrintFrames& Frames() const { return frames_; }
        const std::vector<int>& Pages() const { return pages_; }
    private:
        bool fail(const char* Code);
        HwpPrintFrames frames_;
        quint64 bindingGeneration_ = 1;
        std::vector<int> pages_;
        QDeadlineTimer deadline_;
        QByteArray pageBytes_, pageHash_;
        HwpPrintPage page_;
        qint64 pageLength_ = 0, bytes_ = 0;
        quint64 pixels_ = 0;
        bool begun_ = false, pageOpen_ = false, ready_ = false;
        QString error_;
    };
    // This facade has no filesystem, printer, destination or dialog authority.
    class HwpPrintBridge final : public QObject {
        Q_OBJECT
    public:
        explicit HwpPrintBridge(QObject* Parent = nullptr):QObject(Parent){}
        void Activate(QJsonObject Binding);
        bool Begin(QJsonObject Identity, QJsonArray Pages);
        void Cancel();
        bool Cancel(const QJsonObject& Identity);
        QJsonObject Retired() const { return retired_; }
        bool Idle() const { return command_.isEmpty(); }
        QJsonObject Pending() const { return command_; }
        QVariantMap State() const;
        bool Report(const QString& Text);
        bool TakeAction();
        QJsonObject TakeMessage();
    private:
        QJsonObject binding_, command_, message_, retired_;
        bool action_ = false;
    };
    class HwpPrintEndpoint final : public QObject {
        Q_OBJECT
    public:
        HwpPrintEndpoint(HwpPrintBridge* Bridge, QObject* Parent):QObject(Parent),bridge_(Bridge){}
        Q_INVOKABLE QVariantMap State() const { return bridge_ ? bridge_->State() : QVariantMap{}; }
        Q_INVOKABLE QVariantMap PendingCommand() const { return bridge_ ? bridge_->Pending().toVariantMap() : QVariantMap{}; }
        Q_INVOKABLE bool Report(const QString& Text) { return bridge_ && bridge_->Report(Text); }
    private:
        QPointer<HwpPrintBridge> bridge_;
    };
}
