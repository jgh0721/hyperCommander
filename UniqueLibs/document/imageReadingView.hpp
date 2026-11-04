#pragma once
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "imageBook.hpp"
#include "imageResampler.hpp"
#include <QAbstractScrollArea>
#include <QCache>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QElapsedTimer>
#include <stop_token>

class QAction;
class QMenu;
class QPainter;
namespace hc::document
{
class ImageFramePlayer;
class ImageReadingNavigator;
class ImageReadingView final : public QAbstractScrollArea
{
    Q_OBJECT
    std::shared_ptr<QThreadPool> imagePool_ = ImageWorkerPool();
  public:
    enum Mode : int
    {
        Single,
        Comic,
        Webtoon
    };
    explicit ImageReadingView(QWidget *Parent = nullptr);
    ~ImageReadingView() override;
    void Load(std::shared_ptr<ImageBook> Book, const QImage &First = {}, int Start = 0);
    void OpenPath(const QString &Path, const QString &Entry = {}, int RequestedMode = -1);
    void SetMode(Mode Value, bool UserSelected = true);
    void SetFilter(const QString &Id);
    void SetRightToLeft(bool Enabled);
    void SetCover(bool Enabled);
    void SetSplit(bool Enabled);
    void SetAdjustment(const QString &Name, double Value);
    void SetAutomaticNext(bool Enabled)
    {
        automaticNext_ = Enabled;
    }
    void SetFit(int Mode);
    void Zoom(double Factor);
    void Move(int Direction, bool OnePage = false);
    void GoTo(int Index, double Relative = 0);
    void OpenVolume(int Direction);
    void SeekProgress(int Value);
    QStringList PageNames() const;
    // Registered navigation commands; the key handling below keeps the same defaults.
    bool RunCommand(const QString& Id);
    void Cancel();
    void RetryPage();
    QJsonObject State() const;
    QImage CurrentImage() const;
    bool HasPresentationImage() const { return !CurrentImage().isNull() || !previews_.value(index_).isNull(); }
    QString CurrentName() const;
    bool IsBusy() const;
    void SetGap(int Value);
    void SetPrivate(bool Enabled);
    void Bookmark();
    void OpenBookmark();
    void SavePosition();
    void ToggleAnimation();
    void StepAnimation(int Direction);
    // The right-click menu for the current mode (comic or webtoon terms); the caller owns the menu.
    QMenu *CreateContextMenu(QWidget *Parent = nullptr);
  signals:
    void Changed();
    void VolumeChanged(const QString &Path);
    void FramePresented(int Page);
    // Requests the host owns: the page/image picker (Ctrl+G) and toggling full screen (F11).
    void GoToRequested();
    void FullscreenRequested();

  protected:
    bool event(QEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

  private:
    struct Placement
    {
        int page_;
        QRectF rect_;
        QRect crop_;
    };
    QList<Placement> placements(int Index = -1) const;
    int groupCount(int Index) const;
    bool layoutReady(int Index) const;
    void rememberSize(int Page, QSize Size);
    void requestSource(int Page);
    QString renderKey(const Placement &Place, const QRect &Tile) const;
    QList<QRect> visibleTiles(const Placement &Place) const;
    bool paintGroup(QPainter &Painter);
    void beginTransition();
    void trimSourceWork();
    void rebuild(bool KeepAnchor = true);
    void request(const Placement &Place, const QRect &Tile);
    void prefetch();
    void expandSubpages();
    QHash<int, std::shared_ptr<std::stop_source>> sourceStops_;
    QSet<int> knownSizes_;
    QHash<int, quint64> contentVersions_;
    QHash<int, int> previousGroups_;
    int direction_ = 1;
    struct PresentedTile { QRectF rect_; QImage image_; };
    QList<PresentedTile> presentedTiles_;
    int presentedPage_ = -1, presentedLastPage_ = -1;
    quint64 presentedGeneration_ = 0;
    QElapsedTimer transitionClock_;
    double lastTransitionMs_ = 0;
    bool transitionPending_ = false;
    quint64 renderRequests_ = 0, sourceReads_ = 0, presentedFrames_ = 0;
    QColorSpace displayColor_;
    QString displayColorKey_;
    void changed();
    void persist();
    void invalidateRenders();
    void resetAnimation();
    void openVolumePath(const QString &Path);
    qreal scrollPosition() const;
    void setScrollPosition(qreal Value);
    QString historyFile() const;
    std::shared_ptr<ImageBook> book_;
    QVector<QSize> sizes_;
    mutable QCache<int, QImage> sources_{384 * 1024};
    QCache<QString, QImage> tiles_{128 * 1024};
    QSet<QString> pending_;
    QHash<int, QString> pageErrors_, pageWarnings_;
    std::shared_ptr<std::stop_source> stop_ = std::make_shared<std::stop_source>();
    std::uint64_t generation_ = 0;
    std::uint64_t bookGeneration_ = 0;
    std::shared_ptr<std::stop_source> bookStop_ = std::make_shared<std::stop_source>();
    int workers_ = 0, index_ = 0;
    int fit_ = hc::defaults::edit::ImageReading()["fit"].toInt();
    int splitPart_ = hc::defaults::edit::ImageReading()["part"].toInt();
    int gap_ = hc::defaults::edit::ImageReading()["gap"].toInt();
    Mode mode_ = Mode(hc::defaults::edit::ImageReading()["mode"].toInt());
    QString modeOrigin_ = "legacy";
    QString filter_ = hc::defaults::edit::ImageReading()["filter"].toString(), error_;
    bool rtl_ = hc::defaults::edit::ImageReading()["rtl"].toBool();
    bool cover_ = hc::defaults::edit::ViewValue("imageView/comic/coverAlone").toBool();
    bool split_ = hc::defaults::edit::ImageReading()["split"].toBool();
    bool automaticNext_ = hc::defaults::edit::ImageReading()["automaticNext"].toBool();
    bool volumePending_ = false, rebuilding_ = false;
    double zoom_ = hc::defaults::edit::ImageReading()["zoom"].toDouble();
    double brightness_ = hc::defaults::edit::ImageReading()["brightness"].toDouble();
    double contrast_ = hc::defaults::edit::ImageReading()["contrast"].toDouble();
    double gamma_ = hc::defaults::edit::ImageReading()["gamma"].toDouble();
    double sharpness_ = hc::defaults::edit::ImageReading()["sharpness"].toDouble();
    bool grayscale_ = hc::defaults::edit::ImageReading()["grayscale"].toBool();
    bool invert_ = hc::defaults::edit::ImageReading()["invert"].toBool();
    qreal extent_ = 0;
    qreal width_ = 0;
    double relative_ = 0;
    QTimer saveTimer_;
    ImageFramePlayer *animation_ = nullptr;
    ImageReadingNavigator *navigator_ = nullptr;
    QAction *retry_ = nullptr;
    int animationPage_ = -1;
    bool animationPending_ = false;
    std::uint64_t animationGeneration_ = 0;
    bool dragging_ = false, private_ = false, suspended_ = false;
    QPoint dragPoint_;
    QJsonObject bookmark_;
    QImage currentImage_;
    QHash<int,QImage> previews_;
};
} // namespace hc::document
