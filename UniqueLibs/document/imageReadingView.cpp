#include "imageReadingView.hpp"
#include "imageResampler.hpp"
#include "imageDisplayColor.hpp"
#include "imageFramePlayer.hpp"
#include "core/AppSettings.h"
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QPainter>
#include <QSaveFile>
#include <QLockFile>
#include <QDateTime>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtConcurrent>
#include <cmath>
#include <limits>

namespace hc::document
{
namespace
{
constexpr int ScrollRange = 1000000;
constexpr int TileHeight = 1024;
struct Rendered
{
    QImage source_, tile_;
    QString error_;
};
QString Key(int Page, const QRect &Tile, std::uint64_t Generation)
{
    return QString("%1:%2:%3:%4").arg(Generation).arg(Page).arg(Tile.x()).arg(Tile.y());
}
void Adjust(QImage &Image, double Brightness, double Contrast, double Gamma, bool Gray, bool Invert, double Sharp)
{
    if (!Brightness && Contrast == 1 && Gamma == 1 && !Gray && !Invert && !Sharp)
        return;
    Image = Image.convertToFormat(QImage::Format_RGBA8888);
    const auto Original = Sharp > 0 ? Image.copy() : QImage{};
    for (int Y = 0; Y < Image.height(); ++Y)
    {
        auto *Row = Image.scanLine(Y);
        for (int X = 0; X < Image.width(); ++X)
        {
            double Values[3];
            for (int C = 0; C < 3; ++C)
            {
                double V = Row[4 * X + C] / 255.;
                if (Sharp > 0 && X > 0 && Y > 0 && X + 1 < Image.width() && Y + 1 < Image.height())
                {
                    double Blur = 0;
                    for (int DY = -1; DY <= 1; ++DY)
                        for (int DX = -1; DX <= 1; ++DX)
                            Blur += Original.constScanLine(Y + DY)[4 * (X + DX) + C] / 255.;
                    V += Sharp * (V - Blur / 9);
                }
                V = std::pow(std::clamp((V - .5) * Contrast + .5 + Brightness, 0., 1.), 1. / Gamma);
                Values[C] = Invert ? 1 - V : V;
            }
            if (Gray)
                Values[0] = Values[1] = Values[2] = .2126 * Values[0] + .7152 * Values[1] + .0722 * Values[2];
            for (int C = 0; C < 3; ++C)
                Row[4 * X + C] = uchar(qRound(Values[C] * 255));
        }
    }
}
} // namespace
ImageReadingView::ImageReadingView(QWidget *Parent) : QAbstractScrollArea(Parent)
{
    animation_ = new ImageFramePlayer(this);
    connect(animation_, &ImageFramePlayer::Frame, this,
            [this](const QImage &Image)
            {
                if (animationPage_ != index_ || animationGeneration_ != bookGeneration_ || workers_ > 0)
                    return;
                currentImage_ = Image;
                sources_.insert(index_, new QImage(Image), qMax(1, int(Image.sizeInBytes() / 1024)));
                animationUpdate_ = true;
                invalidateRenders();
                animationUpdate_ = false;
                tiles_.clear();
                changed();
            });
    connect(animation_, &ImageFramePlayer::Changed, this, &ImageReadingView::Changed);
    setObjectName("imageReadingView");
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
    verticalScrollBar()->setSingleStep(20000);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this]
            {
                if (!book_ || rebuilding_)
                    return;
                const int Previous = index_;
                if (mode_ == Webtoon)
                {
                    qreal Y = 0, Pos = scrollPosition();
                    for (int I = 0; I < sizes_.size(); ++I)
                    {
                        const auto S = sizes_[I];
                        const qreal H = (viewport()->width() * zoom_) * S.height() / qMax(1, S.width());
                        if (Y + H > Pos)
                        {
                            index_ = I;
                            relative_ = H > 0 ? (Pos - Y) / H : 0;
                            break;
                        }
                        Y += H + gap_;
                    }
                }
                if (Previous != index_)
                {
                    resetAnimation();
                    currentImage_ = {};
                }
                saveTimer_.start();
                viewport()->update();
                emit Changed();
            });
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(600);
    connect(&saveTimer_, &QTimer::timeout, this, &ImageReadingView::persist);
}
ImageReadingView::~ImageReadingView()
{
    SavePosition();
    Cancel();
}
void ImageReadingView::OpenPath(const QString &Path, const QString &Entry, int RequestedMode)
{
    Cancel();
    const auto Generation = bookGeneration_;
    const auto Stop = bookStop_;
    ++workers_;
    using OpenResult = storage::Result<std::pair<std::shared_ptr<ImageBook>, int>>;
    auto *Watcher = new QFutureWatcher<OpenResult>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this,
            [this, Watcher, Generation, RequestedMode]
            {
                auto Result = Watcher->result();
                Watcher->deleteLater();
                --workers_;
                if (Generation != bookGeneration_)
                    return;
                if (!Result.HasValue())
                {
                    error_ = QString::fromStdString(Result.Failure().code_);
                    changed();
                    return;
                }
                Load(Result.Value().first, {}, Result.Value().second);
                if (RequestedMode >= 0)
                    SetMode(Mode(RequestedMode));
            });
    Watcher->setFuture(QtConcurrent::run(
        [Path, Entry, Stop]() -> OpenResult
        {
            storage::OperationContext Context;
            Context.stopToken_ = Stop->get_token();
            auto Book = OpenImageBook(Path, Context);
            if (!Book.HasValue())
                return Book.Failure();
            int Index = 0;
            for (int I = 0; I < Book.Value()->pages_.size(); ++I)
                if (Book.Value()->pages_[I].file_.compare(Path, Qt::CaseInsensitive) == 0 ||
                    Book.Value()->pages_[I].name_.compare(Entry, Qt::CaseInsensitive) == 0)
                {
                    Index = I;
                    break;
                }
            return std::pair{Book.Value(), Index};
        }));
}
QString ImageReadingView::historyFile() const
{
    AppSettings Settings;
    return QFileInfo(Settings.fileName()).absolutePath() + "/HyperViewer-reading.json";
}
void ImageReadingView::Load(std::shared_ptr<ImageBook> Book, const QImage &First, int Start)
{
    SavePosition();
    Cancel();
    book_ = std::move(Book);
    tiles_.clear();
    sources_.clear();
    pending_.clear();
    pageErrors_.clear();
    error_.clear();
    index_ = Start;
    bookmark_ = {};
    splitPart_ = 0;
    zoom_ = 1;
    currentImage_ = {};
    sizes_.fill(QSize(1000, 1400), book_ ? book_->pages_.size() : 0);
    if (!book_ || book_->pages_.isEmpty())
        return;
    if (!First.isNull() && Start >= 0 && Start < sizes_.size())
    {
        sizes_[Start] = First.size();
        currentImage_ = First;
        sources_.insert(Start, new QImage(First), qMax(1, int(First.sizeInBytes() / 1024)));
    }
    QFile File(historyFile());
    QJsonObject Root;
    if (File.exists() && File.open(QIODevice::ReadOnly))
    {
        const auto Doc = QJsonDocument::fromJson(File.readAll());
        if (Doc.isObject())
            Root = Doc.object();
    }
    const auto Entry = Root.value(book_->path_).toObject();
    const bool Restored = Entry["revision"].toString() == book_->revision_;
    setProperty("readingHistoryRestored", Restored);
    if (Restored)
    {
        for (int I = 0; I < book_->pages_.size(); ++I)
            if (book_->pages_[I].id_ == Entry["pageId"].toString())
            {
                index_ = I;
                break;
            }
        rtl_ = Entry["rtl"].toBool();
        cover_ = Entry["cover"].toBool(true);
        split_ = Entry["split"].toBool();
        splitPart_ = Entry["part"].toInt();
        bookmark_ = Entry["bookmark"].toObject();
        gap_ = qBound(0, Entry["gap"].toInt(), 200);
        fit_ = qBound(0, Entry["fit"].toInt(1), 3);
        brightness_ = qBound(-1., Entry["brightness"].toDouble(), 1.);
        contrast_ = qBound(.1, Entry["contrast"].toDouble(1), 3.);
        gamma_ = qBound(.1, Entry["gamma"].toDouble(1), 5.);
        sharpness_ = qBound(0., Entry["sharpness"].toDouble(), 2.);
        grayscale_ = Entry["grayscale"].toBool();
        invert_ = Entry["invert"].toBool();
        automaticNext_ = Entry["automaticNext"].toBool(true);
        zoom_ = qBound(.1, Entry["zoom"].toDouble(1), 16.);
        const auto SavedFilter = Entry["filter"].toString("auto");
        filter_ = SavedFilter == "auto" || ImageFilterRegistry::Resolve(SavedFilter, {}).kernel_ ? SavedFilter
                                                                                                 : QString("auto");
        mode_ = Mode(qBound(0, Entry["mode"].toInt(int(mode_)), 2));
    }
    if (index_ != Start)
        currentImage_ = {};
    rebuild(false);
    GoTo(qBound(0, index_, int(book_->pages_.size()) - 1), Restored ? Entry["relative"].toDouble() : 0);
    changed();
}
void ImageReadingView::invalidateRenders()
{
    if (!animationUpdate_)
        previousTiles_.clear();
    stop_->request_stop();
    stop_ = std::make_shared<std::stop_source>();
    ++generation_;
    pending_.clear();
}
void ImageReadingView::Cancel()
{
    invalidateRenders();
    bookStop_->request_stop();
    bookStop_ = std::make_shared<std::stop_source>();
    ++bookGeneration_;
    resetAnimation();
    volumePending_ = false;
    animationPending_ = false;
    suspended_ = true;
}
void ImageReadingView::SetMode(Mode Value)
{
    mode_ = Value;
    splitPart_ = 0;
    rebuild();
    changed();
}
void ImageReadingView::SetFilter(const QString &Id)
{
    filter_ = Id == "auto" || ImageFilterRegistry::Resolve(Id, {}).kernel_ ? Id : QString("auto");
    invalidateRenders();
    tiles_.clear();
    changed();
}
void ImageReadingView::SetRightToLeft(bool Enabled)
{
    rtl_ = Enabled;
    rebuild();
    changed();
}
void ImageReadingView::SetCover(bool Enabled)
{
    cover_ = Enabled;
    rebuild();
    changed();
}
void ImageReadingView::SetSplit(bool Enabled)
{
    split_ = Enabled;
    splitPart_ = 0;
    rebuild();
    changed();
}
void ImageReadingView::SetFit(int Mode)
{
    fit_ = qBound(0, Mode, 3);
    zoom_ = 1;
    rebuild();
    changed();
}
void ImageReadingView::Zoom(double Factor)
{
    zoom_ = qBound(.1, zoom_ * Factor, 16.);
    rebuild();
    changed();
}
void ImageReadingView::SetAdjustment(const QString &Name, double Value)
{
    if (Name == "brightness")
        brightness_ = qBound(-1., Value, 1.);
    else if (Name == "contrast")
        contrast_ = qBound(.1, Value, 3.);
    else if (Name == "gamma")
        gamma_ = qBound(.1, Value, 5.);
    else if (Name == "grayscale")
        grayscale_ = Value != 0;
    else if (Name == "invert")
        invert_ = Value != 0;
    else if (Name == "sharpness")
        sharpness_ = qBound(0., Value, 2.);
    else
        return;
    invalidateRenders();
    tiles_.clear();
    changed();
}
qreal ImageReadingView::scrollPosition() const
{
    return qMax(qreal(0), extent_ - viewport()->height()) * verticalScrollBar()->value() / ScrollRange;
}
void ImageReadingView::setScrollPosition(qreal Value)
{
    const auto Max = qMax(qreal(0), extent_ - viewport()->height());
    verticalScrollBar()->setValue(Max > 0 ? qBound(0, int(Value / Max * ScrollRange), ScrollRange) : 0);
}
QList<ImageReadingView::Placement> ImageReadingView::placements() const
{
    QList<Placement> Out;
    if (!book_ || sizes_.isEmpty())
        return Out;
    const qreal W = viewport()->width(), H = viewport()->height(), YScroll = scrollPosition(),
                XScroll = horizontalScrollBar()->value();
    if (mode_ == Webtoon)
    {
        qreal Y = 0;
        for (int I = 0; I < sizes_.size(); ++I)
        {
            const auto S = sizes_[I];
            const qreal Width = W * zoom_, Height = Width * S.height() / qMax(1, S.width());
            if (Y + Height >= YScroll - H && Y <= YScroll + 3 * H)
                Out << Placement{I, QRectF(qMax(qreal(0), (W - Width) / 2) - XScroll, Y - YScroll, Width, Height),
                                 QRect(QPoint(), S)};
            Y += Height + gap_;
        }
        return Out;
    }
    const auto S = sizes_[index_];
    const bool Wide = S.width() > S.height() * 1.35;
    int Count = mode_ == Comic && !split_ && !(cover_ && index_ == 0) && !Wide && index_ + 1 < sizes_.size() ? 2 : 1;
    if (Count == 2 && sizes_[index_ + 1].width() > sizes_[index_ + 1].height() * 1.35)
        Count = 1;
    qreal TotalWidth = 0, MaxHeight = 0;
    for (int N = 0; N < Count; ++N)
    {
        auto Sz = sizes_[index_ + N];
        if (split_ && Wide)
            Sz.setWidth(qMax(1, Sz.width() / 2));
        TotalWidth += Sz.width();
        MaxHeight = qMax(MaxHeight, qreal(Sz.height()));
    }
    qreal Scale = fit_ == 0   ? 1.
                  : fit_ == 2 ? W / TotalWidth
                  : fit_ == 3 ? H / MaxHeight
                              : qMin(W / TotalWidth, H / MaxHeight);
    Scale *= zoom_;
    qreal X = qMax(qreal(0), (W - TotalWidth * Scale) / 2) - XScroll;
    for (int N = 0; N < Count; ++N)
    {
        const int I = index_ + (rtl_ ? Count - 1 - N : N);
        const auto Size = sizes_[I];
        QRect Crop(QPoint(), Size);
        if (split_ && Wide)
        {
            const int Half = Size.width() / 2;
            const bool Right = (splitPart_ == 0) == rtl_;
            Crop = QRect(Right ? Half : 0, 0, Right ? Size.width() - Half : Half, Size.height());
        }
        const qreal Width = Crop.width() * Scale, Height = Crop.height() * Scale;
        Out << Placement{I, QRectF(X, qMax(qreal(0), (H - Height) / 2) - YScroll, Width, Height), Crop};
        X += Width;
    }
    return Out;
}
void ImageReadingView::rebuild(bool KeepAnchor)
{
    const auto Old = State();
    rebuilding_ = true;
    extent_ = 0;
    width_ = 0;
    if (book_ && !sizes_.isEmpty())
    {
        if (mode_ == Webtoon)
        {
            for (const auto &S : sizes_)
                extent_ += viewport()->width() * zoom_ * S.height() / qMax(1, S.width()) + gap_;
            width_ = viewport()->width() * zoom_;
        }
        else
        {
            const auto Places = placements();
            for (const auto &P : Places)
            {
                extent_ = qMax(extent_, P.rect_.height());
                width_ += P.rect_.width();
            }
        }
    }
    horizontalScrollBar()->setRange(0, qMax(0, qRound(width_ - viewport()->width())));
    horizontalScrollBar()->setPageStep(viewport()->width());
    verticalScrollBar()->setRange(0, extent_ > viewport()->height() ? ScrollRange : 0);
    verticalScrollBar()->setPageStep(extent_ > 0 ? qMax(1, int(viewport()->height() / extent_ * ScrollRange))
                                                 : ScrollRange);
    rebuilding_ = false;
    invalidateRenders();
    tiles_.clear();
    if (KeepAnchor)
        GoTo(index_, Old["relative"].toDouble());
    viewport()->update();
}
void ImageReadingView::request(const Placement &P, const QRect &Tile)
{
    const auto CacheKey = Key(P.page_, Tile, generation_);
    if (suspended_ || pending_.contains(CacheKey) || pageErrors_.contains(P.page_) || tiles_.contains(CacheKey) ||
        workers_ >= 2)
        return;
    pending_.insert(CacheKey);
    ++workers_;
    const auto Book = book_;
    const auto Generation = generation_;
    const auto Stop = stop_;
    const auto Source = P.page_ == index_ && !currentImage_.isNull() ? currentImage_
                        : sources_.object(P.page_)                   ? *sources_.object(P.page_)
                                                                     : QImage{};
    const auto DisplayColor = ImageDisplayColorSpace(screen());
    const auto Filter = filter_;
    const auto Dpr = devicePixelRatioF();
    const auto Output = (P.rect_.size() * Dpr).toSize();
    const auto ExpectedSize = sizes_[P.page_];
    const auto Bright = brightness_, Contrast = contrast_, Gamma = gamma_, Sharp = sharpness_;
    const auto Gray = grayscale_, Invert = invert_;
    auto *Watcher = new QFutureWatcher<Rendered>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this,
            [this, Watcher, CacheKey, Generation, Page = P.page_]
            {
                auto Result = Watcher->result();
                Watcher->deleteLater();
                --workers_;
                if (Generation != generation_)
                {
                    viewport()->update();
                    emit Changed();
                    return;
                }
                pending_.remove(CacheKey);
                if (!Result.error_.isEmpty())
                {
                    pageErrors_[Page] = Result.error_;
                    viewport()->update();
                    emit Changed();
                    return;
                }
                const bool Resize = sizes_[Page] != Result.source_.size();
                sizes_[Page] = Result.source_.size();
                if (Page == index_)
                    currentImage_ = Result.source_;
                sources_.insert(Page, new QImage(Result.source_), qMax(1, int(Result.source_.sizeInBytes() / 1024)));
                if (Resize)
                {
                    rebuild();
                    changed();
                    return;
                }
                previousTiles_.insert(CacheKey.section(':', 1), new QImage(Result.tile_),
                                      qMax(1, int(Result.tile_.sizeInBytes() / 1024)));
                tiles_.insert(CacheKey, new QImage(Result.tile_), qMax(1, int(Result.tile_.sizeInBytes() / 1024)));
                viewport()->update();
                emit Changed();
            });
    Watcher->setFuture(QtConcurrent::run(
        [=]
        {
            QImage Original = Source;
            if (Original.isNull())
            {
                storage::OperationContext Context;
                Context.stopToken_ = Stop->get_token();
                auto Loaded = ReadImagePage(Book, P.page_, Context);
                if (!Loaded.HasValue())
                    return Rendered{{}, {}, QString::fromStdString(Loaded.Failure().code_)};
                Original = Loaded.Value().image_;
            }
            if (Original.size() != ExpectedSize)
                return Rendered{Original, {}, {}};
            QImage Input = P.crop_.size() == Original.size() ? Original : Original.copy(P.crop_);
            ImageRenderRequest Request;
            Request.source_ = Input;
            Request.targetColor_ = DisplayColor;
            Request.outputSize_ = Output;
            Request.tile_ = Sharp > 0 ? Tile.adjusted(-1, -1, 1, 1).intersected(QRect(QPoint(), Output)) : Tile;
            Request.filter_ = Filter;
            Request.stop_ = Stop->get_token();
            Request.sourceToOutput_.scale(double(Output.width()) / Input.width(),
                                          double(Output.height()) / Input.height());
            auto Result = ResampleImage(Request);
            if (Result.error_.isEmpty())
            {
                Adjust(Result.image_, Bright, Contrast, Gamma, Gray, Invert, Sharp);
                if (Request.tile_ != Tile)
                    Result.image_ = Result.image_.copy(QRect(Tile.topLeft() - Request.tile_.topLeft(), Tile.size()));
            }
            return Rendered{Original, Result.image_, Result.error_};
        }));
}
void ImageReadingView::paintEvent(QPaintEvent *)
{
    QPainter Painter(viewport());
    Painter.fillRect(viewport()->rect(), palette().brush(QPalette::Base));
    if (!book_)
    {
        Painter.setPen(palette().color(QPalette::Text));
        Painter.drawText(viewport()->rect(), Qt::AlignCenter, error_.isEmpty() ? tr("불러오는 중…") : error_);
        return;
    }
    const qreal Dpr = devicePixelRatioF();
    for (const auto &P : placements())
    {
        const auto Visible = P.rect_.intersected(QRectF(viewport()->rect()));
        if (Visible.isEmpty())
            continue;
        const int FirstY = qMax(0, int((Visible.top() - P.rect_.top()) * Dpr) / TileHeight);
        const int LastY = qMax(FirstY, int((Visible.bottom() - P.rect_.top()) * Dpr) / TileHeight);
        const int FirstX = qMax(0, int((Visible.left() - P.rect_.left()) * Dpr) / TileHeight);
        const int LastX = qMax(FirstX, int((Visible.right() - P.rect_.left()) * Dpr) / TileHeight);
        for (int Y = FirstY; Y <= LastY; ++Y)
            for (int X = FirstX; X <= LastX; ++X)
            {
                const QRect Tile(X * TileHeight, Y * TileHeight,
                                 qMin(TileHeight, qRound(P.rect_.width() * Dpr) - X * TileHeight),
                                 qMin(TileHeight, qRound(P.rect_.height() * Dpr) - Y * TileHeight));
                if (Tile.isEmpty())
                    continue;
                const auto CacheKey = Key(P.page_, Tile, generation_);
                if (auto *Image = tiles_.object(CacheKey))
                    Painter.drawImage(QRectF(P.rect_.left() + Tile.x() / Dpr, P.rect_.top() + Tile.y() / Dpr,
                                             Tile.width() / Dpr, Tile.height() / Dpr),
                                      *Image);
                else
                {
                    if (auto *Previous = previousTiles_.object(CacheKey.section(':', 1)))
                        Painter.drawImage(QRectF(P.rect_.left() + Tile.x() / Dpr, P.rect_.top() + Tile.y() / Dpr,
                                                 Tile.width() / Dpr, Tile.height() / Dpr),
                                          *Previous);
                    else
                    {
                        Painter.setPen(palette().color(QPalette::Text));
                        Painter.drawText(Visible, Qt::AlignCenter, pageErrors_.value(P.page_, tr("불러오는 중…")));
                    }
                    request(P, Tile);
                }
            }
    }
}
bool ImageReadingView::event(QEvent *E)
{
    const auto Result = QAbstractScrollArea::event(E);
    if ((E->type() == QEvent::ScreenChangeInternal || E->type() == QEvent::DevicePixelRatioChange) && animation_)
        rebuild();
    return Result;
}
void ImageReadingView::resizeEvent(QResizeEvent *Event)
{
    rebuilding_ = true;
    QAbstractScrollArea::resizeEvent(Event);
    rebuild();
}
void ImageReadingView::GoTo(int Index, double Relative)
{
    if (!book_ || Index < 0 || Index >= book_->pages_.size())
        return;
    if (index_ != Index)
    {
        resetAnimation();
        currentImage_ = {};
    }
    const bool NewPage = index_ != Index;
    index_ = Index;
    if (NewPage && mode_ != Webtoon)
    {
        splitPart_ = 0;
        rebuild(false);
    }
    relative_ = qBound(0., Relative, 1.);
    error_.clear();
    QSignalBlocker Block(verticalScrollBar());
    if (mode_ == Webtoon)
    {
        qreal Y = 0;
        for (int I = 0; I < Index; ++I)
            Y += viewport()->width() * zoom_ * sizes_[I].height() / qMax(1, sizes_[I].width()) + gap_;
        Y += qBound(0., Relative, 1.) * viewport()->width() * zoom_ * sizes_[Index].height() /
             qMax(1, sizes_[Index].width());
        setScrollPosition(Y);
    }
    else
        setScrollPosition(qBound(0., Relative, 1.) * extent_);
    changed();
}
void ImageReadingView::Move(int Direction, bool OnePage)
{
    if (!book_ || volumePending_)
        return;
    if (mode_ == Webtoon && !OnePage)
    {
        const auto Before = verticalScrollBar()->value();
        setScrollPosition(scrollPosition() + Direction * viewport()->height() * .9);
        if (Before == verticalScrollBar()->value() && Direction > 0 && automaticNext_)
            OpenVolume(1);
        return;
    }
    if (split_ && sizes_[index_].width() > sizes_[index_].height() * 1.35)
    {
        if (Direction > 0 && splitPart_ == 0)
        {
            splitPart_ = 1;
            rebuild();
            changed();
            return;
        }
        if (Direction < 0 && splitPart_ == 1)
        {
            splitPart_ = 0;
            rebuild();
            changed();
            return;
        }
    }
    if (!OnePage && verticalScrollBar()->maximum() > 0)
    {
        const auto Before = verticalScrollBar()->value();
        setScrollPosition(scrollPosition() + Direction * viewport()->height() * .9);
        if (Before != verticalScrollBar()->value())
            return;
    }
    int Step = 1;
    if (mode_ == Comic && !OnePage && Direction > 0)
        Step = placements().size();
    if (mode_ == Comic && !OnePage && !split_ && Direction < 0)
    {
        int At = 0, Previous = 0;
        while (At < index_)
        {
            Previous = At;
            const auto S = sizes_[At];
            At += (cover_ && At == 0) || S.width() > S.height() * 1.35 || At + 1 >= sizes_.size() ||
                          sizes_[At + 1].width() > sizes_[At + 1].height() * 1.35
                      ? 1
                      : 2;
        }
        Step = index_ - Previous;
    }
    const int Next = index_ + Direction * Step;
    if (Next >= book_->pages_.size())
    {
        if (automaticNext_)
            OpenVolume(1);
        return;
    }
    if (Next < 0)
        return;
    resetAnimation();
    currentImage_ = {};
    index_ = Next;
    splitPart_ = Direction < 0 && split_ ? 1 : 0;
    rebuild(false);
    GoTo(index_);
}
void ImageReadingView::OpenVolume(int Direction)
{
    if (!book_ || volumePending_)
        return;
    volumePending_ = true;
    const auto Path = book_->path_;
    const auto Generation = bookGeneration_;
    const auto Stop = bookStop_;
    auto *Watcher = new QFutureWatcher<QStringList>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this,
            [this, Watcher, Generation]
            {
                auto Candidates = Watcher->result();
                Watcher->deleteLater();
                if (Generation != bookGeneration_)
                    return;
                volumePending_ = false;
                if (Candidates.isEmpty())
                {
                    error_ = tr("이어서 읽을 권이 없습니다.");
                    changed();
                    return;
                }
                QString Path = Candidates.first();
                if (Candidates.size() > 1)
                {
                    bool Ok = false;
                    Path = QInputDialog::getItem(this, tr("다음 권 선택"), tr("같은 권의 후보가 여러 개입니다."),
                                                 Candidates, 0, false, &Ok);
                    if (!Ok)
                        return;
                }
                openVolumePath(Path);
            });
    Watcher->setFuture(
        QtConcurrent::run([Path, Direction, Stop] { return NextImageBooks(Path, Direction, Stop->get_token()); }));
}
void ImageReadingView::openVolumePath(const QString &Path)
{
    volumePending_ = true;
    const auto Generation = bookGeneration_;
    const auto Stop = bookStop_;
    auto *Watcher = new QFutureWatcher<storage::Result<std::shared_ptr<ImageBook>>>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this,
            [this, Watcher, Generation, Path]
            {
                auto Result = Watcher->result();
                Watcher->deleteLater();
                if (Generation != bookGeneration_)
                    return;
                volumePending_ = false;
                if (!Result.HasValue())
                {
                    error_ = QString::fromStdString(Result.Failure().code_);
                    changed();
                    return;
                }
                Load(Result.Value());
                GoTo(0);
                emit VolumeChanged(Path);
            });
    Watcher->setFuture(QtConcurrent::run(
        [Path, Stop]
        {
            storage::OperationContext Context;
            Context.stopToken_ = Stop->get_token();
            return OpenImageBook(Path, Context);
        }));
}
void ImageReadingView::wheelEvent(QWheelEvent *Event)
{
    const auto Delta = Event->pixelDelta().isNull() ? Event->angleDelta() : Event->pixelDelta();
    if (Event->modifiers() & Qt::ControlModifier)
    {
        if (Delta.y())
            Zoom(Delta.y() > 0 ? 1.1 : 1 / 1.1);
    }
    else if (!Event->pixelDelta().isNull() && (mode_ == Webtoon || verticalScrollBar()->maximum()))
    {
        const auto Before = scrollPosition();
        setScrollPosition(Before - Delta.y());
        if (Before == scrollPosition() && Delta.y() < 0 && automaticNext_)
            OpenVolume(1);
        if (Delta.x())
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - Delta.x());
    }
    else if (Delta.y())
        Move(Delta.y() < 0 ? 1 : -1);
    Event->accept();
}
void ImageReadingView::keyPressEvent(QKeyEvent *E)
{
    if (E->key() == Qt::Key_Down)
        setScrollPosition(scrollPosition() + 40);
    else if (E->key() == Qt::Key_Up)
        setScrollPosition(scrollPosition() - 40);
    else if (E->key() == Qt::Key_Space)
        Move(E->modifiers() & Qt::ShiftModifier ? -1 : 1);
    else if (E->key() == Qt::Key_PageDown)
        Move(1, E->modifiers() & Qt::ShiftModifier);
    else if (E->key() == Qt::Key_PageUp)
        Move(-1, E->modifiers() & Qt::ShiftModifier);
    else if (E->key() == Qt::Key_Home)
        GoTo(0);
    else if (E->key() == Qt::Key_End && book_)
        GoTo(book_->pages_.size() - 1);
    else if (E->key() == Qt::Key_BracketRight)
        OpenVolume(1);
    else if (E->key() == Qt::Key_BracketLeft)
        OpenVolume(-1);
    else if (E->key() == Qt::Key_Right)
        Move(rtl_ ? -1 : 1);
    else if (E->key() == Qt::Key_Left)
        Move(rtl_ ? 1 : -1);
    else
    {
        QAbstractScrollArea::keyPressEvent(E);
        return;
    }
    E->accept();
}
QString ImageReadingView::CurrentName() const
{
    return book_ && !book_->pages_.isEmpty() ? book_->pages_[index_].name_ : QString{};
}
QImage ImageReadingView::CurrentImage() const
{
    if (!currentImage_.isNull())
        return currentImage_;
    auto *Image = sources_.object(index_);
    return Image ? *Image : QImage{};
}
void ImageReadingView::StepAnimation(int Direction)
{
    if (animationPage_ == index_)
        animation_->Step(Direction);
}
void ImageReadingView::ToggleAnimation()
{
    if (!book_ || animationPending_)
        return;
    if (animationPage_ == index_)
    {
        animation_->Play(!animation_->Playing());
        return;
    }
    animationPending_ = true;
    const auto Book = book_;
    const auto Index = index_;
    const auto Generation = bookGeneration_;
    const auto Stop = bookStop_;
    auto *Watcher = new QFutureWatcher<storage::Result<ImagePageContent>>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this,
            [this, Watcher, Index, Generation]
            {
                auto Result = Watcher->result();
                Watcher->deleteLater();
                if (Generation != bookGeneration_)
                    return;
                animationPending_ = false;
                if (Index != index_)
                    return;
                if (!Result.HasValue())
                {
                    error_ = QString::fromStdString(Result.Failure().code_);
                    changed();
                    return;
                }
                animationPage_ = Index;
                animationGeneration_ = bookGeneration_;
                animation_->Load(Result.Value().bytes_);
                animation_->Play(true);
            });
    Watcher->setFuture(QtConcurrent::run(
        [Book, Index, Stop]
        {
            storage::OperationContext C;
            C.stopToken_ = Stop->get_token();
            return ReadImagePage(Book, Index, C);
        }));
}
QJsonObject ImageReadingView::State() const
{
    double Relative = 0;
    if (book_ && !sizes_.isEmpty())
    {
        qreal Y = 0;
        if (mode_ == Webtoon)
            for (int I = 0; I < index_; ++I)
                Y += viewport()->width() * zoom_ * sizes_[I].height() / qMax(1, sizes_[I].width()) + gap_;
        const auto Height =
            mode_ == Webtoon ? viewport()->width() * zoom_ * sizes_[index_].height() / qMax(1, sizes_[index_].width())
                             : extent_;
        Relative = Height > 0 ? qBound(0., (scrollPosition() - Y) / Height, 1.) : 0;
    }
    return {{"page", index_},
            {"pageCount", book_ ? book_->pages_.size() : 0},
            {"pageName", CurrentName()},
            {"book", book_ ? book_->path_ : QString{}},
            {"mode", int(mode_)},
            {"relative", mode_ == Webtoon ? relative_ : Relative},
            {"rtl", rtl_},
            {"cover", cover_},
            {"split", split_},
            {"part", splitPart_},
            {"filter", filter_},
            {"zoom", zoom_},
            {"automaticNext", automaticNext_},
            {"fit", fit_},
            {"gap", gap_},
            {"brightness", brightness_},
            {"contrast", contrast_},
            {"gamma", gamma_},
            {"sharpness", sharpness_},
            {"grayscale", grayscale_},
            {"invert", invert_},
            {"bookmark", bookmark_},
            {"private", private_},
            {"rendering", IsBusy()},
            {"error", error_}};
}
void ImageReadingView::persist()
{
    if (!book_ || book_->pages_.isEmpty() || private_ ||
        QCoreApplication::instance()->property("privateReading").toBool())
        return;
    // No blocking GUI wait: retry a contended interprocess merge on the next timer tick.
    QLockFile Lock(historyFile() + ".lock");
    if (!Lock.tryLock(0))
    {
        saveTimer_.start();
        return;
    }
    QFile File(historyFile());
    QJsonObject Root;
    if (File.exists())
    {
        if (!File.open(QIODevice::ReadOnly))
        {
            error_ = tr("읽기 기록을 열 수 없습니다.");
            emit Changed();
            return;
        }
        const auto Doc = QJsonDocument::fromJson(File.readAll());
        File.close();
        if (!Doc.isObject())
        {
            error_ = tr("읽기 기록이 손상되어 저장을 보류했습니다.");
            emit Changed();
            return;
        }
        Root = Doc.object();
    }
    auto Entry = State();
    Entry["revision"] = book_->revision_;
    Entry["pageId"] = book_->pages_[index_].id_;
    Entry["saved"] = double(QDateTime::currentMSecsSinceEpoch());
    Root[book_->path_] = Entry;
    while (Root.size() > 500)
    {
        QString Oldest;
        double Time = (std::numeric_limits<double>::max)();
        for (auto I = Root.constBegin(); I != Root.constEnd(); ++I)
        {
            const auto T = I.value().toObject()["saved"].toDouble();
            if (T < Time)
            {
                Time = T;
                Oldest = I.key();
            }
        }
        Root.remove(Oldest);
    }
    QSaveFile Out(historyFile());
    const auto Bytes = QJsonDocument(Root).toJson(QJsonDocument::Compact);
    if (!Out.open(QIODevice::WriteOnly) || Out.write(Bytes) != Bytes.size() || !Out.commit())
    {
        error_ = tr("읽기 기록을 저장할 수 없습니다.");
        emit Changed();
    }
}
bool ImageReadingView::IsBusy() const
{
    return workers_ > 0 || volumePending_ || animationPending_ || animation_->IsBusy();
}
void ImageReadingView::resetAnimation()
{
    animationPage_ = -1;
    animationGeneration_ = 0;
    animation_->Clear();
}
void ImageReadingView::SetGap(int Value)
{
    gap_ = qBound(0, Value, 200);
    rebuild();
    changed();
}
void ImageReadingView::SetPrivate(bool Enabled)
{
    private_ = Enabled;
    if (Enabled)
        saveTimer_.stop();
    else
        changed();
}
void ImageReadingView::Bookmark()
{
    if (book_)
    {
        bookmark_ = State();
        bookmark_.remove("bookmark");
        changed();
    }
}
void ImageReadingView::OpenBookmark()
{
    if (!bookmark_.isEmpty())
        GoTo(bookmark_["page"].toInt(), bookmark_["relative"].toDouble());
}
void ImageReadingView::mousePressEvent(QMouseEvent *E)
{
    if (E->button() == Qt::LeftButton)
    {
        dragging_ = true;
        dragPoint_ = E->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        E->accept();
    }
    else
        QAbstractScrollArea::mousePressEvent(E);
}
void ImageReadingView::mouseMoveEvent(QMouseEvent *E)
{
    if (dragging_)
    {
        const auto Delta = E->pos() - dragPoint_;
        dragPoint_ = E->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - Delta.x());
        setScrollPosition(scrollPosition() - Delta.y());
        E->accept();
    }
    else
        QAbstractScrollArea::mouseMoveEvent(E);
}
void ImageReadingView::mouseReleaseEvent(QMouseEvent *E)
{
    if (dragging_)
    {
        dragging_ = false;
        viewport()->unsetCursor();
        E->accept();
    }
    else
        QAbstractScrollArea::mouseReleaseEvent(E);
}
void ImageReadingView::SavePosition()
{
    saveTimer_.stop();
    persist();
}
void ImageReadingView::changed()
{
    suspended_ = false;
    viewport()->update();
    saveTimer_.start();
    emit Changed();
}
} // namespace hc::document
