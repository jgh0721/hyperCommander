#include "stdafx.h"
#include "imageReadingView.hpp"
#include <QAction>
#include "UniqueLibs/qtMessageDialog.hpp"
#include "imageAsync.hpp"
#include "imageResampler.hpp"
#include "imageDisplayColor.hpp"
#include "imageFramePlayer.hpp"
#include "imagePerformance.hpp"
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
struct ImageProgress { std::mutex mutex_; QImage image_; QSize size_; };
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
                ++contentVersions_[index_];
                invalidateRenders();
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
                    trimSourceWork();
                    resetAnimation();
                    currentImage_ = {};
                }
                saveTimer_.start();
                viewport()->update();
                emit Changed();
            });
    auto* Retry=new QAction(tr("이 페이지 다시 읽기"),this);Retry->setObjectName("image.retryPage");
    addAction(Retry);connect(Retry,&QAction::triggered,this,&ImageReadingView::RetryPage);
    auto* Next=new QAction(tr("다음 페이지"),this);Next->setObjectName("image.nextReadablePage");
    addAction(Next);connect(Next,&QAction::triggered,this,[this]{Move(1,true);});
    setContextMenuPolicy(Qt::ActionsContextMenu);
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
    volumePending_ = true;
    const auto Generation = bookGeneration_;
    const auto Stop = bookStop_;
    ++workers_;
    using OpenResult = storage::Result<std::pair<std::shared_ptr<ImageBook>, int>>;
    auto *Watcher = new QFutureWatcher<OpenResult>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this,
            [this, Watcher, Generation, RequestedMode]
            {
                auto Result = Watcher->result();
                DeleteCompletedImageWork(Watcher);
                --workers_;
                if (Generation != bookGeneration_)
                    return;
                volumePending_ = false;
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
    Watcher->setFuture(QtConcurrent::run(imagePool_.get(),
        [Path, Entry, Stop]() -> OpenResult
        {
            storage::OperationContext Context;
            Context.stopToken_ = Stop->get_token();
            auto Book = OpenImageBook(Path, Context, false);
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
    knownSizes_.clear(); contentVersions_.clear(); previousGroups_.clear();
    presentedTiles_.clear(); presentedPage_ = presentedLastPage_ = -1;
    renderRequests_ = sourceReads_ = presentedFrames_ = 0;
    pending_.clear();
    pageErrors_.clear();pageWarnings_.clear();
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
        knownSizes_.insert(Start);
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
    cover_ = AppSettings().value("imageView/comic/coverAlone", true).toBool();
    if (index_ != Start)
        currentImage_ = {};
    rebuild(false);
    GoTo(qBound(0, index_, int(book_->pages_.size()) - 1), Restored ? Entry["relative"].toDouble() : 0);
    changed();
    if(book_->subpagesPending_)expandSubpages();
}
void ImageReadingView::expandSubpages()
{
    const auto Path=book_->path_;const auto Generation=bookGeneration_;const auto Stop=bookStop_;
    auto* Watcher=new QFutureWatcher<storage::Result<std::shared_ptr<ImageBook>>>(this);
    connect(Watcher,&QFutureWatcherBase::finished,this,[this,Watcher,Generation]{
        auto Result=Watcher->result();DeleteCompletedImageWork(Watcher);
        if(Generation!=bookGeneration_)return;
        if(!Result.HasValue()){book_->subpagesPending_=false;book_->listingWarning_=QString::fromStdString(Result.Failure().code_);changed();return;}
        const auto Name=CurrentName();const auto Image=CurrentImage();const auto Relative=State()["relative"].toDouble();
        int Index=0;for(int I=0;I<Result.Value()->pages_.size();++I)if(Result.Value()->pages_[I].name_==Name){Index=I;break;}
        // Replace the immutable catalog; outstanding readers retain the old one.
        Cancel();book_=std::move(Result.Value());index_=Index;currentImage_=Image;
        sources_.clear();tiles_.clear();pageErrors_.clear();pageWarnings_.clear();
        knownSizes_.clear(); contentVersions_.clear(); previousGroups_.clear();
        presentedTiles_.clear(); presentedPage_ = presentedLastPage_ = -1;
        sizes_.fill(QSize(1000,1400),book_->pages_.size());
        if(!Image.isNull()){knownSizes_.insert(Index);sizes_[Index]=Image.size();sources_.insert(Index,new QImage(Image),qMax(1,int(Image.sizeInBytes()/1024)));}
        rebuild(false);GoTo(Index,Relative);changed();
    });
    Watcher->setFuture(QtConcurrent::run(imagePool_.get(),[Path,Stop]{
        storage::OperationContext C;C.stopToken_=Stop->get_token();return OpenImageBook(Path,C,true);
    }));
}
void ImageReadingView::beginTransition()
{
    if (!transitionPending_) transitionClock_.start();
    transitionPending_ = true;
    QTimer::singleShot(150, this, [this] { if (transitionPending_) viewport()->update(); });
}
void ImageReadingView::invalidateRenders()
{
    stop_->request_stop();
    stop_ = std::make_shared<std::stop_source>();
    ++generation_; previews_.clear(); pending_.clear();
    // Keep the last complete group and reusable renders while the next group
    // is prepared. The generation identifies work, not cached pixel identity.
    beginTransition();
}
void ImageReadingView::trimSourceWork()
{
    for (auto At = sourceStops_.begin(); At != sourceStops_.end(); ++At)
        if (At.key() < index_ - 2 || At.key() > index_ + 3) At.value()->request_stop();
}

void ImageReadingView::RetryPage()
{
    if(!book_ || index_<0 || index_>=book_->pages_.size())return;
    // Re-enumerate on an explicit retry so repaired files get a fresh revision;
    // a transient failure never silently discards the rest of the book.
    OpenPath(book_->path_,book_->pages_[index_].name_,int(mode_));
}

void ImageReadingView::Cancel()
{
    for (const auto& Stop : sourceStops_) Stop->request_stop();
    sourceStops_.clear();
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
    if (mode_ == Value) return;
    mode_ = Value;
    previousGroups_.clear(); presentedTiles_.clear(); presentedPage_ = -1;
    splitPart_ = 0;
    rebuild();
    changed();
}
void ImageReadingView::SetFilter(const QString &Id)
{
    filter_ = Id == "auto" || ImageFilterRegistry::Resolve(Id, {}).kernel_ ? Id : QString("auto");
    invalidateRenders();
    changed();
}
void ImageReadingView::SetRightToLeft(bool Enabled)
{
    rtl_ = Enabled;
    previousGroups_.clear();
    rebuild();
    changed();
}
void ImageReadingView::SetCover(bool Enabled)
{
    cover_ = Enabled;
    AppSettings Settings; Settings.setValue("imageView/comic/coverAlone", Enabled);
    previousGroups_.clear();
    rebuild();
    changed();
}
void ImageReadingView::SetSplit(bool Enabled)
{
    split_ = Enabled;
    previousGroups_.clear();
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
int ImageReadingView::groupCount(int Index) const
{
    if (mode_ != Comic || split_ || (cover_ && Index == 0) || viewport()->width() < viewport()->height()
        || Index < 0 || Index + 1 >= sizes_.size() || !knownSizes_.contains(Index) || !knownSizes_.contains(Index+1)) return 1;
    const auto Portrait = [this](int Page) { const auto S = sizes_[Page]; return S.width() < S.height(); };
    return Portrait(Index) && Portrait(Index+1) && !pageErrors_.contains(Index) && !pageErrors_.contains(Index+1) ? 2 : 1;
}
bool ImageReadingView::layoutReady(int Index) const
{
    if (!book_ || Index < 0 || Index >= sizes_.size()) return false;
    if (pageErrors_.contains(Index)) return true;
    if (!knownSizes_.contains(Index)) return false;
    const auto S = sizes_[Index];
    return mode_ != Comic || split_ || (cover_ && Index == 0) || viewport()->width() < viewport()->height()
        || S.width() >= S.height() || Index+1 >= sizes_.size() || knownSizes_.contains(Index+1) || pageErrors_.contains(Index+1);
}
void ImageReadingView::rememberSize(int Page, QSize Size)
{
    if (!Size.isValid() || Page < 0 || Page >= sizes_.size()) return;
    const bool ChangedSize = !knownSizes_.contains(Page) || sizes_[Page] != Size;
    knownSizes_.insert(Page); sizes_[Page] = Size;
    if (ChangedSize && (mode_ == Webtoon || Page == index_ || Page == index_+1)) rebuild();
}
QList<ImageReadingView::Placement> ImageReadingView::placements(int Index) const
{
    if (Index < 0) Index = index_;
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
            {
                // Round shared document edges before scrolling. Adjacent images
                // then share one physical pixel boundary and scrolling never
                // changes the render size/cache key by a pixel.
                const qreal Dpr=devicePixelRatioF();
                const auto Top=qRound64(Y*Dpr),Bottom=qRound64((Y+Height)*Dpr),Scroll=qRound64(YScroll*Dpr);
                const auto Left=qRound64((qMax(qreal(0),(W-Width)/2)-XScroll)*Dpr);
                Out << Placement{I,QRectF(Left/Dpr,(Top-Scroll)/Dpr,qRound64(Width*Dpr)/Dpr,(Bottom-Top)/Dpr),QRect(QPoint(),S)};
            }
            Y += Height + gap_;
        }
        return Out;
    }
    const auto S = sizes_[Index];
    const bool Wide = S.width() >= S.height();
    const int Count = groupCount(Index);
    qreal TotalWidth = 0, MaxHeight = 0;
    for (int N = 0; N < Count; ++N) MaxHeight = qMax(MaxHeight, qreal(sizes_[Index + N].height()));
    for (int N = 0; N < Count; ++N) {
        const auto Sz = sizes_[Index + N];
        TotalWidth += (split_ && Wide ? qMax(1, Sz.width()/2) : Sz.width())
            * (Count == 2 ? MaxHeight / Sz.height() : 1.);
    }
    qreal Scale = fit_ == 0   ? 1.
                  : fit_ == 2 ? W / TotalWidth
                  : fit_ == 3 ? H / MaxHeight
                              : qMin(W / TotalWidth, H / MaxHeight);
    Scale *= zoom_;
    qreal X = qMax(qreal(0), (W - TotalWidth * Scale) / 2) - XScroll;
    for (int N = 0; N < Count; ++N)
    {
        const int I = Index + (rtl_ ? Count - 1 - N : N);
        const auto Size = sizes_[I];
        QRect Crop(QPoint(), Size);
        if (split_ && Wide)
        {
            const int Half = Size.width() / 2;
            const bool Right = ((Index == index_ ? splitPart_ : 0) == 0) == rtl_;
            Crop = QRect(Right ? Half : 0, 0, Right ? Size.width() - Half : Half, Size.height());
        }
        const qreal PageScale = Scale * (Count == 2 ? MaxHeight / Size.height() : 1.);
        const qreal Width = Crop.width() * PageScale, Height = Crop.height() * PageScale;
        Out << Placement{I, QRectF(X, qMax(qreal(0), (H - Height) / 2) - YScroll, Width, Height), Crop};
        X += Width;
    }
    return Out;
}
void ImageReadingView::rebuild(bool KeepAnchor)
{
    const auto Old = State();
    displayColor_ = ImageDisplayColorSpace(screen());
    displayColorKey_ = QString::fromLatin1(QCryptographicHash::hash(displayColor_.iccProfile(), QCryptographicHash::Sha256).toHex());
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
    if (KeepAnchor)
        GoTo(index_, Old["relative"].toDouble());
    viewport()->update();
}
QString ImageReadingView::renderKey(const Placement &P, const QRect &Tile) const
{
    const auto Output = (P.rect_.size() * devicePixelRatioF()).toSize();
    QString Key = QString::number(bookGeneration_) + ':' + QString::number(P.page_) + ':' + QString::number(contentVersions_.value(P.page_));
    for (const int Value : {P.crop_.x(), P.crop_.y(), P.crop_.width(), P.crop_.height(), Output.width(), Output.height(),
                           Tile.x(), Tile.y(), Tile.width(), Tile.height()}) Key += ':' + QString::number(Value);
    for (const double Value : {devicePixelRatioF(), brightness_, contrast_, gamma_, sharpness_}) Key += ':' + QString::number(Value, 'g', 17);
    return Key + ':' + filter_ + ':' + displayColorKey_ + (grayscale_ ? ":gray" : ":rgb") + (invert_ ? ":invert" : ":normal");
}
void ImageReadingView::requestSource(int Page)
{
    if (suspended_ || !book_ || Page < 0 || Page >= sizes_.size() || pageErrors_.contains(Page)) return;
    if (auto* Source = sources_.object(Page)) { rememberSize(Page, Source->size()); return; }
    if (sourceStops_.contains(Page) || sourceStops_.size() >= 2) return;
    auto Stop = std::make_shared<std::stop_source>(); sourceStops_.insert(Page, Stop);
    const auto Book = book_; const auto Generation = bookGeneration_;
    auto Progress = std::make_shared<ImageProgress>();
    auto* Watcher = new QFutureWatcher<storage::Result<ImagePageContent>>(this);
    auto* Timer = new QTimer(Watcher); Timer->setInterval(15);
    connect(Timer, &QTimer::timeout, this, [this, Progress, Generation, Page] {
        if (Generation != bookGeneration_) return;
        QSize Size; QImage Preview;
        { std::lock_guard Lock(Progress->mutex_); Size = Progress->size_; Preview = std::exchange(Progress->image_, {}); }
        rememberSize(Page, Size);
        if (!Preview.isNull()) previews_[Page] = std::move(Preview);
        viewport()->update();
    });
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Timer, Generation, Page, Stop] {
        Timer->stop(); auto Result = Watcher->result(); DeleteCompletedImageWork(Watcher);
        if (Generation != bookGeneration_) return;
        sourceStops_.remove(Page); previews_.remove(Page);
        if (Result.HasValue()) {
            const auto& Image = Result.Value().image_;
            if (!Result.Value().warning_.isEmpty()) pageWarnings_[Page] = Result.Value().warning_;
            sources_.insert(Page, new QImage(Image), qMax(1, int(Image.sizeInBytes()/1024)));
            if (Page == index_) currentImage_ = Image;
            rememberSize(Page, Image.size());
        } else if (!Stop->stop_requested()) pageErrors_[Page] = QString::fromStdString(Result.Failure().code_);
        viewport()->update(); emit Changed();
    });
    ++sourceReads_; Timer->start();
    Watcher->setFuture(QtConcurrent::run(imagePool_.get(), [Book, Page, Stop, Progress] {
        storage::OperationContext C; C.stopToken_ = Stop->get_token();
        return ReadImagePage(Book, Page, C,
            [Progress](QImage Image) { std::lock_guard Lock(Progress->mutex_); Progress->image_ = std::move(Image); },
            [Progress](QSize Size) { std::lock_guard Lock(Progress->mutex_); Progress->size_ = Size; });
    }));
}
void ImageReadingView::request(const Placement &P, const QRect &Tile)
{
    const auto CacheKey = renderKey(P, Tile);
    const auto PendingKey = QString::number(generation_) + ':' + CacheKey;
    if (suspended_ || pending_.contains(PendingKey) || pageErrors_.contains(P.page_) || tiles_.contains(CacheKey)) return;
    const auto Source = P.page_ == index_ && !currentImage_.isNull() ? currentImage_
        : sources_.object(P.page_) ? *sources_.object(P.page_) : QImage{};
    if (Source.isNull()) { requestSource(P.page_); return; }
    if (workers_ >= 2) return;
    pending_.insert(PendingKey); ++workers_; ++renderRequests_;
    const auto Book = book_; const auto Generation = generation_, SourceGeneration = bookGeneration_;
    const auto Stop = stop_; const auto DisplayColor = displayColor_; const auto Filter = filter_;
    const auto Output = (P.rect_.size() * devicePixelRatioF()).toSize();
    const auto Bright = brightness_, Contrast = contrast_, Gamma = gamma_, Sharp = sharpness_;
    const auto Gray = grayscale_, Invert = invert_;
    auto* Watcher = new QFutureWatcher<ImageRenderResult>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, CacheKey, PendingKey, Generation, SourceGeneration, Page=P.page_] {
        auto Result = Watcher->result(); DeleteCompletedImageWork(Watcher); --workers_;
        if (SourceGeneration != bookGeneration_) return;
        pending_.remove(PendingKey);
        if (Result.error_.isEmpty()) tiles_.insert(CacheKey, new QImage(Result.image_), qMax(1, int(Result.image_.sizeInBytes()/1024)));
        else if (Generation == generation_ && Result.error_ != "image-render-cancelled") pageErrors_[Page] = Result.error_;
        viewport()->update(); emit Changed();
    });
    Watcher->setFuture(QtConcurrent::run(imagePool_.get(), [=] {
        QImage Input = P.crop_.size() == Source.size() ? Source : Source.copy(P.crop_);
        ImageRenderRequest R; R.source_ = Input; R.traceSource_ = Book->path_ + "#" + QString::number(P.page_);
        R.targetColor_ = DisplayColor; R.outputSize_ = Output; R.filter_ = Filter; R.stop_ = Stop->get_token();
        R.tile_ = Sharp > 0 ? Tile.adjusted(-1,-1,1,1).intersected(QRect(QPoint(), Output)) : Tile;
        R.sourceToOutput_.scale(double(Output.width())/Input.width(), double(Output.height())/Input.height());
        auto Result = ResampleImage(R);
        if (Result.error_.isEmpty()) {
            Adjust(Result.image_, Bright, Contrast, Gamma, Gray, Invert, Sharp);
            if (R.tile_ != Tile) Result.image_ = Result.image_.copy(QRect(Tile.topLeft()-R.tile_.topLeft(), Tile.size()));
        }
        return Result;
    }));
}
QList<QRect> ImageReadingView::visibleTiles(const Placement &P) const
{
    QList<QRect> Tiles;
    const auto Visible = P.rect_.intersected(QRectF(viewport()->rect()));
    if (Visible.isEmpty()) return Tiles;
    const auto Dpr = devicePixelRatioF();
    const auto Output = (P.rect_.size()*Dpr).toSize();
    const int FirstX=qMax(0,int((Visible.left()-P.rect_.left())*Dpr)/TileHeight);
    const int FirstY=qMax(0,int((Visible.top()-P.rect_.top())*Dpr)/TileHeight);
    const int LastX=qMax(FirstX,int((Visible.right()-P.rect_.left())*Dpr)/TileHeight);
    const int LastY=qMax(FirstY,int((Visible.bottom()-P.rect_.top())*Dpr)/TileHeight);
    for(int Y=FirstY;Y<=LastY;++Y) for(int X=FirstX;X<=LastX;++X) {
        const QRect Tile(X*TileHeight,Y*TileHeight,qMin(TileHeight,Output.width()-X*TileHeight),qMin(TileHeight,Output.height()-Y*TileHeight));
        if(!Tile.isEmpty()) Tiles.append(Tile);
    }
    return Tiles;
}
bool ImageReadingView::paintGroup(QPainter &Painter)
{
    if (!knownSizes_.contains(index_)) requestSource(index_);
    if (!layoutReady(index_) && knownSizes_.contains(index_)) requestSource(index_+1);
    const auto Places = placements();
    bool Ready = layoutReady(index_); QString Error;
    QList<PresentedTile> Next;
    const auto Dpr = devicePixelRatioF();
    for (const auto& P : Places) {
        if (pageErrors_.contains(P.page_)) { Error = pageErrors_.value(P.page_); Ready = false; continue; }
        if (!layoutReady(index_)) continue;
        for (const auto& Tile : visibleTiles(P)) {
            if (auto* Image = tiles_.object(renderKey(P,Tile)))
                Next.append({QRectF(P.rect_.left()+Tile.x()/Dpr,P.rect_.top()+Tile.y()/Dpr,Tile.width()/Dpr,Tile.height()/Dpr),*Image});
            else { Ready=false; request(P,Tile); }
        }
    }
    if (Ready && !Next.isEmpty()) {
        const bool NewFrame = presentedGeneration_ != generation_ || presentedPage_ != index_ || transitionPending_;
        presentedTiles_ = std::move(Next); presentedPage_ = index_; presentedLastPage_ = index_;
        for (const auto& P : Places) presentedLastPage_ = qMax(presentedLastPage_,P.page_);
        presentedGeneration_ = generation_; transitionPending_ = false;
        int Pinned=0; for (const auto& Tile : presentedTiles_) Pinned+=qMax(1,int(Tile.image_.sizeInBytes()/1024));
        tiles_.setMaxCost(qMax(32*1024,160*1024-Pinned));
        if (NewFrame) {
            ++presentedFrames_; lastTransitionMs_=transitionClock_.isValid()?transitionClock_.nsecsElapsed()/1e6:0;
            const auto Generation=generation_;
            QTimer::singleShot(0,this,[this,Generation] { if(Generation==generation_){emit FramePresented(presentedPage_);emit Changed();} });
        }
    }
    // On the initial open there is no previous group to preserve. Show the
    // available pixels while high-quality tiles are prepared, for every codec.
    // Page turns still keep the previous complete group until atomic replacement.
    if(presentedTiles_.isEmpty() && layoutReady(index_)) {
        for(const auto& P:Places) {
            auto Preview=previews_.value(P.page_);
            if(Preview.isNull())Preview=P.page_==index_?CurrentImage():sources_.object(P.page_)?*sources_.object(P.page_):QImage{};
            if(!Preview.isNull())Painter.drawImage(P.rect_,Preview);
        }
    }
    for (const auto& Tile : presentedTiles_) Painter.drawImage(Tile.rect_,Tile.image_);
    if (!Error.isEmpty()) {
        transitionPending_=false;
        const QRect Banner(0,0,viewport()->width(),64);Painter.fillRect(Banner,palette().brush(QPalette::Base));
        Painter.setPen(palette().color(QPalette::Text));
        Painter.drawText(Banner.adjusted(8,2,-8,-2),Qt::AlignLeft|Qt::AlignVCenter|Qt::TextWordWrap,
            tr("%1 페이지를 읽지 못했습니다. 다시 읽거나 다른 페이지로 이동할 수 있습니다. %2").arg(index_+1).arg(Error));
    } else if (transitionPending_ && transitionClock_.elapsed() >= 150) {
        Painter.fillRect(QRect(8,8,200,30),palette().brush(QPalette::Base));Painter.setPen(palette().color(QPalette::Text));
        Painter.drawText(QRect(16,8,184,30),Qt::AlignVCenter,tr("%1 페이지 불러오는 중…").arg(index_+1));
    }
    return Ready;
}
void ImageReadingView::prefetch()
{
    if (suspended_ || !book_ || workers_ || !sourceStops_.isEmpty() || animation_->Playing()) return;
    if (mode_ != Webtoon && (transitionPending_ || presentedPage_ != index_ || pageErrors_.contains(index_))) return;
    const int Next=index_+groupCount(index_), Previous=previousGroups_.value(index_,index_>=2 && groupCount(index_-2)==2 ? index_-2 : index_-1);
    const QList<int> Groups = direction_ > 0 ? QList<int>{Next,Previous} : QList<int>{Previous,Next};
    for (const int Page : Groups) {
        if (Page < 0 || Page >= sizes_.size() || Page == index_ || pageErrors_.contains(Page)) continue;
        if (!sources_.contains(Page) && !pageErrors_.contains(Page)) { requestSource(Page); return; }
        if (!layoutReady(Page)) { requestSource(Page+1); return; }
        if (mode_ == Webtoon) continue;
        for (const auto& P : placements(Page)) {
            if (!sources_.contains(P.page_) && !pageErrors_.contains(P.page_)) { requestSource(P.page_); return; }
            for (const auto& Tile : visibleTiles(P))
                if (!tiles_.contains(renderKey(P,Tile))) { request(P,Tile); if(workers_>=2)return; }
        }
    }
}

