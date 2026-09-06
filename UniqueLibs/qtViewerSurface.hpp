#pragma once
#include "commonLibs/viewer/viewer.hpp"
#include <QWidget>
#include <QUrl>

class QtViewerSurface final : public QWidget
{
    Q_OBJECT
public:
    // Service belongs to the GUI host and outlives this surface.
    explicit QtViewerSurface(hc::viewer::ViewerService& Service, QWidget* Parent = nullptr);
    ~QtViewerSurface() override;
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
};
