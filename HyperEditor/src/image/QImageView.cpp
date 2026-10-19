#include "image/QImageView.h"

#include "core/ThemeManager.h"
#include "UniqueLibs/document/imageBook.hpp"
#include "UniqueLibs/document/imageDisplayColor.hpp"
#include "UniqueLibs/document/imagePerformance.hpp"
#include "UniqueLibs/document/imageAsync.hpp"
#include "UniqueLibs/document/imageDecodeCache.hpp"
#include "UniqueLibs/document/imageColor.hpp"
#include "core/ToolbarIcons.h"
#include "utils/FileLoadHelper.h"

#include <QApplication>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QBuffer>
#include <QClipboard>
#include <QPainter>
#include <QActionGroup>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QComboBox>
#include <QColorDialog>
#include <QImageReader>
#include <QImageWriter>
#include <QInputDialog>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSizePolicy>
#include <QSpinBox>
#include <QThreadPool>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUndoCommand>
#include <QScrollBar>
#include <QShortcut>
#include <cmath>
#include <algorithm>
#include <utility>
#include <QVariant>
#include <QPainterPath>
#include <QtMath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

class ImageCanvasLabel final : public QLabel
{
public:
    using QLabel::QLabel;
    std::function<void()> presented_;
protected:
    void paintEvent(QPaintEvent* Event) override
    {
        if(pixmap().isNull()){QLabel::paintEvent(Event);return;}
        hc::document::ImageTraceSpan Trace("view.paint",property("imageSource").toString());
        QLabel::paintEvent(Event);
        if(presented_)presented_();
    }
};

struct ImageOpenResult {
    bool success = false;
    QString filePath;
    QString errorMessage;
    QString format;
    QImage image;
};

QPen makeDrawingPen(const QColor& color, int width, Qt::PenStyle style)
{
    return QPen(color, qMax(1, width), style, Qt::RoundCap, Qt::RoundJoin);
}

QString toolButtonForegroundColor(const QColor& color)
{
    return color.lightnessF() < 0.45 ? QStringLiteral("#ffffff") : QStringLiteral("#111111");
}

QString imageFormatText(QImage::Format format, bool hasAlpha)
{
    switch (format) {
        case QImage::Format_Grayscale8:
        case QImage::Format_Indexed8:
            return QObject::tr("Grayscale 8-bit");
        case QImage::Format_RGB32:
        case QImage::Format_RGB888:
        case QImage::Format_BGR888:
        case QImage::Format_RGBX8888:
        case QImage::Format_RGBX64:
            return QObject::tr("RGB %1-bit").arg(QImage(1, 1, format).depth());
        default:
            break;
    }

    if (hasAlpha)
        return QObject::tr("RGBA %1-bit").arg(QImage(1, 1, format).depth());

    return QObject::tr("%1-bit").arg(QImage(1, 1, format).depth());
}

}

// ── Undo Command ──
class ImageEditCommand : public QUndoCommand
{
public:
    ImageEditCommand(QImage* target, const QImage& before, const QImage& after, QString text)
        : QUndoCommand(std::move(text)), m_target(target), m_before(before), m_after(after) {}
    void undo() override { *m_target = m_before; }
    void redo() override { *m_target = m_after; }
private:
    QImage* m_target;
    QImage  m_before, m_after;
};

// ═══════════════════════════════════════════════════════════
QImageView::QImageView(QWidget* parent)
    : QBaseView(parent)
    , m_compositeMode(false)
    , m_compositeRotation(0.0)
    , m_activeCompositeHandle(CompHandleNone)
    , m_compositeDragStartRotation(0.0)
{
    setFocusPolicy(Qt::StrongFocus);
    m_undoStack = new QUndoStack(this);
    m_undoStack->setUndoLimit(2);
    m_slideshowTimer = new QTimer(this);
    m_penColor = ThemeManager::instance().color(QStringLiteral("image.draw"));

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName(QStringLiteral("canvasScrollArea"));
    auto* Canvas=new ImageCanvasLabel(this);
    Canvas->presented_=[this]{emit imagePresented(displayFinal_);};
    m_imageLabel = Canvas;
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setMouseTracking(true);
    m_imageLabel->installEventFilter(this);  // 마우스 이벤트를 여기서 처리
    m_scrollArea->setWidget(m_imageLabel);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setAlignment(Qt::AlignCenter);
    m_scrollArea->viewport()->installEventFilter(this);  // 스크롤 영역 이벤트도 처리

    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]{
        if (m_watermarkOverlay) m_watermarkOverlay->update();
    });
    connect(m_scrollArea->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]{
        if (m_watermarkOverlay) m_watermarkOverlay->update();
    });

    layout()->addWidget(m_scrollArea);

    connect(m_slideshowTimer, &QTimer::timeout, this, &QImageView::nextImage);
    for (const auto Key : {Qt::Key_Space, Qt::Key_Backspace}) {
        auto* Shortcut = new QShortcut(QKeySequence(Key), this);
        Shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(Shortcut, &QShortcut::activated, this, [this, Key] {
            if (m_compositeMode || pendingEdit_) return;
            if (Key == Qt::Key_Space) nextImage();
            else previousImage();
        });
    }
}

QImageView::~QImageView() { renderStop_->request_stop();previewStop_->request_stop();directoryStop_->request_stop();editStop_->request_stop();stopSlideshow(); }

// ═══════════════════════════════════════════════════════════
bool QImageView::openImage(const QImage& image, const QString& title)
{
    if (image.isNull())
        return false;

    stopSlideshow();
    directoryStop_->request_stop();++navigationGeneration_;directoryLoading_=false;pendingDirectorySteps_=0;

    m_originalImage = image;
    m_displayImage  = image;
    m_filePath.clear();
    m_imageFormat = tr("Clipboard");
    setDisplayTitle(title.isEmpty() ? tr("클립보드 이미지") : title);
    setModifiedState(title.isEmpty() == true);
    m_undoStack->clear();
    m_dirFiles.clear();
    m_dirIndex = 0;

    if (isVisible()) {
        updateDisplay();
    } else {
        QMetaObject::invokeMethod(this, &QImageView::updateDisplay, Qt::QueuedConnection);
    }

    emit titleChanged(this->title());
    emit imageChanged();
    updateCopyAvailability();
    return true;
}

bool QImageView::openBlankCanvas(const QSize& size, const QColor& fillColor, const QString& title)
{
    const QSize effectiveSize = size.expandedTo(QSize(1, 1));
    QImage canvas(effectiveSize, QImage::Format_RGB32);
    if (canvas.isNull())
        return false;

    canvas.fill(fillColor);

    if (!openImage(canvas, title.isEmpty() ? tr("제목없음") : title))
        return false;

    m_imageFormat = tr("캔버스");
    return true;
}

// ═══════════════════════════════════════════════════════════
bool QImageView::openFile(const QString& filePath)
{
    Q_UNUSED(filePath);
    return false;
}

bool QImageView::saveFile(const QString& filePath)
{
    Q_UNUSED(filePath);
    return false;
}

bool QImageView::saveFileAs()
{
    emit SaveRequested(); return false;
}

void QImageView::closeFile()
{
    directoryStop_->request_stop();++navigationGeneration_;directoryLoading_=false;pendingDirectorySteps_=0;
    m_openRequestId.fetch_add(1);
    endLoading();
    m_originalImage = {}; m_displayImage = {};
    m_filePath.clear(); setModifiedState(false);
    m_imageFormat.clear();
    setDisplayTitle({});
    m_dirFiles.clear();
    m_dirIndex = 0;
    m_undoStack->clear(); m_imageLabel->clear();
    stopSlideshow();
    emit fileClosed();
    updateCopyAvailability();
}

bool QImageView::isModified() const { return m_modified; }
QStringList QImageView::supportedExtensions() const
{
    return hc::document::ImageExtensions();
}

bool QImageView::opensFileAsynchronously() const
{
    return true;
}

void QImageView::setTheme(Theme theme)
{
    QBaseView::setTheme(theme);
    updateDisplay();
}

