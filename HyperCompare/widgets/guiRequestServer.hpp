#pragma once
#include <QObject>
#include <QLocalServer>
#include <QPointer>
#include <QVector>

namespace hc::compare
{
    class CompareWindow;
    class GuiRequestServer final : public QObject
    {
    public:
        explicit GuiRequestServer(QObject* Parent = nullptr);
        ~GuiRequestServer() override;
        bool Listen(const QString& Endpoint = {});
    private:
        void accept();
        QLocalServer server_;
        QVector<QPointer<CompareWindow>> windows_;
        int connections_ = 0, parsing_ = 0;
        void* lease_ = nullptr;
    };
}
