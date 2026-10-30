#pragma once
#include "pluginHost/hostedViewerSession.hpp"
#include "document/imagePerformance.hpp"
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
    hc::storage::Result<std::uint64_t> SendDocument(QJsonObject Request);
    hc::storage::Result<std::uint64_t> OpenDocuments(std::vector<hc::plugins::HostedViewerSession::DocumentInput> Inputs);
    bool OpenPrepared(std::vector<hc::plugins::HostedViewerSession::DocumentInput> Inputs);
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
    std::unique_ptr<hc::document::ImageTraceSpan> presentationTrace_;
    bool appearancePending_ = true;
    bool ready_ = false;
    std::uint64_t preparedSequence_ = 0;
    QString lastSearch_, clipboardToken_;
};