QList<QRectF> QImageView::visiblePageRects(qreal& scaleRatio) const
{
    if (m_displayImage.isNull() || !m_imageLabel || !m_scrollArea || !isVisible() || !m_imageLabel->isVisible()) {
        scaleRatio = 1.0;
        return {};
    }

    const QPixmap pixmap = m_imageLabel->pixmap();
    if (pixmap.isNull()) {
        scaleRatio = 1.0;
        return {};
    }

    QImage scaled = scaledImage();
    if (scaled.isNull() || m_displayImage.isNull()) {
        scaleRatio = 1.0;
        return {};
    }

    if (m_displayImage.width() <= 0 || m_displayImage.height() <= 0) {
        scaleRatio = 1.0;
        return {};
    }

    scaleRatio = static_cast<qreal>(scaled.width()) / m_displayImage.width();

    // 이미지 라벨의 표시 영역 (QPixmap이 중앙 정렬되므로 오프셋 필요)
    QSize labelSize = m_imageLabel->size();
    QSize pmSize = pixmap.deviceIndependentSize().toSize();
    if (pmSize.isEmpty())
        pmSize = pixmap.size() / qMax(1.0, pixmap.devicePixelRatio());
    if (labelSize.isEmpty() || pmSize.isEmpty()) {
        scaleRatio = 1.0;
        return {};
    }

    int offsetX = (labelSize.width() - pmSize.width()) / 2;
    int offsetY = (labelSize.height() - pmSize.height()) / 2;

    QPoint globalPos = m_imageLabel->mapToGlobal(QPoint(offsetX, offsetY));
    QPoint viewPos = mapFromGlobal(globalPos);

    QRect rectInView(viewPos, pmSize);
    return { rectInView };
}

// ═══════════════════════════════════════════════════════════
// 줌/맞춤
void QImageView::setFitMode(FitMode mode) { m_fitMode = mode; updateDisplay(); emit viewStateChanged(); }
QImageView::FitMode QImageView::fitMode() const { return m_fitMode; }
void QImageView::setZoomLevel(qreal pct)  { m_zoomLevel = qBound(10.0, pct, 1600.0); m_fitMode = OriginalSize; updateDisplay(); emit viewStateChanged(); }
qreal QImageView::zoomLevel() const       { return m_zoomLevel; }

bool QImageView::canCopy() const
{
    return !m_displayImage.isNull();
}

bool QImageView::copySelection()
{
    if(ClipboardRestricted()){emit ClipboardRequested("copy");return true;}
    if (!canCopy())
        return false;

    if (auto* clipboard = QApplication::clipboard()) {
        clipboard->setImage(m_displayImage);
        return true;
    }
    return false;
}

// ── Undo 헬퍼 ──
void QImageView::pushUndoImage()
{
    QImage before = m_displayImage;
    // 편집 후 명령을 push — 호출자가 m_displayImage를 변경한 후 push해야 하므로
    // 실제 사용: before 저장 → 편집 → pushUndoImage에서 after로 현재 이미지 캡처
    // 여기선 간소화: 호출 전의 이미지를 저장
    // (실제로는 편집 전에 호출)
}

void QImageView::rotateClockwise()
{
    QImage before = m_displayImage;
    QTransform t; t.rotate(90);
    m_displayImage = m_displayImage.transformed(t);
    m_undoStack->push(new ImageEditCommand(&m_displayImage, before, m_displayImage, "Rotate 90°"));
    setModifiedState(true);
    updateDisplay();
}

void QImageView::rotateCounterClockwise()
{
    QImage before = m_displayImage;
    QTransform t; t.rotate(-90);
    m_displayImage = m_displayImage.transformed(t);
    m_undoStack->push(new ImageEditCommand(&m_displayImage, before, m_displayImage, "Rotate -90°"));
    setModifiedState(true);
    updateDisplay();
}

void QImageView::flipHorizontal()
{
    QImage before = m_displayImage;
    m_displayImage = m_displayImage.mirrored(true, false);
    m_undoStack->push(new ImageEditCommand(&m_displayImage, before, m_displayImage, "Flip H"));
    setModifiedState(true);
    updateDisplay();
}

void QImageView::flipVertical()
{
    QImage before = m_displayImage;
    m_displayImage = m_displayImage.mirrored(false, true);
    m_undoStack->push(new ImageEditCommand(&m_displayImage, before, m_displayImage, "Flip V"));
    setModifiedState(true);
    updateDisplay();
}

void QImageView::setEditTool(EditTool tool)
{
    m_editTool = tool;
    if (tool == Crop || tool == FreeDraw || tool == Rectangle || tool == Ellipse || tool == Arrow)
        m_imageLabel->setCursor(Qt::CrossCursor);
    else
        m_imageLabel->setCursor(Qt::ArrowCursor);
    // Crop 선택 해제
    if (m_rubberBand) m_rubberBand->hide();
    emit viewStateChanged();
}

QImageView::EditTool QImageView::currentEditTool() const { return m_editTool; }
void QImageView::setPenColor(const QColor& c)
{
    if (c.isValid())
        m_penColor = c;
}

void QImageView::setPenWidth(int w)           { m_penWidth = qBound(1, w, 50); }
void QImageView::setPenStyle(Qt::PenStyle s)  { m_penStyle = s; }
Qt::PenStyle QImageView::penStyle() const     { return m_penStyle; }
void QImageView::setTextPixelSize(int size)   { m_textPixelSize = qBound(8, size, 256); }
int QImageView::textPixelSize() const         { return m_textPixelSize; }

void QImageView::undoEdit()
{
    m_undoStack->undo();
    setModifiedState(true);
    updateDisplay();
}

void QImageView::redoEdit()
{
    m_undoStack->redo();
    setModifiedState(true);
    updateDisplay();
}

void QImageView::cropToSelection()
{
    if (!m_rubberBand || !m_rubberBand->isVisible()) return;

    QRect selRect = m_rubberBand->geometry();
    // 위젯 좌표 → 이미지 좌표
    QPoint tl = toImageCoord(m_imageLabel->mapFrom(m_scrollArea->viewport(), selRect.topLeft()));
    QPoint br = toImageCoord(m_imageLabel->mapFrom(m_scrollArea->viewport(), selRect.bottomRight()));
    QRect imgRect = QRect(tl, br).normalized().intersected(m_displayImage.rect());

    if (imgRect.isEmpty()) return;

    QImage before = m_displayImage;
    m_displayImage = m_displayImage.copy(imgRect);
    m_undoStack->push(new ImageEditCommand(&m_displayImage, before, m_displayImage, "Crop"));
    setModifiedState(true);
    m_rubberBand->hide();
    updateDisplay();
}

QPoint QImageView::toImageCoord(const QPoint& labelPos) const
{
    const QPixmap pm = m_imageLabel->pixmap();
    if (pm.isNull() || m_displayImage.isNull())
        return labelPos;

    const QSize labelSz = m_imageLabel->size();
    const QSize pmSz    = pm.size() / pm.devicePixelRatio();

    const int offsetX = (labelSz.width()  - pmSz.width())  / 2;
    const int offsetY = (labelSz.height() - pmSz.height()) / 2;

    const qreal pxInPm = labelPos.x() - offsetX;
    const qreal pyInPm = labelPos.y() - offsetY;

    const qreal sx = static_cast<qreal>(m_displayImage.width())  / pmSz.width();
    const qreal sy = static_cast<qreal>(m_displayImage.height()) / pmSz.height();

    return QPoint(static_cast<int>(pxInPm * sx),
                  static_cast<int>(pyInPm * sy));
}

QPointF QImageView::toImageCoord(const QPointF& labelPos) const
{
    const QPixmap pm = m_imageLabel->pixmap();
    if (pm.isNull() || m_displayImage.isNull())
        return labelPos;

    const QSize labelSz = m_imageLabel->size();
    const QSize pmSz    = pm.size() / pm.devicePixelRatio();

    const int offsetX = (labelSz.width()  - pmSz.width())  / 2;
    const int offsetY = (labelSz.height() - pmSz.height()) / 2;

    const qreal pxInPm = labelPos.x() - offsetX;
    const qreal pyInPm = labelPos.y() - offsetY;

    const qreal sx = static_cast<qreal>(m_displayImage.width())  / pmSz.width();
    const qreal sy = static_cast<qreal>(m_displayImage.height()) / pmSz.height();

    return QPointF(pxInPm * sx, pyInPm * sy);
}

QPoint QImageView::toWidgetCoord(const QPoint& imagePos) const
{
    const QPixmap pm = m_imageLabel->pixmap();
    if (pm.isNull() || m_displayImage.isNull() || m_displayImage.width() == 0 || m_displayImage.height() == 0)
        return imagePos;

    const QSize labelSz = m_imageLabel->size();
    const QSize pmSz    = pm.size() / pm.devicePixelRatio();

    const int offsetX = (labelSz.width()  - pmSz.width())  / 2;
    const int offsetY = (labelSz.height() - pmSz.height()) / 2;

    const qreal sx = static_cast<qreal>(pmSz.width()) / m_displayImage.width();
    const qreal sy = static_cast<qreal>(pmSz.height()) / m_displayImage.height();

    return QPoint(static_cast<int>(imagePos.x() * sx) + offsetX,
                  static_cast<int>(imagePos.y() * sy) + offsetY);
}

