#pragma once
#include <QObject>
#include <QPointer>
#include <QModelIndex>
#include <QVariantMap>
#include "commonLibs/commands/commandBus.hpp"

class QWidget;
class CmpPanel;
class CFSModel;

std::shared_ptr<hc::commands::IExecutor> MakeQtExecutor(QObject* Owner);

class CommandUiBridge : public QObject
{
public:
    explicit CommandUiBridge(QWidget* Main);
    hc::storage::Task<hc::commands::CommandOutcome> Dispatch(const QString& Id, const QVariantMap& Parameters,
        const QString& Source, const QModelIndex& Index = {});
    hc::storage::Task<hc::commands::CommandOutcome> DispatchInvocation(hc::commands::CommandInvocation Invocation);
    hc::storage::CommandContext Capture(const QModelIndex& Index = {});
    bool DispatchFolder(const QModelIndex& Index);
    std::shared_ptr<hc::commands::EventHub> Events() const { return events_; }
private:
    void bindModels();
    void createActions();
    CFSModel* findModel(const std::string& TabId) const;
    hc::storage::Result<hc::commands::CommandOutcome> execute(const hc::commands::CommandInvocation& Invocation);
    QPointer<QWidget> main_;
    QPointer<CmpPanel> activePanel_;
    std::shared_ptr<hc::commands::IExecutor> executor_;
    std::shared_ptr<hc::commands::CommandRegistry> registry_;
    std::shared_ptr<hc::commands::EventHub> events_;
    std::unique_ptr<hc::commands::CommandDispatcher> dispatcher_;
    hc::commands::EventSubscription errorSubscription_;
};
