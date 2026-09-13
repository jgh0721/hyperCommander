#pragma once
#include "commonLibs/viewer/viewer.hpp"
#include "qtPluginViewerSurface.hpp"
#include <QWidget>
#include <QUrl>

class QtViewerSurface final : public QWidget
{
    Q_OBJECT
public:
    struct PluginViewerChoice { QString id_, name_; hc::plugins::PluginViewerOptions options_; };
    using ViewerChoices = std::function<std::vector<PluginViewerChoice>()>;
    enum class PluginViewerSelectionMode { Manual, Automatic, Force };
    using ViewerSelection = std::function<PluginViewerSelectionMode()>;
    // Service belongs to the GUI host and outlives this surface.
    explicit QtViewerSurface(hc::viewer::ViewerService& Service, QWidget* Parent = nullptr);
    ~QtViewerSurface() override;
    void ConfigurePlugins(QtPluginViewerController* Controller, ViewerChoices Choices, ViewerSelection Selection = {});
    void Open(std::shared_ptr<hc::storage::IContentSource> Source,
        hc::viewer::ViewerPurpose Purpose = hc::viewer::ViewerPurpose::FullViewer);
    void Close();
    void Execute(const QString& Command);
    bool IsReady() const;
    hc::viewer::ViewerSnapshot Snapshot() const;
signals:
    void Presenting(const QString& SessionId);
    void Ready(const QString& SessionId);
    void Failed(const QString& Code);
    void LinkRequested(const QUrl& Url);
    void CloseRequested();
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override;
private:
    struct State;
    std::unique_ptr<State> state_;
    void reopen();
    void poll();
    void present();
    void finishPresentation();
    void updateImage();
    void find(bool Backward);
    void fail(const QString& Code);
    void refreshWlxMenu();
};
