#pragma once
#include "pluginHost/hostedWlxSession.hpp"
#include <QPointer>
#include <QWidget>

class QLabel;
class QTimer;
class QtWlxController final : public QObject
{
    Q_OBJECT
public:
    QtWlxController(QWidget* Main, hc::plugins::WlxViewerService::MaterializerFactory Factory);
    ~QtWlxController() override;
    hc::storage::Result<std::shared_ptr<hc::plugins::HostedWlxSession>> Open(QWidget* Anchor,
        hc::plugins::WlxViewerOptions Options, std::shared_ptr<hc::storage::IContentSource> Source);
    void Retire(QWidget* Anchor, const std::shared_ptr<hc::plugins::HostedWlxSession>& Session);
    void CloseAll();
    bool IsIdle() const;
private:
    void collect();
    struct Record { QPointer<QWidget> anchor_; std::shared_ptr<hc::plugins::HostedWlxSession> session_; };
    std::vector<Record> records_;
    QPointer<QWidget> parking_;
    std::unique_ptr<hc::plugins::WlxViewerService> service_;
    QTimer* timer_ = nullptr;
};

class QtWlxSurface final : public QWidget
{
    Q_OBJECT
public:
    explicit QtWlxSurface(QtWlxController* Controller, QWidget* Parent = nullptr);
    ~QtWlxSurface() override;
    void Open(hc::plugins::WlxViewerOptions Options, std::shared_ptr<hc::storage::IContentSource> Source);
    void Close();
    void Execute(const QString& Command);
    bool IsReady() const;
    hc::viewer::ViewerSnapshot Snapshot() const;
signals:
    void Ready(const QString& SessionId);
    void Failed(const QString& Code);
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override;
    void focusInEvent(QFocusEvent* Event) override;
private:
    void poll();
    void resizeNative();
    QPointer<QtWlxController> controller_;
    QPointer<QWidget> anchor_;
    QLabel* status_ = nullptr;
    QTimer* timer_ = nullptr;
    std::shared_ptr<hc::plugins::HostedWlxSession> session_;
    bool ready_ = false;
};