QPointF QImageView::toWidgetCoord(const QPointF& imagePos) const
{
    const QPixmap pm = m_imageLabel->pixmap();
    if (pm.isNull() || m_displayImage.isNull() || m_displayImage.width() == 0 || m_displayImage.height() == 0)
        return imagePos;

    const QSize labelSz = m_imageLabel->size();
    const QSize pmSz    = pm.size() / pm.devicePixelRatio();

    const int offsetX = (labelSz.width()  - pmSz.width())  / 2;
    const int offsetY = (labelSz.height() - pmSz.height()) / 2;

    const qreal sx = static_cast<qreal>(pmSz.width()) / m_displayImage.width();
    const qreal sy = static_cast<qreal>(pmSz.height()) / m_displayImage.height();

    return QPointF(imagePos.x() * sx + offsetX, imagePos.y() * sy + offsetY);
}

// ═══════════════════════════════════════════════════════════
// 디렉토리 / 슬라이드쇼
// ═══════════════════════════════════════════════════════════
void QImageView::openDirectory(const QString& dirPath)
{
    QDir dir(dirPath);
    QStringList filters;
    for (const auto& ext : supportedExtensions()) filters << QStringLiteral("*.%1").arg(ext);
    dir.setNameFilters(filters); dir.setSorting(QDir::Name);
    m_dirFiles.clear();
    for (const auto& fi : dir.entryInfoList(QDir::Files))
        m_dirFiles.append(fi.absoluteFilePath());
}
void QImageView::SetNavigationSource(const QString& Path)
{
    struct Catalog { QStringList files_;std::chrono::steady_clock::time_point time_; };
    struct Catalogs { QMutex mutex_;QCache<QString,Catalog> values_{16}; };
    static const auto Shared=std::make_shared<Catalogs>();
    m_dirFiles.clear();m_dirIndex=0;directoryLoading_=false;pendingDirectorySteps_=0;directoryStop_->request_stop();directoryStop_=std::make_shared<std::stop_source>();
    const auto Generation=++navigationGeneration_;const QFileInfo Source(Path);
    if(!supportedExtensions().contains(Source.suffix(),Qt::CaseInsensitive))return;
    const auto Directory=Source.absolutePath(),Current=Source.absoluteFilePath();
    const auto Stop=directoryStop_;const auto Cache=Shared;
    auto Apply=[this,Current,Stop](QStringList Files){
        directoryLoading_=false;
        m_dirFiles=std::move(Files);int At=-1;for(int I=0;I<m_dirFiles.size();++I)if(m_dirFiles[I].compare(Current,Qt::CaseInsensitive)==0){At=I;break;}m_dirIndex=std::max(0,At);
        if(At>=0&&m_dirFiles.size()>1){
            QStringList Neighbours{m_dirFiles[(At+1)%m_dirFiles.size()]};
            if(!Current.startsWith("\\\\")&&!Current.startsWith("//")&&m_dirFiles.size()>2)Neighbours.append(m_dirFiles[(At+m_dirFiles.size()-1)%m_dirFiles.size()]);
            hc::document::ImageDecodeCache::Prefetch(Neighbours,Stop->get_token(),imagePool_.get());
        }
        const auto Steps=std::exchange(pendingDirectorySteps_,0);
        if(Steps&&At>=0&&!m_dirFiles.isEmpty()){const int Count=m_dirFiles.size();const int Next=((At+Steps)%Count+Count)%Count;emit OpenRequested(m_dirFiles[Next]);}
        emit viewStateChanged();
    };
    std::optional<QStringList> Existing;
    {QMutexLocker Guard(&Cache->mutex_);if(const auto* Item=Cache->values_.object(Directory))
        if(std::chrono::steady_clock::now()-Item->time_<std::chrono::seconds(2))Existing=Item->files_;}
    if(Existing){Apply(std::move(*Existing));return;}
    directoryLoading_=true;
    auto* Watcher=new QFutureWatcher<QStringList>(this);
    connect(Watcher,&QFutureWatcherBase::finished,this,[this,Watcher,Generation,Apply]{
        const auto Files=Watcher->result();hc::document::DeleteCompletedImageWork(Watcher);if(Generation==navigationGeneration_)Apply(Files);
    });
    const auto Extensions=supportedExtensions();
    Watcher->setFuture(QtConcurrent::run(imagePool_.get(),[Directory,Extensions,Cache,Stop]{
        hc::document::ImageTraceSpan Trace("image.catalog",Directory);QStringList Files,Patterns;
        if(Stop->stop_requested())return Files;
        for(const auto& Ext:Extensions)Patterns.append("*."+Ext);
        QDir Dir(Directory);Dir.setNameFilters(Patterns);Dir.setSorting(QDir::Name);
        for(const auto& Entry:Dir.entryInfoList(QDir::Files)){if(Stop->stop_requested())return QStringList{};Files.append(Entry.absoluteFilePath());}
        {QMutexLocker Guard(&Cache->mutex_);Cache->values_.insert(Directory,new Catalog{Files,std::chrono::steady_clock::now()});}
        return Files;
    }));
}
void QImageView::startSlideshow(int ms)
{
    if (m_dirFiles.isEmpty())
        return;

    m_slideshowTimer->start(ms);
    emit slideshowStateChanged(true);
}
void QImageView::stopSlideshow()         { m_slideshowTimer->stop(); emit slideshowStateChanged(false); }
void QImageView::nextImage()
{
    if(m_dirFiles.isEmpty()&&directoryLoading_){pendingDirectorySteps_=std::clamp(pendingDirectorySteps_+1,-1000000,1000000);return;}
    if (!m_dirFiles.isEmpty()) emit OpenRequested(m_dirFiles[(m_dirIndex+1)%m_dirFiles.size()]);
}

void QImageView::previousImage()
{
    if(m_dirFiles.isEmpty()&&directoryLoading_){pendingDirectorySteps_=std::clamp(pendingDirectorySteps_-1,-1000000,1000000);return;}
    if (!m_dirFiles.isEmpty()) emit OpenRequested(m_dirFiles[(m_dirIndex-1+m_dirFiles.size())%m_dirFiles.size()]);
}
QSize QImageView::originalImageSize() const { return m_originalImage.size(); }
int  QImageView::imageIndexInDirectory() const { return m_dirIndex; }
int  QImageView::imageCountInDirectory() const { return static_cast<int>(m_dirFiles.size()); }

qreal QImageView::displayScalePercent() const
{
    if (m_displayImage.isNull())
        return 100.0;

    if (m_fitMode == OriginalSize)
        return m_zoomLevel;

    const QImage scaled = scaledImage();
    if (scaled.isNull() || m_displayImage.width() <= 0)
        return 100.0;

    return (static_cast<qreal>(scaled.width()) / m_displayImage.width()) * 100.0;
}

QString QImageView::imageFormatName() const
{
    if (!m_imageFormat.isEmpty())
        return m_imageFormat;
    return m_filePath.isEmpty() ? tr("Unknown") : QFileInfo(m_filePath).suffix().toUpper();
}

QString QImageView::imageColorDescription() const
{
    if (m_originalImage.isNull())
        return tr("Unknown");

    return imageFormatText(m_originalImage.format(), m_originalImage.hasAlphaChannel());
}

QPointF QImageView::imageDpi() const
{
    if (m_originalImage.isNull())
        return {};

    constexpr qreal dpmToDpi = 0.0254;
    const qreal dpiX = m_originalImage.dotsPerMeterX() > 0 ? m_originalImage.dotsPerMeterX() * dpmToDpi : 0.0;
    const qreal dpiY = m_originalImage.dotsPerMeterY() > 0 ? m_originalImage.dotsPerMeterY() * dpmToDpi : 0.0;
    return QPointF(dpiX, dpiY);
}

bool QImageView::saveAs(const QString& filePath, const char* format, int quality)
{
    Q_UNUSED(filePath); Q_UNUSED(format); Q_UNUSED(quality); emit SaveRequested(); return false;
}

void QImageView::setModifiedState(bool modified)
{
    ++contentRevision_; renderedKey_.clear();renderError_.clear();renderStop_->request_stop();
    m_modified = modified;
    emit modifiedChanged(m_modified);
    emit ContentChanged();
}

