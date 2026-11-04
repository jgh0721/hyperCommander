#pragma once
#include <QObject>
#include <functional>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QTimer>
#include <QRect>
#include <QJsonObject>
class QWidget;
class QAction;
class QHBoxLayout;
class QLabel;
class QMenu;
class QToolButton;
namespace hc::document
{
class ImageReadingView;
class ImageReadingNavigator;
class ImageFullscreen final : public QObject
{
    Q_OBJECT
  public:
    explicit ImageFullscreen(QWidget *Workspace, std::function<ImageReadingView *()> Reader = {}, std::function<void(int)> Navigate = {});
    // Refreshes the capsule (book · image name, reading tools) and the status line on the next turn.
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
    // active · toolbar (top capsule) · status (status line) · navigator · tools (shared action names) · cursorHidden
    QJsonObject State() const;

  signals:
    void ActiveChanged(bool Active);

  protected:
    bool eventFilter(QObject *, QEvent *) override;

  private:
    void reveal(bool Top, bool Bottom);
    void timeout();
    void save();
    void bind();
    void unbind();
    void schedule();
    void refresh();
    void syncTools(int Mode);
    void place();
    ImageReadingView *reader() const;
    ImageReadingNavigator *navigator() const;
    bool navigated() const;
    QToolButton *firstButton() const;
    QWidget *workspace_;
    std::function<ImageReadingView *()> reader_;
    std::function<void(int)> navigate_;
    QPointer<QWidget> parent_;
    Qt::WindowFlags flags_;
    QRect normal_;
    bool active_ = false, maximized_ = false, restored_ = false, pinTop_ = false, pinBottom_ = false,
         hideCursor_ = true, remember_ = true, tracked_ = false, queued_ = false;
    QList<QPointer<QWidget>> hidden_;
    QWidget *top_, *name_, *leading_, *trailing_;
    QLabel *book_, *item_, *bottom_;
    QHBoxLayout *tools_;
    QToolButton *previous_, *next_, *more_, *exit_;
    QMenu *display_;
    QAction *pinTopAction_, *pinBottomAction_, *cursorAction_, *rememberAction_;
    QList<QPointer<QAction>> toolActions_;
    QList<QPointer<QToolButton>> toolButtons_;
    QHash<QString, QPointer<QAction>> shared_;
    QPointer<ImageReadingView> bound_;
    QList<QMetaObject::Connection> bindings_;
    QTimer idle_, edge_;
    int edgeSide_ = 0;
};
} // namespace hc::document
