#pragma once
#include "qtViewerSurface.hpp"
#include <QPointer>

class QDialog;
class QTimer;
class QtViewerHost final : public QObject
{
public:
    explicit QtViewerHost(QWidget* Main,
        hc::plugins::PluginViewerService::MaterializerFactory Factory = {}, QtViewerSurface::ViewerChoices Choices = {},
        QtViewerSurface::ViewerSelection Selection = {});
    ~QtViewerHost() override;
    QtViewerSurface* CreateSurface(QWidget* Parent);
    QDialog* OpenFull(std::shared_ptr<hc::storage::IContentSource> Source);
    void CloseAll();
    bool IsIdle() const;
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override;
private:
    QPointer<QWidget> main_;
    std::unique_ptr<hc::viewer::ViewerService> service_;
    std::vector<QPointer<QtViewerSurface>> surfaces_;
    QTimer* closeTimer_ = nullptr;
    std::unique_ptr<QtPluginViewerController> wlx_;
    QtViewerSurface::ViewerChoices choices_;
    QtViewerSurface::ViewerSelection selection_;
};