// ═══════════════════════════════════════════════════════════
QImage QImageView::scaledImage() const
{
    if(m_displayImage.isNull()||!m_scrollArea)return {};
    auto Request=renderRequest();Request.targetColor_=hc::document::ImageDisplayColorSpace(screen());const auto Key=hc::document::ImageCacheKey(contentRevision_,Request,devicePixelRatioF());
    if(Key==renderedKey_&&!renderedImage_.isNull())return renderedImage_;
    Request.filter_="preview";
    if(hc::document::ImageCacheKey(contentRevision_,Request,devicePixelRatioF())==previewKey_)return previewImage_;
    return m_displayImage;
}

void QImageView::updateDisplay()
{
    if (m_updatingDisplay || m_displayImage.isNull() || !m_scrollArea || !m_imageLabel) return;
    QScopedValueRollback<bool> guard(m_updatingDisplay, true);

    hc::document::ImageTraceSpan DisplayTrace("view.publish");
    auto Request=renderRequest();Request.targetColor_=hc::document::ImageDisplayColorSpace(screen());const auto Key=hc::document::ImageCacheKey(contentRevision_,Request,devicePixelRatioF());
    Request.traceSource_=m_filePath;
    auto PreviewRequest=Request;PreviewRequest.filter_="preview";
    const auto PreviewKey=hc::document::ImageCacheKey(contentRevision_,PreviewRequest,devicePixelRatioF());
    if(previewing_&&PreviewKey!=previewRunningKey_)previewStop_->request_stop();
    if(rendering_&&Key!=renderRunningKey_)renderStop_->request_stop();
    if(PreviewKey!=previewKey_&&!previewing_){
        previewing_=true;previewRunningKey_=PreviewKey;previewStop_=std::make_shared<std::stop_source>();
        const auto Stop=previewStop_;auto* Watcher=new QFutureWatcher<QImage>(this);
        connect(Watcher,&QFutureWatcherBase::finished,this,[this,Watcher,PreviewKey]{
            const auto Image=Watcher->result();hc::document::DeleteCompletedImageWork(Watcher);previewing_=false;
            auto Current=renderRequest();Current.targetColor_=hc::document::ImageDisplayColorSpace(screen());Current.filter_="preview";
            if(!m_displayImage.isNull()&&PreviewKey==hc::document::ImageCacheKey(contentRevision_,Current,devicePixelRatioF())){
                if(Image.isNull())renderError_="image-preview-cancelled-or-unavailable";
                previewKey_=PreviewKey;previewImage_=Image;previewImage_.setDevicePixelRatio(devicePixelRatioF());
            }
            updateDisplay();emit viewStateChanged();
        });
        Watcher->setFuture(QtConcurrent::run(imagePool_.get(),[PreviewRequest,Stop]{
            hc::document::ImageTraceSpan Trace("image.preview",PreviewRequest.traceSource_);
            if(Stop->stop_requested())return QImage{};
            auto Image=hc::document::ImageColorSource(PreviewRequest.source_.scaled(PreviewRequest.outputSize_,Qt::IgnoreAspectRatio,Qt::SmoothTransformation));
            Image=Image.convertedToColorSpace(PreviewRequest.targetColor_,QImage::Format_RGBA8888);
            return Stop->stop_requested()?QImage{}:Image;
        }));
    }
    if(!rendering_&&PreviewKey==previewKey_&&Key!=renderedKey_&&renderError_.isEmpty()) {
        renderRunningKey_=Key;
        rendering_=true;renderStop_=std::make_shared<std::stop_source>();auto Work=Request;Work.targetColor_=hc::document::ImageDisplayColorSpace(screen());Work.stop_=renderStop_->get_token();
        auto* Watcher=new QFutureWatcher<hc::document::ImageRenderResult>(this);
        connect(Watcher,&QFutureWatcherBase::finished,this,[this,Watcher,Key] {
            const auto Result=Watcher->result();hc::document::DeleteCompletedImageWork(Watcher);rendering_=false;
            if(!m_displayImage.isNull()&&Key==[this]{auto Request=renderRequest();Request.targetColor_=hc::document::ImageDisplayColorSpace(screen());return hc::document::ImageCacheKey(contentRevision_,Request,devicePixelRatioF());}()) {
                renderError_=Result.error_;renderedKey_=Key;renderedImage_=Result.image_;renderedImage_.setDevicePixelRatio(devicePixelRatioF());
            }
            updateDisplay();
            emit viewStateChanged();
        });
        Watcher->setFuture(QtConcurrent::run(imagePool_.get(),[Work]{return hc::document::ResampleImage(Work);}));
    }
    if((Key!=renderedKey_||renderedImage_.isNull())&&(PreviewKey!=previewKey_||previewImage_.isNull()))return;
    displayFinal_=Key==renderedKey_&&!renderedImage_.isNull();
    QImage img = scaledImage();
    if (img.isNull()) img = m_displayImage;

    if (m_compositeMode) {
        QPainter painter(&img);
        paintCompositeOverlay(painter, img);
        painter.end();
    }

    m_imageLabel->setProperty("imageSource",m_filePath);
    const QPixmap pixmap = QPixmap::fromImage(img);
    if (pixmap.isNull()) {
        m_imageLabel->clear();
        return;
    }

    if (m_fitMode == OriginalSize) {
        // 원본/줌 — 스크롤 가능하도록 라벨 크기 = 이미지 크기
        m_scrollArea->setWidgetResizable(false);
        m_imageLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_imageLabel->setPixmap(pixmap);
        m_imageLabel->resize(img.size());
    } else {
        // 맞춤 모드 — 라벨이 viewport를 채우고 중앙 정렬
        m_scrollArea->setWidgetResizable(true);
        m_imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_imageLabel->setPixmap(pixmap);
    }

    if (m_watermarkOverlay) m_watermarkOverlay->update();
    emit viewStateChanged();
}

/// 드래그 중 사각형/타원/화살표 미리보기 오버레이
void QImageView::updateOverlay(const QPoint& curLabelPos)
{
    if (m_displayImage.isNull()) return;

    // 현재 표시용 이미지 위에 오버레이 그리기
    QImage overlay = scaledImage();
    QPainter painter(&overlay);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(makeDrawingPen(m_penColor, m_penWidth, m_penStyle));
    painter.setBrush(Qt::NoBrush);

    // labelPos → 표시 이미지 좌표 (toImageCoord는 원본 좌표이므로 별도 계산)
    const QPixmap pm = m_imageLabel->pixmap();
    const QSize labelSz = m_imageLabel->size();
    const QSize pmSz = pm.size() / pm.devicePixelRatio();
    const int ox = (labelSz.width()  - pmSz.width())  / 2;
    const int oy = (labelSz.height() - pmSz.height()) / 2;

    // 표시 이미지 내 좌표
    QPoint start(m_dragStart.x() - ox, m_dragStart.y() - oy);
    QPoint end(curLabelPos.x() - ox, curLabelPos.y() - oy);

    QRect rect = QRect(start, end).normalized();

    if (m_editTool == Rectangle) {
        painter.drawRect(rect);
    } else if (m_editTool == Ellipse) {
        painter.drawEllipse(rect);
    } else if (m_editTool == Arrow) {
        painter.drawLine(start, end);
    }
    painter.end();

    m_imageLabel->setPixmap(QPixmap::fromImage(overlay));
}

void QImageView::renderPage(QPainter* painter, int, const QRectF& rect)
{
    if (!m_displayImage.isNull()) painter->drawImage(rect, m_displayImage);
}

int QImageView::pageCount() const { return m_displayImage.isNull() ? 0 : 1; }

