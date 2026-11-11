#pragma once
#include "pluginHost/hostedViewerSession.hpp"
#include "document/imagePerformance.hpp"
#include <QPointer>
#include <QWidget>
#include <QElapsedTimer>

class QLabel;
class QProgressBar;
class QTimer;
class QtPluginViewerController final : public QObject
{
    Q_OBJECT
public:
    QtPluginViewerController(QWidget* Main, hc::plugins::PluginViewerService::MaterializerFactory Factory, int PreviewIdleMs = 60000);
    ~QtPluginViewerController() override;
    hc::storage::Result<std::shared_ptr<hc::plugins::HostedViewerSession>> Prepare(QWidget* Anchor,
        hc::plugins::PluginViewerOptions Options);
    hc::storage::Result<std::shared_ptr<hc::plugins::HostedViewerSession>> Open(QWidget* Anchor,
        hc::plugins::PluginViewerOptions Options, std::shared_ptr<hc::storage::IContentSource> Source);
    hc::storage::Result<std::shared_ptr<hc::plugins::HostedViewerSession>> OpenAutomatic(QWidget* Anchor,
        std::vector<hc::plugins::PluginViewerOptions> Candidates, std::shared_ptr<hc::storage::IContentSource> Source,
        hc::plugins::PluginViewerSelectionPolicy Policy);
    void Retire(QWidget* Anchor, const std::shared_ptr<hc::plugins::HostedViewerSession>& Session);
    bool CachePreview(QWidget* Anchor, const std::shared_ptr<hc::plugins::HostedViewerSession>& Session,
        const hc::plugins::PluginViewerOptions& Options);
    struct CachedPreview { QPointer<QWidget> anchor_; std::shared_ptr<hc::plugins::HostedViewerSession> session_; };
    CachedPreview TakePreview(const hc::plugins::PluginViewerOptions& Options);
    void CloseAll();
    bool IsIdle() const;
private:
    void collect();
    struct Record { QPointer<QWidget> anchor_; std::shared_ptr<hc::plugins::HostedViewerSession> session_; };
    std::vector<Record> records_;
    QPointer<QWidget> parking_;
    CachedPreview previewCache_;
    std::optional<hc::plugins::PluginViewerOptions> previewOptions_;
    std::uint64_t previewClearSequence_ = 0;
    QByteArray previewFiles_;
    QElapsedTimer previewIdle_;
    int previewIdleMs_ = 60000;
    std::unique_ptr<hc::plugins::PluginViewerService> service_;
    QTimer* timer_ = nullptr;
};

class QtPluginViewerSurface final : public QWidget
{
    Q_OBJECT
public:
    explicit QtPluginViewerSurface(QtPluginViewerController* Controller, QWidget* Parent = nullptr);
    ~QtPluginViewerSurface() override;
    void Prepare(hc::plugins::PluginViewerOptions Options);
    void Open(hc::plugins::PluginViewerOptions Options, std::shared_ptr<hc::storage::IContentSource> Source);
    void OpenAutomatic(std::vector<hc::plugins::PluginViewerOptions> Candidates,
        std::shared_ptr<hc::storage::IContentSource> Source, hc::plugins::PluginViewerSelectionPolicy Policy);
    hc::plugins::PluginViewerDetails Details() const;
    void Close();
    void RequestArchiveClose();
    void Execute(const QString& Command);
    void Search(const QString& Text, bool Backward);
    bool CanSearch() const;
    hc::storage::Result<std::uint64_t> SendDocument(QJsonObject Request);
    hc::storage::Result<std::uint64_t> OpenDocuments(std::vector<hc::plugins::HostedViewerSession::DocumentInput> Inputs);
    bool ReusePreview(const hc::plugins::PluginViewerOptions& Options, std::shared_ptr<hc::storage::IContentSource> Source);
    bool OpenPrepared(std::vector<hc::plugins::HostedViewerSession::DocumentInput> Inputs);
    bool IsReady() const;
    void SetReturnKey(QKeyCombination Key);
    bool WasInteracted() const;
    hc::viewer::ViewerSnapshot Snapshot() const;
signals:
    void Ready(const QString& SessionId);
    void Failed(const QString& Code);
    void NoMatch();
    void CloseRequested();
    void ReturnFocusRequested();
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override;
    void focusInEvent(QFocusEvent* Event) override;
private:
    void open(std::vector<hc::plugins::PluginViewerOptions> Candidates,
        std::shared_ptr<hc::storage::IContentSource> Source, std::optional<hc::plugins::PluginViewerSelectionPolicy> Policy, bool Preparing = false);
    void beginPreparation(const QString& Name);
    void poll();
    void resizeNative();
    QPointer<QtPluginViewerController> controller_;
    QPointer<QWidget> anchor_;
    QWidget* preparation_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* preparationDetail_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QElapsedTimer preparationElapsed_;
    QTimer* timer_ = nullptr;
    std::shared_ptr<hc::plugins::HostedViewerSession> session_;
    std::unique_ptr<hc::document::ImageTraceSpan> presentationTrace_;
    bool appearancePending_ = true;
    bool ready_ = false, preparing_ = false;
    bool preview_ = false, interacted_ = false;
    std::optional<hc::plugins::PluginViewerOptions> previewOptions_;
    std::uint64_t preparedSequence_ = 0;
    std::uint64_t archiveCloseSequence_ = 0;
    QByteArray previewFiles_;
    QString lastSearch_, clipboardToken_;
    QKeyCombination returnKey_;
};
