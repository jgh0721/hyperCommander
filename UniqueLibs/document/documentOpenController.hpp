#pragma once
#include "commonLibs/document/documentHandlerResolver.hpp"
#include "commonLibs/storage/materializedContent.hpp"
#include "pluginHost/hostedViewerSession.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include <QPointer>
#include <QWidget>
#include <QJsonObject>
#include <functional>
namespace hc::document {
struct DocumentHandlerTarget {
    HandlerDescriptor descriptor_;
    QString executable_, arguments_;
    hc::plugins::PluginViewerOptions plugin_;
    // EXE handlers do not transfer authority to a materialized copy.
    bool productEditor_ = false;
    bool rhwpEditor_ = false;
};
struct DocumentHandlerCatalog {
    DocumentHandlerRegistry registry_;
    std::map<std::string, DocumentHandlerTarget> targets_;
    std::string defaultView_, defaultEdit_;
    QJsonObject overrides_;
};
DocumentHandlerCatalog BuildDocumentHandlerCatalog(hc::settings::Repository& Settings, const QString& Directory, std::vector<DocumentHandlerTarget> Plugins = {});
DocumentHandlerTarget BuildPluginViewerTarget(const QString& Name, hc::plugins::PluginViewerOptions Options,
    const QStringList& RegisteredExtensions);
struct DocumentOpenRequest {
    DocumentIntent intent_ = DocumentIntent::View;
    std::vector<hc::plugins::HostedViewerSession::DocumentInput> documents_;
    std::function<bool()> stillCurrent_;
};
class DocumentOpenController final : public QObject {
    Q_OBJECT
public:
    using Catalog = std::function<DocumentHandlerCatalog()>;
    using Completion = std::function<void(bool, const QString&)>;
    using PluginLaunch = std::function<bool(const DocumentHandlerTarget&, std::vector<hc::plugins::HostedViewerSession::DocumentInput>, Completion)>;
    DocumentOpenController(QWidget* Main, Catalog Candidates, PluginLaunch Launch);
    ~DocumentOpenController() override;
    QString Capture(DocumentOpenRequest Request);
    bool Open(const QString& Token);
    void Cancel(const QString& Token);
    bool HasRequest(const QString& Token) const { return requests_.contains(Token); }
    static QStringList Arguments(const QString& Template, const QStringList& Paths);
signals:
    void ConfigureRequested();
    void Finished(const QString& Token, const QString& Status, const QString& Reason);
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override;
private:
    void prepare(const QString& Token, DocumentOpenRequest Request, DocumentHandlerTarget Target, HandlerSelection Selection);
    void report(const QString& Token, const QString& Status, const QString& Reason = {});
    QPointer<QWidget> main_;
    Catalog catalog_;
    PluginLaunch launch_;
    QHash<QString, DocumentOpenRequest> requests_;
    QSet<QString> cancelled_;
    QHash<QString, std::shared_ptr<std::stop_source>> pending_;
    int leasedProcesses_ = 0;
};
}
