#pragma once
#include "commonLibs/document/documentTypes.hpp"
#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <QVariantMap>
#include <QDeadlineTimer>
#include <optional>
namespace hc::document
{
    class HwpEditBridge;
    // Only this facade enters WebChannel. Native grants and storage paths are
    // deliberately absent from its invokable surface.
    class HwpEditEndpoint final : public QObject
    {
        Q_OBJECT
    public:
        HwpEditEndpoint(HwpEditBridge* Bridge, QObject* Parent);
        Q_INVOKABLE QVariantMap State() const;
        Q_INVOKABLE QVariantMap PendingCommand() const;
        Q_INVOKABLE bool Report(const QString& Json);
    private:
        QPointer<HwpEditBridge> bridge_;
    };
    class HwpEditBridge final : public QObject
    {
        Q_OBJECT
    public:
        explicit HwpEditBridge(QObject* Parent = nullptr);
        bool Activate(const DocumentOperationTicket& Load, quint64 BindingOwner,
            quint64 BindingGeneration, const QByteArray& InputSha256, bool NativeEditAllowed);
        void Retire();
        void DiscardExport();
        bool Synchronize(const QString& Id);
        bool Resume();
        QString TakeUserAction();
        QJsonObject State() const;
        QJsonObject PendingCommand() const;
        bool Report(const QString& Json);
        bool BeginExport(const QJsonObject& Identity, const QString& Format, int TimeoutMs = 30000);
        std::optional<QJsonObject> TakeExportMessage();
        bool NotifySaved(const ExternalModelRevision& Model);
        ExternalModelRevision Model() const { return model_; }
        QString Error() const;
    signals:
        void ModelChanged();
    private:
        bool sameBinding(const QJsonObject& Identity) const;
        QJsonObject binding_, command_, message_;
        ExternalModelRevision model_;
        QDeadlineTimer deadline_;
        quint64 exportOffset_ = 0, exportLength_ = 0;
        QString error_, userAction_, synchronized_;
        bool retiring_ = false, resuming_ = false;
        bool active_ = false, used_ = false, exporting_ = false, exportBegun_ = false;
    };
}
