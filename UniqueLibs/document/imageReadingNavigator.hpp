#pragma once
#include <QWidget>
#include <QTimer>

class QComboBox;
class QIcon;
class QLabel;
class QMenu;
class QPainter;
class QRectF;
class QSlider;
class QToolButton;
namespace hc::document
{
class ImageReadingView;
class ImageReadingNavigator final : public QWidget
{
    Q_OBJECT
  public:
    explicit ImageReadingNavigator(ImageReadingView *Reader);
    ~ImageReadingNavigator() override;
    void Sync();
    void Reveal();
    // Hides an unpinned navigator unless the pointer, the focus or a popup still uses it.
    void HideIdle();
    // Opens the page list; the reading context menu and the shared page list command use it too.
    void ShowPages();
    bool Pinned() const;
    void SetPinned(bool On);
    void SetOptionsMenu(QMenu *Menu);
    // Shared look of the reading overlays: the full screen capsule and the loading notice match this pill.
    static QString OverlayStyleSheet(const QString &Root);
    static QIcon OverlayIcon(const QString &Name);
    static void PaintOverlay(QPainter &Painter, const QRectF &Rect, qreal Radius, int Shadow = 0);
  signals:
    void ModeRequested(int Mode);
    void PinnedChanged(bool Pinned);
  protected:
    bool eventFilter(QObject *Object, QEvent *Event) override;
    void paintEvent(QPaintEvent *) override;
  private:
    void position();
    void showPages();
    ImageReadingView *reader_;
    QSlider *progress_;
    QLabel *position_;
    QComboBox *mode_, *fit_;
    QToolButton *pages_, *previousVolume_, *previous_, *next_, *nextVolume_, *minus_, *plus_, *pin_, *more_;
    QTimer idle_, message_;
    bool eligible_ = false;
    int lastMode_ = -1;
    QString lastError_;
};
} // namespace hc::document
