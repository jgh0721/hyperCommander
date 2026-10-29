#pragma once
#include <QObject>
#include <functional>
#include <QPointer>
#include <QTimer>
#include <QRect>
#include <QJsonObject>
class QWidget;
class QToolBar;
class QLabel;
namespace hc::document
{
class ImageReadingView;
class ImageFullscreen final : public QObject
{
    Q_OBJECT
  public:
    explicit ImageFullscreen(QWidget *Workspace, std::function<ImageReadingView *()> Reader = {}, std::function<void(int)> Navigate = {});
    void UpdateStatus();
    ~ImageFullscreen() override;
    void Toggle();
    void Enter();
    void Leave();
    bool Active() const
    {
        return active_;
    }
    void Restore();
    QJsonObject State() const;

  signals:
    void ActiveChanged(bool Active);

  protected:
    bool eventFilter(QObject *, QEvent *) override;

  private:
    void reveal(bool Top, bool Bottom);
    void timeout();
    void save();
    QWidget *workspace_;
    std::function<ImageReadingView *()> reader_;
    std::function<void(int)> navigate_;
    QPointer<QWidget> parent_;
    Qt::WindowFlags flags_;
    QRect normal_;
    bool active_ = false, maximized_ = false, restored_ = false, pinTop_ = false, pinBottom_ = false,
         hideCursor_ = true, remember_ = true, tracked_ = false;
    QList<QPointer<QWidget>> hidden_;
    QToolBar *top_;
    QLabel *bottom_;
    QTimer idle_, edge_;
    int edgeSide_ = 0;
};
} // namespace hc::document
