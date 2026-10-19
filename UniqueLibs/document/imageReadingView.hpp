#pragma once
#include "imageBook.hpp"
#include <QAbstractScrollArea>
#include <QCache>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <stop_token>

namespace hc::document
{
class ImageFramePlayer;
class ImageReadingView final : public QAbstractScrollArea
{
    Q_OBJECT
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
    void SetMode(Mode Value);
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
    void Cancel();
    QJsonObject State() const;
    QImage CurrentImage() const;
    QString CurrentName() const;
    bool IsBusy() const;
    void SetGap(int Value);
    void SetPrivate(bool Enabled);
    void Bookmark();
    void OpenBookmark();
    void SavePosition();
    void ToggleAnimation();
    void StepAnimation(int Direction);
  signals:
    void Changed();
    void VolumeChanged(const QString &Path);

  protected:
    bool event(QEvent *) override;
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
    QList<Placement> placements() const;
    void rebuild(bool KeepAnchor = true);
    void request(const Placement &Place, const QRect &Tile);
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
    QCache<QString, QImage> previousTiles_{32 * 1024};
    bool animationUpdate_ = false;
    QSet<QString> pending_;
    QHash<int, QString> pageErrors_;
    std::shared_ptr<std::stop_source> stop_ = std::make_shared<std::stop_source>();
    std::uint64_t generation_ = 0;
    std::uint64_t bookGeneration_ = 0;
    std::shared_ptr<std::stop_source> bookStop_ = std::make_shared<std::stop_source>();
    int workers_ = 0, index_ = 0, fit_ = 1;
    int splitPart_ = 0, gap_ = 0;
    Mode mode_ = Single;
    QString filter_ = "auto", error_;
    bool rtl_ = false, cover_ = true, split_ = false, automaticNext_ = true, volumePending_ = false,
         rebuilding_ = false;
    double zoom_ = 1, brightness_ = 0, contrast_ = 1, gamma_ = 1, sharpness_ = 0;
    bool grayscale_ = false, invert_ = false;
    qreal extent_ = 0;
    qreal width_ = 0;
    double relative_ = 0;
    QTimer saveTimer_;
    ImageFramePlayer *animation_ = nullptr;
    int animationPage_ = -1;
    bool animationPending_ = false;
    std::uint64_t animationGeneration_ = 0;
    bool dragging_ = false, private_ = false, suspended_ = false;
    QPoint dragPoint_;
    QJsonObject bookmark_;
    QImage currentImage_;
};
} // namespace hc::document
