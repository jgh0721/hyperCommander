#pragma once
#include <QWidget>
#include <QTimer>

class QComboBox;
class QLabel;
class QMenu;
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
    void SetOptionsMenu(QMenu *Menu);
  signals:
    void ModeRequested(int Mode);
  protected:
    bool eventFilter(QObject *Object, QEvent *Event) override;
    void paintEvent(QPaintEvent *) override;
  private:
    void position();
    void hideIdle();
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