// ═══════════════════════════════════════════════════════════
// 도구모음
// ═══════════════════════════════════════════════════════════
QToolBar* QImageView::createToolBar()
{
    auto* tb = QBaseView::createToolBar();

    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.previous"),
            &QAction::triggered, this, &QImageView::previousImage);
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.next"),
            &QAction::triggered, this, &QImageView::nextImage);
    tb->addSeparator();

    auto* fitCombo = new QComboBox(tb);
    fitCombo->addItem(tr("원본 크기"), OriginalSize);
    fitCombo->addItem(tr("창에 맞춤"),  FitToWindow);
    fitCombo->addItem(tr("너비 맞춤"),  FitWidth);
    fitCombo->addItem(tr("높이 맞춤"),  FitHeight);
    fitCombo->addItem(tr("늘림"),       StretchToWindow);
    // 현재 fitMode에 맞는 항목 선택
    int fitIdx = fitCombo->findData(static_cast<int>(m_fitMode));
    if (fitIdx >= 0) fitCombo->setCurrentIndex(fitIdx);
    connect(fitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, fitCombo](int idx) { setFitMode(static_cast<FitMode>(fitCombo->itemData(idx).toInt())); });
    tb->addWidget(fitCombo);
    tb->addSeparator();

    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.rotateCw"),
            &QAction::triggered, this, &QImageView::rotateClockwise);
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.rotateCcw"),
            &QAction::triggered, this, &QImageView::rotateCounterClockwise);
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.flipHorizontal"),
            &QAction::triggered, this, &QImageView::flipHorizontal);
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.flipVertical"),
            &QAction::triggered, this, &QImageView::flipVertical);
    tb->addSeparator();

    // 그리기 도구
    auto* toolGroup = new QActionGroup(tb);
    toolGroup->setExclusive(true);

    auto* freeDrawAction = MV_CREATE_TOOLBAR_ACTION(tb, "image.tool.freeDraw");
    auto* rectangleAction = MV_CREATE_TOOLBAR_ACTION(tb, "image.tool.rectangle");
    auto* ellipseAction = MV_CREATE_TOOLBAR_ACTION(tb, "image.tool.ellipse");
    auto* arrowAction = MV_CREATE_TOOLBAR_ACTION(tb, "image.tool.arrow");
    auto* textAction = MV_CREATE_TOOLBAR_ACTION(tb, "image.tool.text");
    auto* cropAction = MV_CREATE_TOOLBAR_ACTION(tb, "image.tool.crop");

    toolGroup->addAction(freeDrawAction);
    toolGroup->addAction(rectangleAction);
    toolGroup->addAction(ellipseAction);
    toolGroup->addAction(arrowAction);
    toolGroup->addAction(textAction);
    toolGroup->addAction(cropAction);

    const auto syncToolChecks = [this, freeDrawAction, rectangleAction, ellipseAction, arrowAction, textAction, cropAction] {
        freeDrawAction->setChecked(m_editTool == FreeDraw);
        rectangleAction->setChecked(m_editTool == Rectangle);
        ellipseAction->setChecked(m_editTool == Ellipse);
        arrowAction->setChecked(m_editTool == Arrow);
        textAction->setChecked(m_editTool == TextTool);
        cropAction->setChecked(m_editTool == Crop);
    };
    syncToolChecks();
    connect(this, &QImageView::viewStateChanged, tb, syncToolChecks);

    connect(freeDrawAction, &QAction::triggered, this, [this]{ setEditTool(FreeDraw); });
    connect(rectangleAction, &QAction::triggered, this, [this]{ setEditTool(Rectangle); });
    connect(ellipseAction, &QAction::triggered, this, [this]{ setEditTool(Ellipse); });
    connect(arrowAction, &QAction::triggered, this, [this]{ setEditTool(Arrow); });
    connect(textAction, &QAction::triggered, this, [this]{ setEditTool(TextTool); });
    connect(cropAction, &QAction::triggered, this, [this]{ setEditTool(Crop); });

    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.cropApply"),
            &QAction::triggered, this, &QImageView::cropToSelection);
    tb->addSeparator();

    auto* penWidthSpin = new QSpinBox(tb);
    penWidthSpin->setRange(1, 50);
    penWidthSpin->setPrefix(tr("굵기"));
    penWidthSpin->setValue(m_penWidth);
    penWidthSpin->setSuffix(tr(" px"));
    penWidthSpin->setToolTip(tr("선 굵기"));
    connect(penWidthSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &QImageView::setPenWidth);
    tb->addWidget(penWidthSpin);

    auto* penStyleCombo = new QComboBox(tb);
    penStyleCombo->addItem(tr("실선"), static_cast<int>(Qt::SolidLine));
    penStyleCombo->addItem(tr("점선"), static_cast<int>(Qt::DashLine));
    penStyleCombo->addItem(tr("도트"), static_cast<int>(Qt::DotLine));
    penStyleCombo->addItem(tr("일점쇄선"), static_cast<int>(Qt::DashDotLine));
    const int penStyleIndex = penStyleCombo->findData(static_cast<int>(m_penStyle));
    if (penStyleIndex >= 0)
        penStyleCombo->setCurrentIndex(penStyleIndex);
    penStyleCombo->setToolTip(tr("선 스타일"));
    connect(penStyleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, penStyleCombo](int index) {
                setPenStyle(static_cast<Qt::PenStyle>(penStyleCombo->itemData(index).toInt()));
            });
    tb->addWidget(penStyleCombo);

    auto* textSizeSpin = new QSpinBox(tb);
    textSizeSpin->setRange(8, 256);
    textSizeSpin->setPrefix(tr("글자"));
    textSizeSpin->setValue(m_textPixelSize);
    textSizeSpin->setSuffix(tr(" px"));
    textSizeSpin->setToolTip(tr("텍스트 도구 글자 크기"));
    connect(textSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &QImageView::setTextPixelSize);
    tb->addWidget(textSizeSpin);

    auto* colorButton = new QToolButton(tb);
    colorButton->setIcon(ToolbarIcons::iconByName(QStringLiteral("color-picker")));
    colorButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const auto updateColorButton = [this, colorButton] {
        colorButton->setStyleSheet(QStringLiteral(
            "QToolButton { background-color: %1; color: %2; border: 1px solid palette(mid); padding: 3px 8px; border-radius: 3px; }"
        ).arg(m_penColor.name(QColor::HexArgb), toolButtonForegroundColor(m_penColor)));
        colorButton->setToolTip(tr("선/도형/텍스트 색상"));
        colorButton->setStatusTip(tr("선/도형/텍스트 색상"));
    };
    updateColorButton();
    connect(colorButton, &QToolButton::clicked, this, [this, updateColorButton] {
        const QColor color = QColorDialog::getColor(m_penColor, this, tr("그리기 색상 선택"));
        if (!color.isValid())
            return;
        setPenColor(color);
        updateColorButton();
    });
    tb->addWidget(colorButton);

    tb->addSeparator();
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.undo"),
            &QAction::triggered, this, &QImageView::undoEdit);
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.redo"),
            &QAction::triggered, this, &QImageView::redoEdit);
    tb->addSeparator();

    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.slideshowStart"),
            &QAction::triggered, this, [this]{ startSlideshow(); });
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.slideshowStop"),
            &QAction::triggered, this, &QImageView::stopSlideshow);
    tb->addSeparator();

    connect(MV_CREATE_TOOLBAR_ACTION(tb, "image.saveAs"),
            &QAction::triggered, this, [this]{
        saveFileAs();
    });

    return tb;
}

// ?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧??
// ?⑹꽦 (Composite)
// ?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧?먥븧??
bool QImageView::startComposite(const QImage& image)
{
    if (image.isNull() || m_displayImage.isNull())
        return false;

    m_compositeImage = image;
    m_compositeMode = true;
    m_compositeRotation = 0.0;

    // 珥덇린 ?ш린: ?붾㈃(?cheon) 以묒븰??蹂몃옒 ?ш린濡? ???붾㈃蹂대떎 ?щ㈃ 以꾩엫
    double ratio = 1.0;
    if (m_compositeImage.width() > m_displayImage.width() || m_compositeImage.height() > m_displayImage.height()) {
        double rw = static_cast<double>(m_displayImage.width()) / m_compositeImage.width();
        double rh = static_cast<double>(m_displayImage.height()) / m_compositeImage.height();
        ratio = std::min(rw, rh) * 0.8;
    }

    double nw = m_compositeImage.width() * ratio;
    double nh = m_compositeImage.height() * ratio;
    double nx = (m_displayImage.width() - nw) / 2.0;
    double ny = (m_displayImage.height() - nh) / 2.0;

    m_compositeRect = QRectF(nx, ny, nw, nh);
    m_activeCompositeHandle = CompHandleNone;

    updateDisplay();
    return true;
}

void QImageView::finishComposite(bool accept)
{
    if(accept&&!ApplyComposite(m_compositeImage,m_compositeRect,m_compositeRotation))return;
    m_compositeMode=false;m_compositeImage={};updateDisplay();
}

