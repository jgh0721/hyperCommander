#pragma once
#include "pluginHost/hostedViewerSession.hpp"
#include <QPointer>
#include <QWidget>

class QLabel;
class QTimer;
class QtPluginViewerController final : public QObject
{
    Q_OBJECT
public:
    QtPluginViewerController(QWidget* Main, hc::plugins::PluginViewerService::MaterializerFactory Factory);
    ~QtPluginViewerController() override;
    hc::storage::Result<std::shared_ptr<hc::plugins::HostedViewerSession>> Open(QWidget* Anchor,
        hc::plugins::PluginViewerOptions Options, std::shared_ptr<hc::storage::IContentSource> Source);
    hc::storage::Result<std::shared_ptr<hc::plugins::HostedViewerSession>> OpenAutomatic(QWidget* Anchor,
        std::vector<hc::plugins::PluginViewerOptions> Candidates, std::shared_ptr<hc::storage::IContentSource> Source,
        hc::plugins::PluginViewerSelectionPolicy Policy);
    void Retire(QWidget* Anchor, const std::shared_ptr<hc::plugins::HostedViewerSession>& Session);
    void CloseAll();
    bool IsIdle() const;
private:
    void collect();
    struct Record { QPointer<QWidget> anchor_; std::shared_ptr<hc::plugins::HostedViewerSession> session_; };
    std::vector<Record> records_;
    QPointer<QWidget> parking_;
    std::unique_ptr<hc::plugins::PluginViewerService> service_;
    QTimer* timer_ = nullptr;
};

class QtPluginViewerSurface final : public QWidget
{
    Q_OBJECT
public:
    explicit QtPluginViewerSurface(QtPluginViewerController* Controller, QWidget* Parent = nullptr);
    ~QtPluginViewerSurface() override;
    void Open(hc::plugins::PluginViewerOptions Options, std::shared_ptr<hc::storage::IContentSource> Source);
    void OpenAutomatic(std::vector<hc::plugins::PluginViewerOptions> Candidates,
        std::shared_ptr<hc::storage::IContentSource> Source, hc::plugins::PluginViewerSelectionPolicy Policy);
    hc::plugins::PluginViewerDetails Details() const;
    void Close();
    void Execute(const QString& Command);
    void Search(const QString& Text, bool Backward);
    bool CanSearch() const;
    bool IsReady() const;
    hc::viewer::ViewerSnapshot Snapshot() const;
signals:
    void Ready(const QString& SessionId);
    void Failed(const QString& Code);
    void NoMatch();
    void CloseRequested();
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override;
    void focusInEvent(QFocusEvent* Event) override;
private:
    void open(std::vector<hc::plugins::PluginViewerOptions> Candidates,
        std::shared_ptr<hc::storage::IContentSource> Source, std::optional<hc::plugins::PluginViewerSelectionPolicy> Policy);
    void poll();
    void resizeNative();
    QPointer<QtPluginViewerController> controller_;
    QPointer<QWidget> anchor_;
    QLabel* status_ = nullptr;
    QTimer* timer_ = nullptr;
    std::shared_ptr<hc::plugins::HostedViewerSession> session_;
    bool ready_ = false;
    QString lastSearch_;
};
