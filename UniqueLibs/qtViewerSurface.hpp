#pragma once
#include "commonLibs/viewer/viewer.hpp"
#include "qtWlxSurface.hpp"
#include <QWidget>
#include <QUrl>

class QtViewerSurface final : public QWidget
{
    Q_OBJECT
public:
    struct WlxChoice { QString id_, name_; hc::plugins::WlxViewerOptions options_; };
    using WlxChoices = std::function<std::vector<WlxChoice>()>;
    enum class WlxSelectionMode { Manual, Automatic, Force };
    using WlxSelection = std::function<WlxSelectionMode()>;
    // Service belongs to the GUI host and outlives this surface.
    explicit QtViewerSurface(hc::viewer::ViewerService& Service, QWidget* Parent = nullptr);
    ~QtViewerSurface() override;
    void ConfigureWlx(QtWlxController* Controller, WlxChoices Choices, WlxSelection Selection = {});
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