QImageView::CompositeHandle QImageView::hitTestCompositeHandles(const QPoint& widgetPos)
{
    if (!m_compositeMode) return CompHandleNone;

    // Convert handles to widget coordinates
    QPointF center = toWidgetCoord(m_compositeRect.center().toPoint());

    // We'll calculate the 4 corners after rotation
    double rad = m_compositeRotation * M_PI / 180.0;
    double cosR = std::cos(rad);
    double sinR = std::sin(rad);

    auto rotatePt = [&](const QPointF& pt) -> QPointF {
        QPointF c = m_compositeRect.center();
        double dx = pt.x() - c.x();
        double dy = pt.y() - c.y();
        double rx = dx * cosR - dy * sinR;
        double ry = dx * sinR + dy * cosR;
        return QPointF(c.x() + rx, c.y() + ry);
    };

    QPointF tl = toWidgetCoord(rotatePt(m_compositeRect.topLeft()).toPoint());
    QPointF tr = toWidgetCoord(rotatePt(m_compositeRect.topRight()).toPoint());
    QPointF bl = toWidgetCoord(rotatePt(m_compositeRect.bottomLeft()).toPoint());
    QPointF br = toWidgetCoord(rotatePt(m_compositeRect.bottomRight()).toPoint());

    // Top center for rotate handle
    QPointF topCenterImg = rotatePt(QPointF(m_compositeRect.center().x(), m_compositeRect.top() - 30));
    QPointF rotateHandle = toWidgetCoord(topCenterImg.toPoint());

    // Bottom handles for Accept / Cancel
    QPointF bottomCenterImg = rotatePt(QPointF(m_compositeRect.center().x(), m_compositeRect.bottom() + 30));
    QPointF bcWidget = toWidgetCoord(bottomCenterImg.toPoint());

    const QPointF buttonOffset(25.0 * cosR, 25.0 * sinR);
    QPointF acceptHandle = bcWidget - buttonOffset;
    QPointF cancelHandle = bcWidget + buttonOffset;

    const double handleRadius = 12.0; // click radius
    auto distSq = [](const QPointF& a, const QPointF& b) {
        return (a.x()-b.x())*(a.x()-b.x()) + (a.y()-b.y())*(a.y()-b.y());
    };

    QPointF wPos(widgetPos);

    if (distSq(wPos, acceptHandle) <= handleRadius*handleRadius * 2) return CompHandleAccept;
    if (distSq(wPos, cancelHandle) <= handleRadius*handleRadius * 2) return CompHandleCancel;
    if (distSq(wPos, rotateHandle) <= handleRadius*handleRadius) return CompHandleRotate;
    if (distSq(wPos, tl) <= handleRadius*handleRadius) return CompHandleResizeTL;
    if (distSq(wPos, tr) <= handleRadius*handleRadius) return CompHandleResizeTR;
    if (distSq(wPos, bl) <= handleRadius*handleRadius) return CompHandleResizeBL;
    if (distSq(wPos, br) <= handleRadius*handleRadius) return CompHandleResizeBR;

    // Check if inside image
    // Map wPos back to image, inverse rotate, check rect
    QPoint imgPos = toImageCoord(widgetPos);
    double dx = imgPos.x() - m_compositeRect.center().x();
    double dy = imgPos.y() - m_compositeRect.center().y();
    double invX = dx * cosR + dy * sinR;
    double invY = -dx * sinR + dy * cosR;
    QPointF invPt(m_compositeRect.center().x() + invX, m_compositeRect.center().y() + invY);

    if (m_compositeRect.contains(invPt))
        return CompHandleMove;

    return CompHandleNone;
}

bool QImageView::handleCompositeEvent(QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            finishComposite(false);
            return true;
        } else if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            finishComposite(true);
            return true;
        }
        return false;
    }

    auto* me = dynamic_cast<QMouseEvent*>(event);
    if (!me) return false;

    QPoint labelPos = m_imageLabel->mapFromGlobal(me->globalPosition().toPoint());

    if (event->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
        m_activeCompositeHandle = hitTestCompositeHandles(labelPos);
        if (m_activeCompositeHandle == CompHandleAccept) {
            finishComposite(true);
            return true;
        } else if (m_activeCompositeHandle == CompHandleCancel) {
            finishComposite(false);
            return true;
        } else if (m_activeCompositeHandle != CompHandleNone) {
            m_compositeDragStartPos = labelPos;
            m_compositeDragStartRect = m_compositeRect;
            m_compositeDragStartRotation = m_compositeRotation;
            return true;
        }
    } else if (event->type() == QEvent::MouseMove) {
        if (m_activeCompositeHandle != CompHandleNone) {
            QPoint imgPos = toImageCoord(labelPos);
            QPoint startImgPos = toImageCoord(m_compositeDragStartPos);
            QPointF delta(imgPos.x() - startImgPos.x(), imgPos.y() - startImgPos.y());

            if (m_activeCompositeHandle == CompHandleMove) {
                m_compositeRect = m_compositeDragStartRect.translated(delta);
            } else if (m_activeCompositeHandle == CompHandleRotate) {
                QPointF center = m_compositeRect.center();
                double angle1 = std::atan2(startImgPos.y() - center.y(), startImgPos.x() - center.x());
                double angle2 = std::atan2(imgPos.y() - center.y(), imgPos.x() - center.x());
                m_compositeRotation = m_compositeDragStartRotation + (angle2 - angle1) * 180.0 / M_PI;
            } else {
                // Resize
                double rad = -m_compositeRotation * M_PI / 180.0;
                double cosR = std::cos(rad);
                double sinR = std::sin(rad);
                double rotDx = delta.x() * cosR - delta.y() * sinR;
                double rotDy = delta.x() * sinR + delta.y() * cosR;

                QRectF r = m_compositeDragStartRect;
                if (m_activeCompositeHandle == CompHandleResizeTL) {
                    r.setTopLeft(r.topLeft() + QPointF(rotDx, rotDy));
                } else if (m_activeCompositeHandle == CompHandleResizeTR) {
                    r.setTopRight(r.topRight() + QPointF(rotDx, rotDy));
                } else if (m_activeCompositeHandle == CompHandleResizeBL) {
                    r.setBottomLeft(r.bottomLeft() + QPointF(rotDx, rotDy));
                } else if (m_activeCompositeHandle == CompHandleResizeBR) {
                    r.setBottomRight(r.bottomRight() + QPointF(rotDx, rotDy));
                }

                if (r.width() < 10) r.setWidth(10);
                if (r.height() < 10) r.setHeight(10);

                // Adjust center offset due to resize
                QPointF newCenter = r.center();
                double nCx = newCenter.x(); double nCy = newCenter.y();
                double oCx = m_compositeDragStartRect.center().x(); double oCy = m_compositeDragStartRect.center().y();
                double shiftX = nCx - oCx; double shiftY = nCy - oCy;

                // Re-rotate the shift to keep opposite corner stationary
                rad = m_compositeRotation * M_PI / 180.0;
                cosR = std::cos(rad); sinR = std::sin(rad);
                double realShiftX = shiftX * cosR - shiftY * sinR;
                double realShiftY = shiftX * sinR + shiftY * cosR;

                QPointF finalCenter(oCx + realShiftX, oCy + realShiftY);
                r.moveCenter(finalCenter);
                m_compositeRect = r;
            }
            updateDisplay();
            return true;
        } else {
            CompositeHandle hit = hitTestCompositeHandles(labelPos);
            Qt::CursorShape shape = Qt::ArrowCursor;
            if (hit == CompHandleMove) shape = Qt::SizeAllCursor;
            else if (hit == CompHandleResizeTL || hit == CompHandleResizeBR) shape = Qt::SizeFDiagCursor;
            else if (hit == CompHandleResizeTR || hit == CompHandleResizeBL) shape = Qt::SizeBDiagCursor;
            else if (hit == CompHandleRotate) shape = Qt::ClosedHandCursor;
            else if (hit == CompHandleAccept || hit == CompHandleCancel) shape = Qt::PointingHandCursor;

            if (m_imageLabel->cursor().shape() != shape) {
                m_imageLabel->setCursor(shape);
            }

            // We usually don't consume the event here so mouse tracking behaves normally
            // but we can return true.
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton) {
        if (m_activeCompositeHandle != CompHandleNone) {
            m_activeCompositeHandle = CompHandleNone;
            return true;
        }
    }

    return false;
}

