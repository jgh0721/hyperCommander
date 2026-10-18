#pragma once
#include "qtViewerSurface.hpp"
#include <QPointer>

class QDialog;
class QTimer;
class QTemporaryDir;
class QtViewerHost final : public QObject
{
public:
    explicit QtViewerHost(QWidget* Main,
        hc::plugins::PluginViewerService::MaterializerFactory Factory = {}, QtViewerSurface::ViewerChoices Choices = {},
        QtViewerSurface::ViewerSelection Selection = {});
    ~QtViewerHost() override;
    QtViewerSurface* CreateSurface(QWidget* Parent);
    QDialog* OpenFull(std::shared_ptr<hc::storage::IContentSource> Source);
    QDialog* OpenPlugin(hc::plugins::PluginViewerOptions Options,
        std::vector<hc::plugins::HostedViewerSession::DocumentInput> Inputs, const QString& Title,
        std::function<void(bool, const QString&)> Completion = {});
    void PreparePlugin(hc::plugins::PluginViewerOptions Options);
    void CloseAll();
    bool IsIdle() const;
    std::function<void(const QString&)> reviewPlugin_;
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override;
private:
    std::shared_ptr<hc::storage::IContentSource> preparedSource();
    bool matchesPrepared(const hc::plugins::PluginViewerOptions& Options) const;
    void parkPrepared(QDialog* Window, QtPluginViewerSurface* Surface);
    void retirePlugin(QDialog* Window, QtPluginViewerSurface* Surface, bool Recyclable);
    QPointer<QWidget> main_;
    QPointer<QDialog> preparedWindow_;
    QPointer<QtPluginViewerSurface> preparedSurface_;
    std::optional<hc::plugins::PluginViewerOptions> preparedOptions_;
    std::unique_ptr<QTemporaryDir> preparedDirectory_;
    bool closing_ = false;
    std::unique_ptr<hc::viewer::ViewerService> service_;
    std::vector<QPointer<QtViewerSurface>> surfaces_;
    QTimer* closeTimer_ = nullptr;
    std::unique_ptr<QtPluginViewerController> pluginViewer_;
    QtViewerSurface::ViewerChoices choices_;
    QtViewerSurface::ViewerSelection selection_;
};