void ImageReadingView::paintEvent(QPaintEvent *)
{
    QTimer::singleShot(0,this,&ImageReadingView::prefetch);
    setToolTip(book_ && !book_->listingWarning_.isEmpty()?tr("일부 목록만 확인했습니다: %1").arg(book_->listingWarning_):QString{});
    QPainter Painter(viewport());
    Painter.fillRect(viewport()->rect(), palette().brush(QPalette::Base));
    if (!book_)
    {
        Painter.setPen(palette().color(QPalette::Text));
        Painter.drawText(viewport()->rect(), Qt::AlignCenter, error_.isEmpty() ? tr("불러오는 중…") : error_);
        return;
    }
    if (mode_ != Webtoon) {
        paintGroup(Painter);
        if (!pageWarnings_.value(index_).isEmpty()) {
            Painter.setPen(palette().color(QPalette::Text));
            Painter.drawText(QRect(8,viewport()->height()-32,viewport()->width()-16,28),tr("손상된 파일에서 복구한 이미지입니다."));
        }
        return;
    }
    const qreal Dpr = devicePixelRatioF();
    for (const auto &P : placements())
    {
        const auto Visible = P.rect_.intersected(QRectF(viewport()->rect()));
        if (Visible.isEmpty())
            continue;
        // Paint one continuous base before any finished tiles. A missing tile
        // must not repaint the whole page over tiles already drawn this frame.
        auto Preview=previews_.value(P.page_);
        if(Preview.isNull())if(auto* Source=sources_.object(P.page_))Preview=*Source;
        if(!Preview.isNull()) {
            Painter.save();Painter.setClipRect(Visible);Painter.drawImage(P.rect_,Preview);Painter.restore();
        }
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
                const auto CacheKey = renderKey(P, Tile);
                if (auto *Image = tiles_.object(CacheKey))
                    Painter.drawImage(QRectF(P.rect_.left() + Tile.x() / Dpr, P.rect_.top() + Tile.y() / Dpr,
                                             Tile.width() / Dpr, Tile.height() / Dpr),
                                      *Image);
                else
                {
                    if(Preview.isNull())
                    {
                        Painter.setPen(palette().color(QPalette::Text));
                        Painter.drawText(Visible, Qt::AlignCenter, pageErrors_.contains(P.page_)?tr("이 페이지를 읽지 못했습니다.\n다른 페이지로 이동하거나 마우스 오른쪽 메뉴에서 다시 읽을 수 있습니다.\n%1").arg(pageErrors_.value(P.page_)):tr("불러오는 중…"));
                    }
                    request(P, Tile);
                }
            }
    }
    if(!previews_.isEmpty() || !book_->listingWarning_.isEmpty() || !pageWarnings_.value(index_).isEmpty()){
        const QRect Banner(0,0,viewport()->width(),44);Painter.fillRect(Banner,palette().brush(QPalette::Base));
        Painter.setPen(palette().color(QPalette::Text));Painter.drawText(Banner.adjusted(8,2,-8,-2),Qt::AlignLeft|Qt::AlignVCenter|Qt::TextWordWrap,
            !previews_.isEmpty()?tr("파일을 읽는 중입니다. 임시 미리보기를 표시합니다."):tr("손상되거나 불완전한 파일에서 읽을 수 있는 내용만 표시하고 있습니다."));
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
    if (NewPage) { direction_ = Index > index_ ? 1 : -1; transitionClock_.start(); transitionPending_ = false; }
    index_ = Index; trimSourceWork();
    if (auto* Source = sources_.object(Index)) { currentImage_ = *Source; knownSizes_.insert(Index); sizes_[Index] = Source->size(); }
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
    if (split_ && knownSizes_.contains(index_) && sizes_[index_].width() >= sizes_[index_].height())
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
    if (mode_ == Comic && !OnePage && !split_ && Direction < 0) {
        const int Previous = previousGroups_.value(index_, index_ >= 2 && groupCount(index_-2) == 2 ? index_-2 : index_-1);
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
    if (Direction > 0) previousGroups_[Next] = index_;
    GoTo(Next);
    if (Direction < 0 && split_) { splitPart_ = 1; rebuild(false); }

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
                DeleteCompletedImageWork(Watcher);
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
                    Path = hc::ui::InputDialog::getItem(this, tr("다음 권 선택"), tr("같은 권의 후보가 여러 개입니다."),
                                                 Candidates, 0, false, &Ok);
                    if (!Ok)
                        return;
                }
                openVolumePath(Path);
            });
    Watcher->setFuture(
        QtConcurrent::run(imagePool_.get(),[Path, Direction, Stop] { return NextImageBooks(Path, Direction, Stop->get_token()); }));
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
                DeleteCompletedImageWork(Watcher);
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
    Watcher->setFuture(QtConcurrent::run(imagePool_.get(),
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
bool ImageReadingView::RunCommand(const QString& Id)
{
    if (Id == "image.scrollDown") setScrollPosition(scrollPosition() + 40);
    else if (Id == "image.scrollUp") setScrollPosition(scrollPosition() - 40);
    else if (Id == "image.next" || Id == "image.pageNext") Move(1);
    else if (Id == "image.previous" || Id == "image.previousSpace" || Id == "image.pagePrevious") Move(-1);
    else if (Id == "image.pageNextSingle") Move(1, true);
    else if (Id == "image.pagePreviousSingle") Move(-1, true);
    else if (Id == "image.first") GoTo(0);
    else if (Id == "image.last" && book_) GoTo(book_->pages_.size() - 1);
    else if (Id == "image.volumeNext") OpenVolume(1);
    else if (Id == "image.volumePrevious") OpenVolume(-1);
    else if (Id == "image.right") Move(rtl_ ? -1 : 1);
    else if (Id == "image.left") Move(rtl_ ? 1 : -1);
    else return false;
    return true;
}
void ImageReadingView::keyPressEvent(QKeyEvent *E)
{
    if (E->key() == Qt::Key_Down)
        setScrollPosition(scrollPosition() + 40);
    else if (E->key() == Qt::Key_Up)
        setScrollPosition(scrollPosition() - 40);
    else if (E->key() == Qt::Key_Space)
        Move(E->modifiers() & Qt::ShiftModifier ? -1 : 1);
    else if (E->key() == Qt::Key_Backspace && E->modifiers() == Qt::NoModifier)
        Move(-1);
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
                DeleteCompletedImageWork(Watcher);
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
    Watcher->setFuture(QtConcurrent::run(imagePool_.get(),
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
    int LastPage=index_;
    if(mode_!=Webtoon){const auto Places=placements();for(const auto& Place:Places)LastPage=qMax(LastPage,Place.page_);}
    return {{"presentedPage",presentedPage_},{"presentedLastPage",presentedLastPage_},
            {"presentedName",book_ && presentedPage_>=0 && presentedPage_<book_->pages_.size()?book_->pages_[presentedPage_].name_:QString{}},
            {"sourceJobs",sourceStops_.size()},{"renderJobs",workers_},
            {"transitioning",transitionPending_},{"transitionMs",lastTransitionMs_},
            {"renderRequests",double(renderRequests_)},{"sourceReads",double(sourceReads_)},{"presentedFrames",double(presentedFrames_)},
            {"lastPage",LastPage},{"page", index_},
            {"pageCount", book_ ? book_->pages_.size() : 0},
            {"listingWarning",book_?book_->listingWarning_:QString{}},{"discoveringPages",book_&&book_->subpagesPending_},
            {"pageName", CurrentName()},
            {"pageWarning",pageWarnings_.value(index_)},{"pageError",pageErrors_.value(index_)},{"failedPages",pageErrors_.size()},
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
    // The isolated host waits for idle before presenting its initially hidden window.
    // A hidden viewport cannot paint; its pending frame must not block that handshake.
    return (mode_ == Webtoon && workers_ > 0) || sourceStops_.contains(index_) || (groupCount(index_)==2 && sourceStops_.contains(index_+1))
        || (!suspended_ && book_ && isVisible() && mode_!=Webtoon && transitionPending_)
        || volumePending_ || animationPending_ || animation_->IsBusy();
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