void QImageView::paintCompositeOverlay(QPainter& painter, const QImage& baseImage)
{
    if (!m_compositeMode || m_compositeImage.isNull()) return;

    painter.save();

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // Map original image coordinates to baseImage coordinates
    const qreal sx = m_displayImage.width() ? static_cast<qreal>(baseImage.width()) / m_displayImage.width() : 1.0;
    const qreal sy = m_displayImage.height() ? static_cast<qreal>(baseImage.height()) / m_displayImage.height() : 1.0;
    auto toBaseCoord = [&](const QPointF& p) {
        return QPointF(p.x() * sx, p.y() * sy);
    };

    QPointF center = toBaseCoord(m_compositeRect.center());
    QPointF tlMap = toBaseCoord(m_compositeRect.topLeft());
    QPointF brMap = toBaseCoord(m_compositeRect.bottomRight());
    QRectF widgetRect(tlMap, QSizeF(brMap.x() - tlMap.x(), brMap.y() - tlMap.y()));

    painter.translate(center);
    painter.rotate(m_compositeRotation);
    painter.translate(-center);

    painter.drawImage(widgetRect, m_compositeImage);

    // Draw bounding box
    QPen pen(ThemeManager::instance().color(QStringLiteral("image.compositeOutline")), 1, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(widgetRect);

    painter.restore();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);

    // Draw handles
    auto drawHandle = [&](const QPointF& pt, bool isCircle = true) {
        painter.setPen(QPen(ThemeManager::instance().color(QStringLiteral("image.handleBorder")), 1));
        painter.setBrush(ThemeManager::instance().color(QStringLiteral("image.handleFill")));
        if (isCircle)
            painter.drawEllipse(pt, 5, 5);
        else
            painter.drawRect(QRectF(pt.x()-5, pt.y()-5, 10, 10));
    };

    double rad = m_compositeRotation * M_PI / 180.0;
    double cosR = std::cos(rad);
    double sinR = std::sin(rad);
    auto rotatePt = [&](const QPointF& pt) -> QPointF {
        QPointF c = m_compositeRect.center();
        double dx = pt.x() - c.x();
        double dy = pt.y() - c.y();
        double rx = dx * cosR - dy * sinR;
        double ry = dx * sinR + dy * cosR;
        return QPointF(c.x() + rx, c.y() + ry);
    };

    QPointF tl = toBaseCoord(rotatePt(m_compositeRect.topLeft()));
    QPointF tr = toBaseCoord(rotatePt(m_compositeRect.topRight()));
    QPointF bl = toBaseCoord(rotatePt(m_compositeRect.bottomLeft()));
    QPointF br = toBaseCoord(rotatePt(m_compositeRect.bottomRight()));

    drawHandle(tl, false);
    drawHandle(tr, false);
    drawHandle(bl, false);
    drawHandle(br, false);

    // Rotate handle
    QPointF topCenterImg = rotatePt(QPointF(m_compositeRect.center().x(), m_compositeRect.top() - 30));
    QPointF rotateHandle = toBaseCoord(topCenterImg);

    painter.setPen(QPen(ThemeManager::instance().color(QStringLiteral("image.compositeOutline")), 1, Qt::DashLine));
    QPointF topWidget = toBaseCoord(rotatePt(QPointF(m_compositeRect.center().x(), m_compositeRect.top())));
    painter.drawLine(topWidget, rotateHandle);

    painter.setPen(QPen(ThemeManager::instance().color(QStringLiteral("image.handleBorder")), 1));
    painter.setBrush(ThemeManager::instance().color(QStringLiteral("image.accept")));
    painter.drawEllipse(rotateHandle, 5, 5);

    // Accept / Cancel handles at bottom
    QPointF bottomCenterImg = rotatePt(QPointF(m_compositeRect.center().x(), m_compositeRect.bottom() + 30));
    QPointF bcWidget = toBaseCoord(bottomCenterImg);

    const QPointF buttonOffset(25.0 * cosR, 25.0 * sinR);
    QPointF acceptHandle = bcWidget - buttonOffset;
    QPointF cancelHandle = bcWidget + buttonOffset;

    // Accept Hook
    painter.setPen(Qt::NoPen);
    painter.setBrush(ThemeManager::instance().color(QStringLiteral("image.accept")));
    painter.drawEllipse(acceptHandle, 12, 12);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawLine(acceptHandle + QPointF(-4, 0), acceptHandle + QPointF(-1, 4));
    painter.drawLine(acceptHandle + QPointF(-1, 4), acceptHandle + QPointF(5, -4));

    // Cancel X
    painter.setPen(Qt::NoPen);
    painter.setBrush(ThemeManager::instance().color(QStringLiteral("image.cancel")));
    painter.drawEllipse(cancelHandle, 12, 12);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawLine(cancelHandle + QPointF(-4, -4), cancelHandle + QPointF(4, 4));
    painter.drawLine(cancelHandle + QPointF(-4, 4), cancelHandle + QPointF(4, -4));

    painter.restore();
}

// ═══════════════════════════════════════════════════════════
// 이벤트 — m_imageLabel / viewport 에서 처리
// ═══════════════════════════════════════════════════════════
bool QImageView::eventFilter(QObject* obj, QEvent* event)
{
    if (!m_scrollArea || !m_imageLabel) return false;

    // A hosted viewer can finish its native layout after our resizeEvent.
    // Fit against the actual viewport once that layout has settled; otherwise
    // a hidden/prepared F3 window can retain its initial thumbnail-sized image.
    if (obj == m_scrollArea->viewport()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show)
        && m_fitMode != OriginalSize && !fitUpdatePending_) {
        fitUpdatePending_ = true;
        QTimer::singleShot(0, this, [this] {
            fitUpdatePending_ = false;
            if (m_fitMode != OriginalSize) updateDisplay();
        });
    }

    if (m_compositeMode) {
        if (event->type() == QEvent::KeyPress ||
            event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseMove ||
            event->type() == QEvent::MouseButtonRelease) {

            if (event->type() == QEvent::KeyPress) {
                return handleCompositeEvent(event);
            }

            // Only handle mouse events if they are on the image label or viewport
            if (obj == m_imageLabel) {
                if (handleCompositeEvent(event))
                    return true;
            }
        }
        if (event->type() != QEvent::Wheel && event->type() != QEvent::Resize && event->type() != QEvent::Paint) {
            // Block other edit tools while in composite mode
            // (Wait, we allow standard scrolling)
            if (obj == m_imageLabel && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease))
                return true;
        }
    }

    if (obj != m_imageLabel && obj != m_scrollArea->viewport())
        return QBaseView::eventFilter(obj, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = dynamic_cast<QMouseEvent*>(event);
        if (!me) return false;
        if (me->button() != Qt::LeftButton) return false;
        if (m_editTool == None) return false;

        // 모든 좌표를 m_imageLabel 로컬 좌표로 통일
        QPoint labelPos = m_imageLabel->mapFromGlobal(me->globalPosition().toPoint());
        m_dragStart     = labelPos;       // imageLabel 기준
        m_lastDrawPoint = labelPos;

        if (m_editTool == Crop) {
            if (!m_rubberBand)
                m_rubberBand = new QRubberBand(QRubberBand::Rectangle, m_scrollArea->viewport());
            QPoint vpPos = m_scrollArea->viewport()->mapFromGlobal(me->globalPosition().toPoint());
            m_rubberBand->setGeometry(QRect(vpPos, QSize()));
            m_rubberBand->show();
            m_drawing = true;
            return true;
        }
        if (m_editTool == TextTool) {
            bool ok;
            QString text = QInputDialog::getText(this, tr("텍스트 입력"), tr("텍스트:"),
                                                 QLineEdit::Normal, {}, &ok);
            if (ok && !text.isEmpty()) {
                QImage before = m_displayImage;
                QPainter painter(&m_displayImage);
                painter.setRenderHint(QPainter::TextAntialiasing);
                QPoint imgPt = toImageCoord(labelPos);
                painter.setPen(m_penColor);
                QFont f = painter.font();
                f.setPixelSize(m_textPixelSize);
                painter.setFont(f);
                painter.drawText(imgPt, text);
                painter.end();
                m_undoStack->push(new ImageEditCommand(&m_displayImage, before, m_displayImage, "Text"));
                setModifiedState(true);
                updateDisplay();
            }
            return true;
        }
        // FreeDraw, Rectangle, Ellipse, Arrow
        m_drawing = true;
        return true;
    }

    if (event->type() == QEvent::MouseMove) {
        if (!m_drawing) return false;
        auto* me = dynamic_cast<QMouseEvent*>(event);
        if (!me) return false;
        QPoint labelPos = m_imageLabel->mapFromGlobal(me->globalPosition().toPoint());

        if (m_editTool == Crop && m_rubberBand) {
            // viewport 좌표로 rubberband
            QPoint vpStart = m_scrollArea->viewport()->mapFromGlobal(
                m_imageLabel->mapToGlobal(m_dragStart));
            QPoint vpCur = m_scrollArea->viewport()->mapFromGlobal(me->globalPosition().toPoint());
            m_rubberBand->setGeometry(QRect(vpStart, vpCur).normalized());
            return true;
        }
        if (m_editTool == FreeDraw) {
            QPainter painter(&m_displayImage);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(makeDrawingPen(m_penColor, m_penWidth, m_penStyle));
            painter.drawLine(toImageCoord(m_lastDrawPoint), toImageCoord(labelPos));
            painter.end();
            m_lastDrawPoint = labelPos;
            setModifiedState(true);
            updateDisplay();
            return true;
        }
        if (m_editTool == Rectangle || m_editTool == Ellipse || m_editTool == Arrow) {
            // 드래그 중 실시간 미리보기 (점선 오버레이)
            updateOverlay(labelPos);
            return true;
        }
        return false;
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        if (!m_drawing) return false;
        auto* me = dynamic_cast<QMouseEvent*>(event);
        if (!me) return false;
        m_drawing = false;

        if (m_editTool == Crop) return true;   // rubberband 유지

        QPoint labelPos = m_imageLabel->mapFromGlobal(me->globalPosition().toPoint());
        QPoint imgStart = toImageCoord(m_dragStart);   // m_dragStart = imageLabel 좌표
        QPoint imgEnd   = toImageCoord(labelPos);

        if (m_editTool == Rectangle || m_editTool == Ellipse || m_editTool == Arrow) {
            QImage before = m_displayImage;
            QPainter painter(&m_displayImage);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(makeDrawingPen(m_penColor, m_penWidth, m_penStyle));
            painter.setBrush(Qt::NoBrush);

            if (m_editTool == Rectangle) {
                painter.drawRect(QRect(imgStart, imgEnd).normalized());
            } else if (m_editTool == Ellipse) {
                painter.drawEllipse(QRect(imgStart, imgEnd).normalized());
            } else if (m_editTool == Arrow) {
                painter.drawLine(imgStart, imgEnd);
                QLineF line(imgStart, imgEnd);
                double angle = std::atan2(-line.dy(), line.dx());
                int arrowSize = m_penWidth * 5;
                QPointF p1 = QPointF(imgEnd) - QPointF(
                    std::cos(angle + M_PI/6) * arrowSize,
                    -std::sin(angle + M_PI/6) * arrowSize);
                QPointF p2 = QPointF(imgEnd) - QPointF(
                    std::cos(angle - M_PI/6) * arrowSize,
                    -std::sin(angle - M_PI/6) * arrowSize);
                painter.drawLine(QPointF(imgEnd), p1);
                painter.drawLine(QPointF(imgEnd), p2);
            }
            painter.end();
            m_undoStack->push(new ImageEditCommand(&m_displayImage, before, m_displayImage, "Shape"));
            setModifiedState(true);
            updateDisplay();
            return true;
        }
        return false;
    }

    return QBaseView::eventFilter(obj, event);
}

void QImageView::resizeEvent(QResizeEvent* event)
{
    QBaseView::resizeEvent(event);
    if (m_fitMode != OriginalSize) updateDisplay();
}

void QImageView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        setZoomLevel(m_zoomLevel + (event->angleDelta().y() > 0 ? 10.0 : -10.0));
        event->accept();
    } else {
        QBaseView::wheelEvent(event);
    }
}



void QImageView::LoadContent(const QImage& Image,const QString& Path,bool Traverse)
{
    if(Image.sizeInBytes()>8*1024*1024)m_undoStack->setUndoLimit(2);
    openImage(Image,QFileInfo(Path).fileName());m_fitMode=FitToWindow;m_zoomLevel=100;m_filePath=Path;m_imageFormat=QFileInfo(Path).suffix().toUpper();
    if(Traverse) SetNavigationSource(Path);
    else {m_dirFiles.clear();m_dirIndex=0;}
    m_undoStack->setClean();updateDisplay();
}
QByteArray QImageView::SerializeContent(const QByteArray& Format,int Quality) const
{
    QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);
    QImageWriter Writer(&Buffer,Format);Writer.setQuality(Quality);
    return Writer.write(m_displayImage)?Bytes:QByteArray{};
}
void QImageView::CommitContent() {m_undoStack->setClean();m_modified=false;}
void QImageView::DisplayAnimationFrame(const QImage& Image){if(Image.isNull()||m_modified||rendering_||pendingEdit_)return;m_displayImage=Image;++contentRevision_;renderStop_->request_stop();renderedKey_.clear();renderError_.clear();updateDisplay();emit imageChanged();}
void QImageView::SetDisplayFilter(const QString& Filter)
{
    if(Filter!="auto") {bool Found=false;for(const auto& Item:hc::document::ImageFilterRegistry::Filters())Found|=Item.id_==Filter;if(!Found)return;}
    filter_=Filter;renderError_.clear();renderedKey_.clear();renderStop_->request_stop();updateDisplay();
}
bool QImageView::ApplyCrop(const QRect& Rect)
{
    const auto Crop=Rect.intersected(m_displayImage.rect());if(Crop.isEmpty())return false;
    const auto Before=m_displayImage;m_displayImage=m_displayImage.copy(Crop);
    m_undoStack->push(new ImageEditCommand(&m_displayImage,Before,m_displayImage,"Crop"));setModifiedState(true);updateDisplay();return true;
}
bool QImageView::ApplyResize(const QSize& Size,const QString& Filter)
{
    if(Size.isEmpty()||m_displayImage.isNull()||pendingEdit_)return false;
    hc::document::ImageRenderRequest Request;Request.source_=m_displayImage;Request.outputSize_=Size;Request.filter_=Filter;
    Request.sourceToOutput_.scale(double(Size.width())/m_displayImage.width(),double(Size.height())/m_displayImage.height());
    return startImageEdit(Request,"Resize");
}
bool QImageView::ApplyComposite(const QImage& Image,const QRectF& Rect,double Angle)
{
    if(Image.isNull()||Rect.isEmpty()||m_displayImage.isNull()||pendingEdit_)return false;
    hc::document::ImageRenderRequest Request;Request.source_=Image;Request.outputSize_=m_displayImage.size();Request.filter_=filter_;Request.transparentBoundary_=true;
    Request.sourceToOutput_.translate(Rect.center().x(),Rect.center().y());Request.sourceToOutput_.rotate(Angle);
    Request.sourceToOutput_.translate(-Rect.width()/2,-Rect.height()/2);Request.sourceToOutput_.scale(Rect.width()/Image.width(),Rect.height()/Image.height());
    return startImageEdit(Request,"Composite");
}
hc::document::ImageRenderRequest QImageView::renderRequest() const
{
    hc::document::ImageRenderRequest Request;Request.source_=m_displayImage;Request.filter_=filter_;
    const auto Viewport=m_scrollArea->viewport()->size().expandedTo(QSize(1,1));auto Logical=m_displayImage.size();
    switch(m_fitMode) {
    case OriginalSize:Logical=m_displayImage.size()*(m_zoomLevel/100);break;
    case FitToWindow:Logical=m_displayImage.size().scaled(Viewport,Qt::KeepAspectRatio);break;
    case FitWidth:Logical=m_displayImage.size()*(double(Viewport.width())/m_displayImage.width());break;
    case FitHeight:Logical=m_displayImage.size()*(double(Viewport.height())/m_displayImage.height());break;
    case StretchToWindow:Logical=Viewport;break;
    }
    Request.outputSize_=(m_fitMode==OriginalSize?m_displayImage.size()*(m_zoomLevel/100):(Logical*devicePixelRatioF())).expandedTo(QSize(1,1));
    Request.sourceToOutput_.scale(double(Request.outputSize_.width())/m_displayImage.width(),double(Request.outputSize_.height())/m_displayImage.height());
    return Request;
}

bool QImageView::startImageEdit(hc::document::ImageRenderRequest Request,const QString& Label)
{
    if(pendingEdit_||!hc::document::ImageFilterRegistry::Resolve(Request.filter_,Request.sourceToOutput_).kernel_)return false;
    pendingEdit_=true;renderError_.clear();setEnabled(false);editStop_=std::make_shared<std::stop_source>();Request.stop_=editStop_->get_token();
    const auto Revision=contentRevision_;const auto Before=m_displayImage;auto* Watcher=new QFutureWatcher<hc::document::ImageRenderResult>(this);
    connect(Watcher,&QFutureWatcherBase::finished,this,[this,Watcher,Revision,Before,Label]{
        const auto Result=Watcher->result();hc::document::DeleteCompletedImageWork(Watcher);pendingEdit_=false;setEnabled(true);
        if(Revision!=contentRevision_){emit viewStateChanged();return;}
        if(!Result.error_.isEmpty()){renderError_=Result.error_;emit viewStateChanged();return;}
        m_displayImage=Label=="Composite"?Before:Result.image_;
        if(Label=="Composite"){QPainter Painter(&m_displayImage);Painter.drawImage(0,0,Result.image_);}
        m_undoStack->push(new ImageEditCommand(&m_displayImage,Before,m_displayImage,Label));setModifiedState(true);updateDisplay();
    });Watcher->setFuture(QtConcurrent::run(imagePool_.get(),[Request]{return hc::document::ResampleImage(Request);}));return true;
}
