#include "pdf/QPDFView.h"

#include "core/ThemeManager.h"
#include "core/ToolbarIcons.h"
#include "utils/FileLoadHelper.h"

#include <fpdfview.h>
#include <fpdf_annot.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_text.h>

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QDebug>
#include <QPainter>
#include <QActionGroup>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QPointer>
#include <QSpinBox>
#include <QComboBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QFileInfo>
#include <QFile>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidgetItem>

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>

void InitializeDocumentPdfium();
namespace {

struct PdfOpenResult {
    bool success = false;
    QString filePath;
    QString errorMessage;
    QByteArray fileData;
};

void ensurePdfiumInitialized()
{
    InitializeDocumentPdfium();
}

class ReversedPageSpinBox : public QSpinBox
{
public:
    explicit ReversedPageSpinBox(QWidget* parent = nullptr) : QSpinBox(parent) {}

    void stepBy(int steps) override
    {
        QSpinBox::stepBy(-steps);
    }
};
}

// ═══════════════════════════════════════════════════════════
QPDFView::QPDFView(QWidget* parent)
    : QBaseView(parent)
    , m_formManager(new PDFFormManager(this))
    , m_annotationManager(nullptr)
{
    ensurePdfiumInitialized();
    m_annotationColor = ThemeManager::instance().color(QStringLiteral("pdf.annotation"));
    m_annotationFont = QFont(QStringLiteral("Arial"));
    setupUI();
    setFocusPolicy(Qt::StrongFocus);
    connect(m_formManager, &PDFFormManager::unsupportedFieldClicked, this,
            [this](const PDFFormManager::FieldInfo&) {
                QMessageBox::information(this, tr("미지원 필드"),
                    tr("이 폼 필드 유형은 지원하지 않습니다 (서명/XFA 등)."));
            });
}

QPDFView::~QPDFView()
{
    qDebug().noquote() << "[QPDFView] dtor begin" << this << "file=" << m_filePath;
    closeFileInternal(false);
    teardownUiForDestruction();
    qDebug().noquote() << "[QPDFView] dtor end" << this;
}

// ═══════════════════════════════════════════════════════════
void QPDFView::setupUI()
{
    m_splitter    = new QSplitter(Qt::Horizontal, this);
    m_sidebarTabs = new QTabWidget(this);
    m_thumbList   = new QListWidget(this);
    m_bookmarkTree= new QTreeWidget(this);
    m_scrollArea  = new QScrollArea(this);
    m_scrollArea->setObjectName(QStringLiteral("canvasScrollArea"));
    m_scrollArea->setFocusProxy(this);
    m_scrollArea->installEventFilter(this);
    m_scrollArea->verticalScrollBar()->installEventFilter(this);
    m_scrollArea->horizontalScrollBar()->installEventFilter(this);
    m_pageLabel   = new QLabel(this);

    // 사이드바 탭 (썸네일 + 목차)
    m_thumbList->setIconSize(QSize(120, 160));
    m_bookmarkTree->setHeaderHidden(true);
    m_sidebarTabs->addTab(m_thumbList, tr("썸네일"));
    m_sidebarTabs->addTab(m_bookmarkTree, tr("목차"));
    m_sidebarTabs->setMaximumWidth(220);
    m_sidebarTabs->setMinimumWidth(120);

    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_pageLabel->setMouseTracking(true);
    m_pageLabel->installEventFilter(this);
    m_scrollArea->setWidget(m_pageLabel);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setAlignment(Qt::AlignCenter);
    m_scrollArea->viewport()->setMouseTracking(true);
    m_scrollArea->viewport()->installEventFilter(this);
    m_textSelectionBand = new QRubberBand(QRubberBand::Rectangle, m_scrollArea->viewport());
    m_textSelectionBand->hide();
    m_horizontalScrollAnimation = new QPropertyAnimation(m_scrollArea->horizontalScrollBar(), "value", this);
    m_verticalScrollAnimation = new QPropertyAnimation(m_scrollArea->verticalScrollBar(), "value", this);
    m_thumbnailTimer = new QTimer(this);
    m_thumbnailTimer->setSingleShot(false);
    connect(m_thumbnailTimer, &QTimer::timeout, this, &QPDFView::generateNextThumbnail);

    connect(m_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]{
        if (m_watermarkOverlay) m_watermarkOverlay->update();
    });
    connect(m_scrollArea->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]{
        if (m_watermarkOverlay) m_watermarkOverlay->update();
    });

    m_splitter->addWidget(m_sidebarTabs);
    m_splitter->addWidget(m_scrollArea);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);

    layout()->addWidget(m_splitter);

    connect(m_thumbList, &QListWidget::currentRowChanged, this, &QPDFView::goToPage);
    connect(m_bookmarkTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        int page = item->data(0, Qt::UserRole).toInt();
        goToPage(page);
    });
}

// ═══════════════════════════════════════════════════════════
bool QPDFView::openFile(const QString& filePath)
{
    Q_UNUSED(filePath);
    return false;
}

bool QPDFView::saveFile(const QString& filePath)
{
    Q_UNUSED(filePath);
    return false;
}

bool QPDFView::saveFileAs()
{
    emit SaveRequested(); return false;
}

void QPDFView::closeFile()
{
    m_openRequestId.fetch_add(1);
    endLoading();
    closeFileInternal(true);
}

void QPDFView::closeFileInternal(bool emitSignals)
{
    if (m_closeFileInProgress) {
        qDebug().noquote() << "[QPDFView] closeFile reentry ignored" << this << "file=" << m_filePath;
        return;
    }

    const bool hadResources = m_document
        || !m_fileData.isEmpty()
        || m_pageCount > 0
        || !m_filePath.isEmpty()
        || m_selectedImage.isValid()
        || !m_selectedText.isEmpty();

    if (!hadResources && !emitSignals) {
        qDebug().noquote() << "[QPDFView] closeFile skipped (already closed)" << this;
        return;
    }

    m_closeFileInProgress = true;
    const QString closingFilePath = m_filePath;
    qDebug().noquote() << "[QPDFView] closeFile begin"
                       << this
                       << "emitSignals=" << emitSignals
                       << "file=" << closingFilePath
                       << "doc=" << m_document
                       << "pages=" << m_pageCount;

    if (m_horizontalScrollAnimation) {
        m_horizontalScrollAnimation->stop();
        m_horizontalScrollAnimation->setTargetObject(nullptr);
    }
    if (m_verticalScrollAnimation) {
        m_verticalScrollAnimation->stop();
        m_verticalScrollAnimation->setTargetObject(nullptr);
    }
    if (m_thumbnailTimer)
        m_thumbnailTimer->stop();
    m_pageCache.clear();
    clearSearchHighlights();
    clearSelectionState();
    m_formManager->closeForm();
    m_annotationManager.setDocument(nullptr);
    if (m_document) { FPDF_CloseDocument(m_document); m_document = nullptr; }
    m_fileData.clear();
    m_pageCount = 0; m_currentPage = 0; setModifiedState(false);
    m_searchPage = 0; m_searchMatchStart = -1; m_searchMatchCount = 0;
    m_searchResultIndex = -1; m_searchResultTotal = 0;
    m_nextThumbnailPage = 0;
    m_annotationTool = AnnotationNone;
    m_annotationDragging = false;
    m_annotationAnchorPage = -1;
    m_annotationInkPoints.clear();
    m_filePath.clear();
    if (m_pageLabel)    m_pageLabel->clear();
    if (m_thumbList)    m_thumbList->clear();
    if (m_bookmarkTree) m_bookmarkTree->clear();
    if (emitSignals)
        emit fileClosed();
    updateCopyAvailability();
    qDebug().noquote() << "[QPDFView] closeFile end" << this << "previousFile=" << closingFilePath;
    m_closeFileInProgress = false;
}

void QPDFView::teardownUiForDestruction()
{
    qDebug().noquote() << "[QPDFView] teardownUi begin" << this;

    auto removeEventFilters = [this](QWidget* widget, const char* name) {
        if (!widget)
            return;
        qDebug().noquote() << "[QPDFView] removeEventFilter" << name << widget;
        widget->removeEventFilter(this);
        widget->blockSignals(true);
        widget->hide();
    };

    removeEventFilters(m_pageLabel, "pageLabel");
    if (m_scrollArea)
        removeEventFilters(m_scrollArea->viewport(), "scrollViewport");

    if (m_horizontalScrollAnimation) {
        qDebug().noquote() << "[QPDFView] delete horizontal animation" << m_horizontalScrollAnimation;
        m_horizontalScrollAnimation->stop();
        m_horizontalScrollAnimation->setTargetObject(nullptr);
        delete m_horizontalScrollAnimation;
        m_horizontalScrollAnimation = nullptr;
    }

    if (m_verticalScrollAnimation) {
        qDebug().noquote() << "[QPDFView] delete vertical animation" << m_verticalScrollAnimation;
        m_verticalScrollAnimation->stop();
        m_verticalScrollAnimation->setTargetObject(nullptr);
        delete m_verticalScrollAnimation;
        m_verticalScrollAnimation = nullptr;
    }

    if (m_textSelectionBand) {
        qDebug().noquote() << "[QPDFView] delete selection band" << m_textSelectionBand;
        m_textSelectionBand->hide();
        delete m_textSelectionBand;
        m_textSelectionBand = nullptr;
    }

    if (m_scrollArea && m_pageLabel && m_scrollArea->widget() == m_pageLabel) {
        qDebug().noquote() << "[QPDFView] take pageLabel from scrollArea" << m_pageLabel;
        m_scrollArea->takeWidget();
    }

    if (m_pageLabel) {
        qDebug().noquote() << "[QPDFView] delete pageLabel" << m_pageLabel;
        m_pageLabel->clear();
        delete m_pageLabel;
        m_pageLabel = nullptr;
    }

    if (m_formManager) {
        qDebug().noquote() << "[QPDFView] delete formManager" << m_formManager;
        delete m_formManager;
        m_formManager = nullptr;
    }

    m_scrollArea = nullptr;
    m_thumbList = nullptr;
    m_bookmarkTree = nullptr;
    m_sidebarTabs = nullptr;
    m_splitter = nullptr;

    qDebug().noquote() << "[QPDFView] teardownUi end" << this;
}

bool QPDFView::isModified() const { return m_modified; }
QStringList QPDFView::supportedExtensions() const { return {"pdf"}; }

bool QPDFView::opensFileAsynchronously() const
{
    return true;
}

void QPDFView::setTheme(Theme theme)
{
    QBaseView::setTheme(theme);
    m_pageCache.clear();
    if (m_document && m_pageCount > 0)
        renderCurrentPage();
}

// ═══════════════════════════════════════════════════════════
int  QPDFView::currentPage() const { return m_currentPage; }
int  QPDFView::totalPageCount() const { return m_pageCount; }
void QPDFView::goToPage(int page)
{
    if (!m_document || page < 0 || page >= m_pageCount) return;
    m_currentPage = page;
    m_pageCache.clear();
    renderCurrentPage();
    emit pageChanged(m_currentPage + 1, m_pageCount);
    emit viewStateChanged();
}
void QPDFView::nextPage()     { goToPage(m_currentPage + (m_viewMode == TwoPageSpread ? 2 : 1)); }
void QPDFView::previousPage() { goToPage(m_currentPage - (m_viewMode == TwoPageSpread ? 2 : 1)); }

void QPDFView::setZoomMode(ZoomMode mode)
{
    m_zoomMode = mode;
    m_pageCache.clear();
    renderCurrentPage();
    emit viewStateChanged();
}

void QPDFView::setZoomLevel(qreal pct)
{
    m_zoomLevel = qBound(10.0, pct, 800.0);
    m_zoomMode = CustomZoom;
    m_pageCache.clear();
    renderCurrentPage();
    emit viewStateChanged();
}
QPDFView::ZoomMode QPDFView::zoomMode() const { return m_zoomMode; }
qreal QPDFView::zoomLevel() const          { return m_zoomLevel; }

void QPDFView::setSidebarPosition(SidebarPos pos)
{
    m_sidebarPos = pos;
    if (pos == SidebarHidden) {
        m_sidebarTabs->hide();
    } else {
        m_sidebarTabs->show();
        m_splitter->insertWidget(pos == SidebarLeft ? 0 : 1, m_sidebarTabs);
    }
}
QPDFView::SidebarPos QPDFView::sidebarPosition() const { return m_sidebarPos; }

void QPDFView::setViewMode(ViewMode mode)
{
    if (m_viewMode == mode)
        return;

    m_viewMode = mode;
    if (m_viewMode == TwoPageSpread && (m_currentPage % 2) != 0)
        --m_currentPage;

    m_pageCache.clear();
    renderCurrentPage();
    emit pageChanged(m_currentPage + 1, m_pageCount);
    emit viewStateChanged();
}
QPDFView::ViewMode QPDFView::viewMode() const { return m_viewMode; }

void QPDFView::setSpreadReadingDirection(SpreadReadingDirection direction)
{
    if (m_spreadReadingDirection == direction)
        return;

    m_spreadReadingDirection = direction;
    m_pageCache.clear();
    renderCurrentPage();
    emit viewStateChanged();
}

QPDFView::SpreadReadingDirection QPDFView::spreadReadingDirection() const
{
    return m_spreadReadingDirection;
}

void QPDFView::setRotationAngle(RotationAngle angle)
{
    if (!m_document) return;
    auto Page=FPDF_LoadPage(m_document,m_currentPage); if (!Page) return;
    FPDFPage_SetRotation(Page,(int(angle)/90)%4); FPDF_ClosePage(Page);
    m_rotationAngle=Rotate0; m_pageCache.clear(); setModifiedState(true); renderCurrentPage(); emit viewStateChanged();
}

QPDFView::RotationAngle QPDFView::rotationAngle() const
{
    return m_rotationAngle;
}

void QPDFView::setModifiedState(bool modified)
{
    m_modified=modified; emit modifiedChanged(modified); emit ContentChanged();
}

void QPDFView::setAnnotationTool(AnnotationTool tool)
{
    if (m_annotationTool == tool)
        return;

    m_annotationTool = tool;
    if (m_annotationTool != AnnotationInk)
        m_annotationInkPoints.clear();
    updateHoverCursor(QPoint(-1, -1));
    emit viewStateChanged();
}

void QPDFView::setAnnotationToolBarVisible(bool visible)
{
    m_annotationToolBarVisible = visible;
    if (m_annotationToolBar)
        m_annotationToolBar->setVisible(visible && isToolBarVisible());
    emit viewStateChanged();
}

PDFAnnotationManager::PdfRect QPDFView::toAnnotationRect(const PdfRect& rect) const
{
    return {rect.left, rect.right, rect.bottom, rect.top};
}

bool QPDFView::commitAnnotationChange(bool success)
{
    if (!success)
        return false;

    clearSelectionState();
    m_annotationDragging = false;
    m_annotationAnchorPage = -1;
    m_annotationInkPoints.clear();
    if (m_textSelectionBand)
        m_textSelectionBand->hide();
    m_pageCache.clear();
    renderCurrentPage();
    setModifiedState(true);
    updateCopyAvailability();
    return true;
}

bool QPDFView::addMarkupAnnotationFromRect(int pageIndex, const PdfRect& rect)
{
    if (!rect.isValid())
        return false;

    FPDF_ANNOTATION_SUBTYPE subtype = FPDF_ANNOT_HIGHLIGHT;
    switch (m_annotationTool) {
        case AnnotationUnderline:
            subtype = FPDF_ANNOT_UNDERLINE;
            break;
        case AnnotationStrikeOut:
            subtype = FPDF_ANNOT_STRIKEOUT;
            break;
        case AnnotationHighlight:
        default:
            subtype = FPDF_ANNOT_HIGHLIGHT;
            break;
    }

    return commitAnnotationChange(m_annotationManager.addTextMarkup(pageIndex,
                                                                    subtype,
                                                                    toAnnotationRect(rect),
                                                                    m_annotationColor,
                                                                    m_annotationThickness));
}

bool QPDFView::addFreeTextAnnotationFromRect(int pageIndex, const PdfRect& rect)
{
    if (!rect.isValid())
        return false;

    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(this,
                                                        tr("자유 텍스트"),
                                                        tr("PDF에 추가할 텍스트를 입력하세요."),
                                                        {},
                                                        &ok);
    if (!ok || text.trimmed().isEmpty())
        return false;

    return commitAnnotationChange(m_annotationManager.addFreeText(pageIndex,
                                                                  toAnnotationRect(rect),
                                                                  text,
                                                                  m_annotationFont,
                                                                  m_annotationFontSize,
                                                                  m_annotationColor));
}

bool QPDFView::addNoteAnnotationAt(int pageIndex, const QPointF& pdfPoint)
{
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(this,
                                                        tr("팝업 노트"),
                                                        tr("노트 내용을 입력하세요."),
                                                        {},
                                                        &ok);
    if (!ok || text.trimmed().isEmpty())
        return false;

    return commitAnnotationChange(m_annotationManager.addTextNote(pageIndex,
                                                                  pdfPoint,
                                                                  text,
                                                                  m_annotationColor));
}

bool QPDFView::hasTextSelection() const
{
    return !m_selectedText.trimmed().isEmpty();
}

bool QPDFView::hasSearchResults() const
{
    return m_searchResultTotal > 0;
}

int QPDFView::currentSearchResultIndex() const
{
    return hasSearchResults() ? m_searchResultIndex : 0;
}

int QPDFView::totalSearchResultCount() const
{
    return m_searchResultTotal;
}

int QPDFView::selectedTextLength() const
{
    if (!hasTextSelection())
        return 0;

    const qsizetype maxLength = static_cast<qsizetype>(INT_MAX);
    const qsizetype clampedLength = qMin(m_selectedText.size(), maxLength);
    return static_cast<int>(clampedLength);
}

bool QPDFView::hasImageSelection() const
{
    return m_selectedImage.isValid();
}

// ═══════════════════════════════════════════════════════════
// 렌더링
// ═══════════════════════════════════════════════════════════
QImage QPDFView::renderPageToImage(int pageIndex, qreal scale)
{
    if (!m_document || pageIndex < 0 || pageIndex >= m_pageCount) return {};
    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page) return {};

    if (!qIsFinite(scale) || scale <= 0.0)
        scale = 1.0;

    const double Width=FPDF_GetPageWidth(page)*scale,Height=FPDF_GetPageHeight(page)*scale;
    if(!qIsFinite(Width)||!qIsFinite(Height)||Width<=0||Height<=0||Width>16384||Height>16384){FPDF_ClosePage(page);return {};}
    int w = qMax(1, static_cast<int>(Width));
    int h = qMax(1, static_cast<int>(FPDF_GetPageHeight(page) * scale));

    if(std::uint64_t(w)*h>16*1024*1024){FPDF_ClosePage(page);return {};}
    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()){FPDF_ClosePage(page);return {};}
    img.fill(Qt::white);

    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, img.bits(), static_cast<int>(img.bytesPerLine()));
    if(!bitmap){FPDF_ClosePage(page);return {};}
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, FPDF_ANNOT | FPDF_PRINTING);

    if (m_formManager->handle())
        FPDF_FFLDraw(m_formManager->handle(), bitmap, page, 0, 0, w, h, 0, FPDF_ANNOT);

    FPDFBitmap_Destroy(bitmap);

    if (m_rotationAngle != Rotate0) {
        QTransform transform;
        transform.rotate(static_cast<int>(m_rotationAngle));
        img = img.transformed(transform, Qt::SmoothTransformation);
    }

    QPainter overlayPainter(&img);
    overlayPainter.setRenderHint(QPainter::Antialiasing);

    for (const SearchResult& result : m_searchHighlights) {
        if (result.pageIndex != pageIndex)
            continue;

        const QColor fillColor = result.startIndex == m_searchMatchStart
            ? ThemeManager::instance().color(QStringLiteral("pdf.searchCurrent"))
            : ThemeManager::instance().color(QStringLiteral("pdf.searchHighlight"));
        QColor borderColor = fillColor;
        borderColor.setAlpha(qMin(255, qMax(160, fillColor.alpha() + 90)));
        overlayPainter.setPen(QPen(borderColor, 2));
        overlayPainter.setBrush(fillColor);
        for (const PdfRect& rect : result.rects)
            overlayPainter.drawRect(mapPdfRectToImageRect(rect, pageIndex, scale));
    }

    if (m_selectedTextPage == pageIndex && m_selectedTextBounds.isValid()) {
        QColor textSelection = ThemeManager::instance().color(QStringLiteral("pdf.textSelection"));
        QColor textSelectionBorder = textSelection;
        textSelectionBorder.setAlpha(qMin(255, qMax(180, textSelection.alpha() + 120)));
        overlayPainter.setPen(QPen(textSelectionBorder, 2));
        overlayPainter.setBrush(textSelection);
        overlayPainter.drawRect(mapPdfRectToImageRect(m_selectedTextBounds, pageIndex, scale));
    }

    if (m_selectedImage.pageIndex == pageIndex && m_selectedImage.bounds.isValid()) {
        overlayPainter.setPen(QPen(ThemeManager::instance().color(QStringLiteral("pdf.imageSelection")), 3));
        overlayPainter.setBrush(Qt::NoBrush);
        overlayPainter.drawRect(mapPdfRectToImageRect(m_selectedImage.bounds, pageIndex, scale));
    }
    overlayPainter.end();

    FPDF_ClosePage(page);
    return img;
}

QSizeF QPDFView::pageSize(int pageIndex) const
{
    if (!m_document || pageIndex < 0 || pageIndex >= m_pageCount)
        return {};

    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page)
        return {};

    const QSizeF size(FPDF_GetPageWidth(page), FPDF_GetPageHeight(page));
    FPDF_ClosePage(page);
    return size;
}

QSizeF QPDFView::rotatedPageSize(int pageIndex) const
{
    const QSizeF size = pageSize(pageIndex);
    if (m_rotationAngle == Rotate90 || m_rotationAngle == Rotate270)
        return {size.height(), size.width()};
    return size;
}

QPair<int, int> QPDFView::spreadPagePair(int firstPageIndex) const
{
    if (firstPageIndex < 0 || firstPageIndex >= m_pageCount)
        return {-1, -1};

    const int secondPageIndex = firstPageIndex + 1 < m_pageCount ? firstPageIndex + 1 : -1;
    if (m_spreadReadingDirection == LeftToRight)
        return {firstPageIndex, secondPageIndex};

    if (secondPageIndex >= 0)
        return {secondPageIndex, firstPageIndex};
    return {firstPageIndex, -1};
}

QPointF QPDFView::mapPdfPointToImagePoint(const QPointF& pdfPoint, int pageIndex, qreal scale) const
{
    const QSizeF baseSize = pageSize(pageIndex);
    if (!baseSize.isValid() || scale <= 0.0)
        return {};

    const qreal width = baseSize.width();
    const qreal height = baseSize.height();

    switch (m_rotationAngle) {
        case Rotate90:
            return {pdfPoint.y() * scale, pdfPoint.x() * scale};
        case Rotate180:
            return {(width - pdfPoint.x()) * scale, pdfPoint.y() * scale};
        case Rotate270:
            return {(height - pdfPoint.y()) * scale, (width - pdfPoint.x()) * scale};
        case Rotate0:
        default:
            return {pdfPoint.x() * scale, (height - pdfPoint.y()) * scale};
    }
}

QPointF QPDFView::mapImagePointToPdfPoint(const QPointF& imagePoint, int pageIndex, qreal scale) const
{
    const QSizeF baseSize = pageSize(pageIndex);
    if (!baseSize.isValid() || scale <= 0.0)
        return {};

    const qreal width = baseSize.width();
    const qreal height = baseSize.height();
    const qreal x = imagePoint.x() / scale;
    const qreal y = imagePoint.y() / scale;

    switch (m_rotationAngle) {
        case Rotate90:
            return {y, x};
        case Rotate180:
            return {width - x, y};
        case Rotate270:
            return {width - y, height - x};
        case Rotate0:
        default:
            return {x, height - y};
    }
}

QRectF QPDFView::mapPdfRectToImageRect(const PdfRect& rect, int pageIndex, qreal scale) const
{
    if (!rect.isValid())
        return {};

    const QPointF p1 = mapPdfPointToImagePoint(QPointF(rect.left, rect.bottom), pageIndex, scale);
    const QPointF p2 = mapPdfPointToImagePoint(QPointF(rect.left, rect.top), pageIndex, scale);
    const QPointF p3 = mapPdfPointToImagePoint(QPointF(rect.right, rect.bottom), pageIndex, scale);
    const QPointF p4 = mapPdfPointToImagePoint(QPointF(rect.right, rect.top), pageIndex, scale);

    const qreal minX = qMin(qMin(p1.x(), p2.x()), qMin(p3.x(), p4.x()));
    const qreal maxX = qMax(qMax(p1.x(), p2.x()), qMax(p3.x(), p4.x()));
    const qreal minY = qMin(qMin(p1.y(), p2.y()), qMin(p3.y(), p4.y()));
    const qreal maxY = qMax(qMax(p1.y(), p2.y()), qMax(p3.y(), p4.y()));
    return QRectF(QPointF(minX, minY), QPointF(maxX, maxY)).normalized();
}

QPDFView::PdfRect QPDFView::mapImageRectToPdfRect(const QRectF& imageRect, int pageIndex, qreal scale) const
{
    const QPointF p1 = mapImagePointToPdfPoint(imageRect.topLeft(), pageIndex, scale);
    const QPointF p2 = mapImagePointToPdfPoint(imageRect.topRight(), pageIndex, scale);
    const QPointF p3 = mapImagePointToPdfPoint(imageRect.bottomLeft(), pageIndex, scale);
    const QPointF p4 = mapImagePointToPdfPoint(imageRect.bottomRight(), pageIndex, scale);

    PdfRect result;
    result.left = qMin(qMin(p1.x(), p2.x()), qMin(p3.x(), p4.x()));
    result.right = qMax(qMax(p1.x(), p2.x()), qMax(p3.x(), p4.x()));
    result.bottom = qMin(qMin(p1.y(), p2.y()), qMin(p3.y(), p4.y()));
    result.top = qMax(qMax(p1.y(), p2.y()), qMax(p3.y(), p4.y()));
    return result;
}

QList<QPDFView::PageViewportInfo> QPDFView::currentViewportPages(qreal& scaleRatio) const
{
    QList<PageViewportInfo> pages;
    scaleRatio = m_zoomLevel / 100.0;

    if (!m_document || !m_pageLabel || !m_scrollArea) {
        scaleRatio = 1.0;
        return pages;
    }

    const QPixmap pixmap = m_pageLabel->pixmap();
    if (pixmap.isNull()) {
        scaleRatio = 1.0;
        return pages;
    }

    const auto mapLabelRectToView = [this](const QRectF& labelRect) {
        const QPoint topLeft = m_pageLabel->mapTo(this, labelRect.topLeft().toPoint());
        return QRectF(topLeft, labelRect.size());
    };

    if (m_viewMode == ContinuousScroll) {
        const int gap = 24;
        qreal maxWidth = 0.0;
        QVector<QSizeF> pageSizes;
        pageSizes.reserve(m_pageCount);
        for (int i = 0; i < m_pageCount; ++i) {
            const QSizeF size = rotatedPageSize(i) * scaleRatio;
            if (!size.isValid() || size.isEmpty()) {
                pageSizes.append(QSizeF());
                continue;
            }
            maxWidth = qMax(maxWidth, size.width());
            pageSizes.append(size);
        }

        qreal y = 0.0;
        for (int i = 0; i < pageSizes.size(); ++i) {
            const QSizeF size = pageSizes.at(i);
            if (!size.isValid() || size.isEmpty())
                continue;
            const qreal x = (maxWidth - size.width()) / 2.0;
            pages.append({i, mapLabelRectToView(QRectF(x, y, size.width(), size.height()))});
            y += size.height() + gap;
        }
        return pages;
    }

    if (m_viewMode == TwoPageSpread) {
        const int gap = 24;
        const auto [leftPageIndex, rightPageIndex] = spreadPagePair(m_currentPage);
        const QSizeF leftSize = rotatedPageSize(leftPageIndex) * scaleRatio;
        const QSizeF rightSize = rotatedPageSize(rightPageIndex) * scaleRatio;
        const qreal totalHeight = qMax(leftSize.height(), rightSize.height());
        if (leftPageIndex >= 0 && leftSize.isValid() && !leftSize.isEmpty()) {
            pages.append({leftPageIndex,
                          mapLabelRectToView(QRectF(0.0, (totalHeight - leftSize.height()) / 2.0,
                                                     leftSize.width(), leftSize.height()))});
        }
        if (rightPageIndex >= 0 && rightSize.isValid() && !rightSize.isEmpty()) {
            const qreal leftWidth = leftSize.isValid() ? leftSize.width() : 0.0;
            pages.append({rightPageIndex,
                          mapLabelRectToView(QRectF(leftWidth + gap,
                                                     (totalHeight - rightSize.height()) / 2.0,
                                                     rightSize.width(), rightSize.height()))});
        }
        return pages;
    }

    const QSizeF size = rotatedPageSize(m_currentPage) * scaleRatio;
    if (!size.isValid() || size.isEmpty()) {
        scaleRatio = 1.0;
        return pages;
    }
    pages.append({m_currentPage, mapLabelRectToView(QRectF(QPointF(0.0, 0.0), size))});
    return pages;
}

bool QPDFView::mapViewPointToPage(const QPoint& viewPos, int* pageIndex, QPointF* pdfPoint) const
{
    qreal scaleRatio = 1.0;
    const QList<PageViewportInfo> pages = currentViewportPages(scaleRatio);
    for (const PageViewportInfo& page : pages) {
        if (!page.rect.contains(viewPos))
            continue;

        if (pageIndex)
            *pageIndex = page.pageIndex;
        if (pdfPoint)
            *pdfPoint = mapImagePointToPdfPoint(viewPos - page.rect.topLeft(), page.pageIndex, scaleRatio);
        return true;
    }
    return false;
}

QImage QPDFView::renderSpreadImage(int firstPageIndex, qreal scale)
{
    if (!m_document || firstPageIndex < 0 || firstPageIndex >= m_pageCount)
        return {};

    const int gap = 24;
    const auto [leftPageIndex, rightPageIndex] = spreadPagePair(firstPageIndex);
    const QImage left = leftPageIndex >= 0 ? renderPageToImage(leftPageIndex, scale) : QImage();
    if (left.isNull())
        return {};

    const bool hasRightPage = rightPageIndex >= 0 && rightPageIndex < m_pageCount;
    const QImage right = hasRightPage ? renderPageToImage(rightPageIndex, scale) : QImage();

    const int width = left.width() + (hasRightPage ? gap + right.width() : 0);
    const int height = qMax(left.height(), right.height());
    if(std::uint64_t(qMax(1,width))*qMax(1,height)>16*1024*1024)return {};
    QImage canvas(qMax(1, width), qMax(1, height), QImage::Format_ARGB32);
    canvas.fill(ThemeManager::instance().color(QStringLiteral("pdf.pageGap")));

    QPainter painter(&canvas);
    const int leftY = (height - left.height()) / 2;
    painter.drawImage(0, leftY, left);
    if (hasRightPage) {
        const int rightX = left.width() + gap;
        const int rightY = (height - right.height()) / 2;
        painter.drawImage(rightX, rightY, right);
    }
    painter.end();
    return canvas;
}

QImage QPDFView::renderContinuousImage(qreal scale)
{
    if (!m_document || m_pageCount <= 0)
        return {};

    const int gap = 24;
    QList<QImage> pages;
    pages.reserve(m_pageCount);

    int maxWidth = 1;
    int totalHeight = 0;
    for (int i = 0; i < m_pageCount; ++i) {
        QImage page = renderPageToImage(i, scale);
        if (page.isNull())
            continue;
        maxWidth = qMax(maxWidth, page.width());
        totalHeight += page.height();
        if(std::uint64_t(qMax(1,maxWidth))*qMax(1,totalHeight)>16*1024*1024)return {};
        pages.append(page);
    }

    if (pages.isEmpty())
        return {};

    totalHeight += gap * (pages.size() - 1);
    QImage canvas(qMax(1, maxWidth), qMax(1, totalHeight), QImage::Format_ARGB32);
    canvas.fill(ThemeManager::instance().color(QStringLiteral("pdf.pageGap")));

    QPainter painter(&canvas);
    int y = 0;
    for (const QImage& page : pages) {
        const int x = (maxWidth - page.width()) / 2;
        painter.drawImage(x, y, page);
        y += page.height() + gap;
    }
    painter.end();
    return canvas;
}

void QPDFView::renderCurrentPageNow()
{
    if (!m_document || m_pageCount <= 0 || m_currentPage < 0 || m_currentPage >= m_pageCount)
        return;

    qreal scale = m_zoomLevel / 100.0;
    if (m_zoomMode != CustomZoom) {
        const qreal viewW = qMax(1, m_scrollArea->viewport()->width());
        const qreal viewH = qMax(1, m_scrollArea->viewport()->height());
        qreal targetWidth = 0.0;
        qreal targetHeight = 0.0;

        if (m_viewMode == ContinuousScroll) {
            for (int i = 0; i < m_pageCount; ++i) {
                const QSizeF size = rotatedPageSize(i);
                if (!size.isValid() || size.isEmpty())
                    continue;
                targetWidth = qMax(targetWidth, size.width());
                targetHeight += size.height();
            }
            if (m_pageCount > 1)
                targetHeight += 24.0 * (m_pageCount - 1);
        } else if (m_viewMode == TwoPageSpread) {
            const auto [leftPageIndex, rightPageIndex] = spreadPagePair(m_currentPage);
            const QSizeF leftSize = rotatedPageSize(leftPageIndex);
            const QSizeF rightSize = rotatedPageSize(rightPageIndex);
            targetWidth = leftSize.width();
            targetHeight = leftSize.height();
            if (!rightSize.isEmpty()) {
                targetWidth += 24.0 + rightSize.width();
                targetHeight = qMax(targetHeight, rightSize.height());
            }
        } else {
            const QSizeF size = rotatedPageSize(m_currentPage);
            targetWidth = size.width();
            targetHeight = size.height();
        }

        if (targetWidth <= 0.0 || targetHeight <= 0.0)
            return;

        if (m_zoomMode == FitWidth)       scale = viewW / targetWidth;
        else if (m_zoomMode == FitHeight) scale = viewH / targetHeight;
        else if (m_zoomMode == FitPage)   scale = qMin(viewW / targetWidth, viewH / targetHeight);

        m_zoomLevel = scale * 100.0;
    }

    if (!qIsFinite(scale) || scale <= 0.0)
        scale = 1.0;

    QImage img;
    if (m_viewMode == TwoPageSpread) {
        img = renderSpreadImage(m_currentPage, scale);
    } else if (m_viewMode == ContinuousScroll) {
        img = renderContinuousImage(scale);
    } else {
        QImage* cached = m_pageCache.object(m_currentPage);
        img = cached ? *cached : renderPageToImage(m_currentPage, scale);
        if (!cached && !img.isNull())
            m_pageCache.insert(m_currentPage,new QImage(img),qMax(1,int(img.sizeInBytes()/1024)));
    }

    if (img.isNull()) {
        m_pageLabel->clear();
        return;
    }

    m_pageLabel->setPixmap(QPixmap::fromImage(img));
    m_pageLabel->resize(img.size());

    emit viewStateChanged();
}

void QPDFView::updateSidebar()
{
    m_thumbList->clear();
    for (int i = 0; i < m_pageCount; ++i) {
        QImage thumb = renderPageToImage(i, 0.2);
        auto* item = new QListWidgetItem(QIcon(QPixmap::fromImage(thumb)), tr("Page %1").arg(i + 1));
        m_thumbList->addItem(item);
        updateLoadingProgress(tr("PDF 썸네일 생성 중..."), i + 1, m_pageCount);
    }
}

void QPDFView::startThumbnailGeneration()
{
    if (!m_thumbList) {
        endLoading();
        return;
    }

    m_thumbList->clear();
    m_nextThumbnailPage = 0;
    if (m_pageCount <= 0) {
        endLoading();
        return;
    }

    updateLoadingProgress(tr("PDF 썸네일 생성 중..."), 0, m_pageCount);
    if (m_thumbnailTimer)
        m_thumbnailTimer->start(0);
}

void QPDFView::generateNextThumbnail()
{
    if (!m_document || !m_thumbList || m_pageCount <= 0) {
        if (m_thumbnailTimer)
            m_thumbnailTimer->stop();
        endLoading();
        return;
    }

    if (m_nextThumbnailPage >= m_pageCount) {
        if (m_thumbnailTimer)
            m_thumbnailTimer->stop();
        if (m_currentPage >= 0 && m_currentPage < m_thumbList->count()) {
            const QSignalBlocker blocker(m_thumbList);
            m_thumbList->setCurrentRow(m_currentPage);
        }
        endLoading();
        return;
    }

    const int pageIndex = m_nextThumbnailPage++;
    QImage thumb = renderPageToImage(pageIndex, 0.2);
    auto* item = new QListWidgetItem(QIcon(QPixmap::fromImage(thumb)), tr("Page %1").arg(pageIndex + 1));
    m_thumbList->addItem(item);
    if (pageIndex == m_currentPage) {
        const QSignalBlocker blocker(m_thumbList);
        m_thumbList->setCurrentRow(m_currentPage);
    }

    updateLoadingProgress(tr("PDF 썸네일 생성 중..."), pageIndex + 1, m_pageCount);
    if (m_nextThumbnailPage >= m_pageCount) {
        if (m_thumbnailTimer)
            m_thumbnailTimer->stop();
        endLoading();
    }
}

// ── 목차/북마크 ──
void QPDFView::loadBookmarks()
{
    m_bookmarkTree->clear();
    if (!m_document) return;
    FPDF_BOOKMARK root = FPDFBookmark_GetFirstChild(m_document, nullptr);
    if (!root) return;
    loadBookmarkChildren(nullptr, nullptr);
}

void QPDFView::loadBookmarkChildren(FPDF_BOOKMARK parent, QTreeWidgetItem* parentItem)
{
    FPDF_BOOKMARK child = FPDFBookmark_GetFirstChild(m_document, parent);
    while (child) {
        // 제목 가져오기
        unsigned long len = FPDFBookmark_GetTitle(child, nullptr, 0);
        QString title;
        if (len > 0) {
            QVector<char16_t> buf(len / 2 + 1, 0);
            FPDFBookmark_GetTitle(child, buf.data(), len);
            title = QString::fromUtf16(buf.data());
        }

        // 대상 페이지
        int page = 0;
        FPDF_DEST dest = FPDFBookmark_GetDest(m_document, child);
        if (dest)
            page = FPDFDest_GetDestPageIndex(m_document, dest);

        auto* item = new QTreeWidgetItem;
        item->setText(0, title);
        item->setData(0, Qt::UserRole, page);

        if (parentItem)
            parentItem->addChild(item);
        else
            m_bookmarkTree->addTopLevelItem(item);

        // 재귀
        loadBookmarkChildren(child, item);

        child = FPDFBookmark_GetNextSibling(m_document, child);
    }
}

// ═══════════════════════════════════════════════════════════
// 검색
// ═══════════════════════════════════════════════════════════
void QPDFView::searchText(const QString& text)
{
    m_searchText = text;
    m_searchPage = m_currentPage;
    m_searchMatchStart = -1;
    m_searchMatchCount = 0;
    m_searchResultIndex = -1;
    m_searchResultTotal = 0;
    if (m_searchText.trimmed().isEmpty()) {
        clearSearchHighlights();
        m_pageCache.clear();
        renderCurrentPage();
        emit viewStateChanged();
        return;
    }
    searchNext();
}

void QPDFView::searchNext()
{
    if (m_searchText.isEmpty() || !m_document) return;

    const QVector<SearchResult> allResults = findAllSearchResults();
    if (allResults.isEmpty()) {
        clearSearchHighlights();
        m_searchMatchStart = -1;
        m_searchMatchCount = 0;
        m_searchResultIndex = -1;
        m_searchResultTotal = 0;
        m_pageCache.clear();
        renderCurrentPage();
        emit viewStateChanged();
        QMessageBox::information(this, tr("검색"), tr("'%1'을(를) 찾을 수 없습니다.").arg(m_searchText));
        return;
    }

    const int currentIndex = searchResultIndexFor(allResults, m_searchPage, m_searchMatchStart);
    int nextIndex = -1;
    if (currentIndex >= 0) {
        nextIndex = (currentIndex + 1) % allResults.size();
    } else {
        for (int i = 0; i < allResults.size(); ++i) {
            const SearchResult& result = allResults.at(i);
            if (result.pageIndex >= m_searchPage) {
                nextIndex = i;
                break;
            }
        }
        if (nextIndex < 0)
            nextIndex = 0;
    }

    applySearchResultSelection(allResults, nextIndex);
}

void QPDFView::searchPrev()
{
    if (m_searchText.isEmpty() || !m_document) return;

    const QVector<SearchResult> allResults = findAllSearchResults();
    if (allResults.isEmpty()) {
        clearSearchHighlights();
        m_searchMatchStart = -1;
        m_searchMatchCount = 0;
        m_searchResultIndex = -1;
        m_searchResultTotal = 0;
        m_pageCache.clear();
        renderCurrentPage();
        emit viewStateChanged();
        QMessageBox::information(this, tr("검색"), tr("'%1'을(를) 찾을 수 없습니다.").arg(m_searchText));
        return;
    }

    const int currentIndex = searchResultIndexFor(allResults, m_searchPage, m_searchMatchStart);
    int prevIndex = -1;
    if (currentIndex >= 0) {
        prevIndex = (currentIndex - 1 + allResults.size()) % allResults.size();
    } else {
        for (int i = allResults.size() - 1; i >= 0; --i) {
            const SearchResult& result = allResults.at(i);
            if (result.pageIndex <= m_searchPage) {
                prevIndex = i;
                break;
            }
        }
        if (prevIndex < 0)
            prevIndex = allResults.size() - 1;
    }

    applySearchResultSelection(allResults, prevIndex);
}

QVector<QPDFView::SearchResult> QPDFView::findSearchResultsOnPage(int pageIndex) const
{
    QVector<SearchResult> results;
    if (!m_document || m_searchText.isEmpty() || pageIndex < 0 || pageIndex >= m_pageCount)
        return results;

    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page)
        return results;

    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) {
        FPDF_ClosePage(page);
        return results;
    }

    FPDF_SCHHANDLE search = FPDFText_FindStart(
        textPage,
        reinterpret_cast<const FPDF_WIDESTRING>(m_searchText.utf16()),
        0,
        0);
    while (search && FPDFText_FindNext(search)) {
        SearchResult result;
        result.pageIndex = pageIndex;
        result.startIndex = FPDFText_GetSchResultIndex(search);
        result.count = FPDFText_GetSchCount(search);

        const int rectCount = FPDFText_CountRects(textPage, result.startIndex, result.count);
        for (int rectIndex = 0; rectIndex < rectCount; ++rectIndex) {
            double left = 0.0;
            double top = 0.0;
            double right = 0.0;
            double bottom = 0.0;
            if (FPDFText_GetRect(textPage, rectIndex, &left, &top, &right, &bottom))
                result.rects.append({left, right, bottom, top});
        }

        if (result.isValid())
            results.append(result);
    }

    if (search)
        FPDFText_FindClose(search);
    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return results;
}

QVector<QPDFView::SearchResult> QPDFView::findAllSearchResults() const
{
    QVector<SearchResult> results;
    if (!m_document || m_searchText.isEmpty() || m_pageCount <= 0)
        return results;

    for (int pageIndex = 0; pageIndex < m_pageCount; ++pageIndex) {
        const QVector<SearchResult> pageResults = findSearchResultsOnPage(pageIndex);
        results.reserve(results.size() + pageResults.size());
        for (const SearchResult& result : pageResults)
            results.append(result);
    }
    return results;
}

int QPDFView::searchResultIndexFor(const QVector<SearchResult>& results, int pageIndex, int startIndex) const
{
    if (pageIndex < 0 || startIndex < 0)
        return -1;

    for (int i = 0; i < results.size(); ++i) {
        const SearchResult& result = results.at(i);
        if (result.pageIndex == pageIndex && result.startIndex == startIndex)
            return i;
    }
    return -1;
}

void QPDFView::applySearchResultSelection(const QVector<SearchResult>& results, int resultIndex)
{
    if (resultIndex < 0 || resultIndex >= results.size())
        return;

    const SearchResult& result = results.at(resultIndex);
    if (!result.isValid())
        return;

    if (m_currentPage != result.pageIndex)
        goToPage(result.pageIndex);

    updateSearchHighlights(result.pageIndex, result.startIndex);
    m_searchPage = result.pageIndex;
    m_searchMatchStart = result.startIndex;
    m_searchMatchCount = result.count;
    m_searchResultIndex = resultIndex + 1;
    m_searchResultTotal = results.size();
    m_pageCache.clear();
    renderCurrentPage();
    revealSearchResult(result);
    emit viewStateChanged();
}

void QPDFView::updateSearchHighlights(int pageIndex, int activeStartIndex)
{
    m_searchHighlights = findSearchResultsOnPage(pageIndex);
    if (activeStartIndex >= 0)
        m_searchMatchStart = activeStartIndex;
}

void QPDFView::clearSearchHighlights()
{
    m_searchHighlights.clear();
}

void QPDFView::revealSearchResult(const SearchResult& result)
{
    if (!result.isValid() || !m_pageLabel || !m_scrollArea)
        return;

    qreal scaleRatio = 1.0;
    const QList<PageViewportInfo> pages = currentViewportPages(scaleRatio);
    if (!qIsFinite(scaleRatio) || scaleRatio <= 0.0)
        scaleRatio = 1.0;

    QRectF pageRectInLabel;
    for (const PageViewportInfo& pageInfo : pages) {
        if (pageInfo.pageIndex != result.pageIndex)
            continue;

        pageRectInLabel = QRectF(m_pageLabel->mapFrom(this, pageInfo.rect.topLeft().toPoint()), pageInfo.rect.size());
        break;
    }

    if (!pageRectInLabel.isValid() || pageRectInLabel.isEmpty())
        return;

    QRectF activeRect;
    for (const PdfRect& rect : result.rects) {
        const QRectF mappedRect = mapPdfRectToImageRect(rect, result.pageIndex, scaleRatio).translated(pageRectInLabel.topLeft());
        if (!mappedRect.isValid() || mappedRect.isEmpty())
            continue;
        activeRect = activeRect.isNull() ? mappedRect : activeRect.united(mappedRect);
    }

    const QRectF targetRect = activeRect.isValid() && !activeRect.isEmpty() ? activeRect : pageRectInLabel;
    animateScrollToRect(targetRect);
}

void QPDFView::animateScrollToRect(const QRectF& targetRectInLabel)
{
    if (!m_scrollArea)
        return;

    QScrollBar* horizontalScrollBar = m_scrollArea->horizontalScrollBar();
    QScrollBar* verticalScrollBar = m_scrollArea->verticalScrollBar();
    QWidget* viewport = m_scrollArea->viewport();
    if (!horizontalScrollBar || !verticalScrollBar || !viewport)
        return;

    const QRectF expandedTarget = targetRectInLabel.marginsAdded(QMarginsF(36.0, 28.0, 36.0, 28.0));
    const qreal centeredX = expandedTarget.center().x() - (viewport->width() / 2.0);
    const qreal centeredY = expandedTarget.center().y() - (viewport->height() / 2.0);

    const int targetX = qBound(horizontalScrollBar->minimum(),
                               qRound(centeredX),
                               horizontalScrollBar->maximum());
    const int targetY = qBound(verticalScrollBar->minimum(),
                               qRound(centeredY),
                               verticalScrollBar->maximum());

    animateScrollBarTo(horizontalScrollBar, m_horizontalScrollAnimation, targetX);
    animateScrollBarTo(verticalScrollBar, m_verticalScrollAnimation, targetY);
}

void QPDFView::animateScrollBarTo(QScrollBar* scrollBar, QPropertyAnimation* animation, int targetValue)
{
    if (!scrollBar || !animation)
        return;

    const int startValue = scrollBar->value();
    const int distance = qAbs(targetValue - startValue);
    if (distance <= 2) {
        scrollBar->setValue(targetValue);
        return;
    }

    animation->stop();
    animation->setStartValue(startValue);
    animation->setEndValue(targetValue);
    animation->setDuration(qBound(120, 120 + distance / 4, 260));
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->start();
}

void QPDFView::clearSelectionState()
{
    m_selectedText.clear();
    m_selectedTextPage = -1;
    m_selectedTextBounds = {};
    m_selectedImage = {};
    m_selectionAnchorPage = -1;
}

QPDFView::TextHitInfo QPDFView::textHitTestAtViewPoint(const QPoint& viewPos) const
{
    TextHitInfo hitInfo;
    if (!m_document)
        return hitInfo;

    int pageIndex = -1;
    QPointF pdfPoint;
    if (!mapViewPointToPage(viewPos, &pageIndex, &pdfPoint))
        return hitInfo;

    qreal scaleRatio = 1.0;
    const QList<PageViewportInfo> pages = currentViewportPages(scaleRatio);
    Q_UNUSED(pages);
    if (!qIsFinite(scaleRatio) || scaleRatio <= 0.0)
        scaleRatio = 1.0;

    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page)
        return hitInfo;

    FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
    if (!textPage) {
        FPDF_ClosePage(page);
        return hitInfo;
    }

    const double pdfTolerance = qBound(0.15, 0.85 / scaleRatio, 0.6);
    const int charIndex = FPDFText_GetCharIndexAtPos(textPage,
                                                     pdfPoint.x(),
                                                     pdfPoint.y(),
                                                     pdfTolerance,
                                                     pdfTolerance);
    if (charIndex >= 0) {
        FS_RECTF looseRect{};
        bool hasBounds = false;
        PdfRect bounds;
        if (FPDFText_GetLooseCharBox(textPage, charIndex, &looseRect)) {
            bounds = {looseRect.left, looseRect.right, looseRect.bottom, looseRect.top};
            hasBounds = bounds.isValid();
        }
        if (!hasBounds) {
            double left = 0.0;
            double right = 0.0;
            double bottom = 0.0;
            double top = 0.0;
            if (FPDFText_GetCharBox(textPage, charIndex, &left, &right, &bottom, &top)) {
                bounds = {left, right, bottom, top};
                hasBounds = bounds.isValid();
            }
        }

        if (hasBounds) {
            const qreal hitPadding = qBound(0.05, 0.55 / scaleRatio, 0.35);
            const bool insideBounds = pdfPoint.x() >= (bounds.left - hitPadding)
                                   && pdfPoint.x() <= (bounds.right + hitPadding)
                                   && pdfPoint.y() >= (bounds.bottom - hitPadding)
                                   && pdfPoint.y() <= (bounds.top + hitPadding);
            if (insideBounds) {
                hitInfo.hit = true;
                hitInfo.pageIndex = pageIndex;
                hitInfo.charIndex = charIndex;
                hitInfo.bounds = bounds;
            }
        }
    }

    FPDFText_ClosePage(textPage);
    FPDF_ClosePage(page);
    return hitInfo;
}

bool QPDFView::hasTextAtViewPoint(const QPoint& viewPos) const
{
    return textHitTestAtViewPoint(viewPos).hit;
}

void QPDFView::updateHoverCursor(const QPoint& viewPos, Qt::MouseButtons buttons)
{
    const Qt::CursorShape cursorShape = (m_annotationTool != AnnotationNone)
        ? Qt::CrossCursor
        : (m_dragSelectingText || (buttons & Qt::LeftButton) || hasTextAtViewPoint(viewPos))
        ? Qt::IBeamCursor
        : Qt::ArrowCursor;

    auto applyCursor = [cursorShape](QWidget* widget) {
        if (!widget)
            return;
        if (widget->cursor().shape() != cursorShape)
            widget->setCursor(cursorShape);
    };

    applyCursor(this);
    applyCursor(m_pageLabel);
    applyCursor(m_scrollArea ? m_scrollArea->viewport() : nullptr);
}

QImage QPDFView::imageFromPdfBitmap(FPDF_BITMAP bitmap)
{
    if (!bitmap)
        return {};

    const int width = FPDFBitmap_GetWidth(bitmap);
    const int height = FPDFBitmap_GetHeight(bitmap);
    const int stride = FPDFBitmap_GetStride(bitmap);
    const int format = FPDFBitmap_GetFormat(bitmap);
    const uchar* src = static_cast<const uchar*>(FPDFBitmap_GetBuffer(bitmap));
    if (!src || width <= 0 || height <= 0 || stride <= 0)
        return {};

    switch (format) {
        case FPDFBitmap_Gray: {
            QImage img(width, height, QImage::Format_Grayscale8);
            for (int y = 0; y < height; ++y)
                memcpy(img.scanLine(y), src + y * stride, width);
            return img;
        }
        case FPDFBitmap_BGR: {
            QImage img(width, height, QImage::Format_BGR888);
            for (int y = 0; y < height; ++y)
                memcpy(img.scanLine(y), src + y * stride, width * 3);
            return img;
        }
        case FPDFBitmap_BGRA:
        case FPDFBitmap_BGRA_Premul: {
            QImage img(width, height,
                       format == FPDFBitmap_BGRA_Premul ? QImage::Format_ARGB32_Premultiplied
                                                        : QImage::Format_ARGB32);
            for (int y = 0; y < height; ++y)
                memcpy(img.scanLine(y), src + y * stride, width * 4);
            return img;
        }
        case FPDFBitmap_BGRx:
        default: {
            QImage img(width, height, QImage::Format_ARGB32);
            for (int y = 0; y < height; ++y) {
                const uchar* srcLine = src + y * stride;
                QRgb* dstLine = reinterpret_cast<QRgb*>(img.scanLine(y));
                for (int x = 0; x < width; ++x) {
                    const int offset = x * 4;
                    dstLine[x] = qRgba(srcLine[offset + 2], srcLine[offset + 1], srcLine[offset + 0], 255);
                }
            }
            return img;
        }
    }
}

QPDFView::SelectedImageInfo QPDFView::extractTopmostImageAtPoint(int pageIndex, const QPointF& pdfPoint) const
{
    SelectedImageInfo selection;
    if (!m_document || pageIndex < 0 || pageIndex >= m_pageCount)
        return selection;

    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page)
        return selection;

    const int objectCount = FPDFPage_CountObjects(page);
    for (int objectIndex = objectCount - 1; objectIndex >= 0; --objectIndex) {
        FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, objectIndex);
        if (!object || FPDFPageObj_GetType(object) != FPDF_PAGEOBJ_IMAGE)
            continue;

        float left = 0.0f;
        float bottom = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        if (!FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top))
            continue;

        if (pdfPoint.x() < left || pdfPoint.x() > right || pdfPoint.y() < bottom || pdfPoint.y() > top)
            continue;

        FPDF_BITMAP bitmap = FPDFImageObj_GetBitmap(object);
        if (!bitmap)
            bitmap = FPDFImageObj_GetRenderedBitmap(m_document, page, object);
        if (!bitmap)
            continue;

        selection.pageIndex = pageIndex;
        selection.bounds = {left, right, bottom, top};
        selection.image = imageFromPdfBitmap(bitmap);
        FPDFBitmap_Destroy(bitmap);
        break;
    }

    FPDF_ClosePage(page);
    return selection;
}

bool QPDFView::selectImageAtViewPoint(const QPoint& viewPos)
{
    int pageIndex = -1;
    QPointF pdfPoint;
    if (!mapViewPointToPage(viewPos, &pageIndex, &pdfPoint))
        return false;

    SelectedImageInfo selection = extractTopmostImageAtPoint(pageIndex, pdfPoint);
    if (!selection.isValid())
        return false;

    m_selectedImage = selection;
    m_selectedText.clear();
    m_selectedTextPage = -1;
    m_selectedTextBounds = {};
    return true;
}

// ═══════════════════════════════════════════════════════════
// 인쇄용 렌더링
// ═══════════════════════════════════════════════════════════
void QPDFView::renderPage(QPainter* painter, int pageIndex, const QRectF& rect)
{
    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page) return;
    QImage img = renderPageToImage(pageIndex, rect.width() / FPDF_GetPageWidth(page));
    painter->drawImage(rect, img);
    FPDF_ClosePage(page);
}

int QPDFView::pageCount() const { return m_pageCount; }

// ═══════════════════════════════════════════════════════════
// 도구모음
// ═══════════════════════════════════════════════════════════
QToolBar* QPDFView::createToolBar()
{
    auto* tb = QBaseView::createToolBar();

    connect(MV_CREATE_TOOLBAR_ACTION(tb, "pdf.previousPage"),
            &QAction::triggered, this, &QPDFView::previousPage);

    auto* pageBox = new ReversedPageSpinBox(tb);
    pageBox->setRange(1, qMax(1, m_pageCount));
    pageBox->setValue(m_currentPage + 1);
    connect(pageBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){ goToPage(v-1); });
    connect(this, &QPDFView::pageChanged, pageBox, [pageBox](int cur, int total){
        pageBox->setMaximum(total); pageBox->setValue(cur);
    });
    tb->addWidget(pageBox);

    connect(MV_CREATE_TOOLBAR_ACTION(tb, "pdf.nextPage"),
            &QAction::triggered, this, &QPDFView::nextPage);
    tb->addSeparator();

    auto* zoomCombo = new QComboBox(tb);
    zoomCombo->setEditable(true);
    for (int z : {50, 75, 100, 125, 150, 200, 300})
        zoomCombo->addItem(QStringLiteral("%1%").arg(z), z);
    zoomCombo->setCurrentText(QStringLiteral("%1%").arg(static_cast<int>(m_zoomLevel)));
    connect(zoomCombo, &QComboBox::currentTextChanged, this, [this](const QString& t){
        QString num = t; num.remove('%');
        bool ok; qreal val = num.toDouble(&ok);
        if (ok) setZoomLevel(val);
    });
    tb->addWidget(zoomCombo);

    auto* fitModeGroup = new QActionGroup(tb);
    fitModeGroup->setExclusive(true);

    auto* fitWidthAction = MV_CREATE_TOOLBAR_ACTION(tb, "pdf.fitWidth");
    auto* fitPageAction = MV_CREATE_TOOLBAR_ACTION(tb, "pdf.fitPage");
    fitModeGroup->addAction(fitWidthAction);
    fitModeGroup->addAction(fitPageAction);
    fitWidthAction->setChecked(m_zoomMode == FitWidth);
    fitPageAction->setChecked(m_zoomMode == FitPage);

    connect(fitWidthAction, &QAction::triggered, this, [this]{ setZoomMode(FitWidth); });
    connect(fitPageAction, &QAction::triggered, this, [this]{ setZoomMode(FitPage); });
    connect(this, &QPDFView::viewStateChanged, fitWidthAction, [this, fitWidthAction, fitPageAction] {
        const bool widthChecked = m_zoomMode == FitWidth;
        const bool pageChecked = m_zoomMode == FitPage;
        fitWidthAction->setChecked(widthChecked);
        fitPageAction->setChecked(pageChecked);
    });
    tb->addSeparator();

    auto* viewModeCombo = new QComboBox(tb);
    viewModeCombo->addItem(tr("단일 페이지"), SinglePage);
    viewModeCombo->addItem(tr("연속 스크롤"), ContinuousScroll);
    viewModeCombo->addItem(tr("2장 보기"), TwoPageSpread);
    viewModeCombo->setCurrentIndex(viewModeCombo->findData(m_viewMode));
    connect(viewModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, viewModeCombo](int index) {
                const QVariant data = viewModeCombo->itemData(index);
                if (data.isValid())
                    setViewMode(static_cast<ViewMode>(data.toInt()));
            });
    tb->addWidget(new QLabel(tr(" 보기: "), tb));
    tb->addWidget(viewModeCombo);

    auto* directionCombo = new QComboBox(tb);
    directionCombo->addItem(tr("좌→우"), LeftToRight);
    directionCombo->addItem(tr("우→좌"), RightToLeft);
    directionCombo->setCurrentIndex(directionCombo->findData(m_spreadReadingDirection));
    connect(directionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, directionCombo](int index) {
                const QVariant data = directionCombo->itemData(index);
                if (data.isValid())
                    setSpreadReadingDirection(static_cast<SpreadReadingDirection>(data.toInt()));
            });

    auto* rotationCombo = new QComboBox(tb);
    rotationCombo->addItem(tr("0°"), Rotate0);
    rotationCombo->addItem(tr("90°"), Rotate90);
    rotationCombo->addItem(tr("180°"), Rotate180);
    rotationCombo->addItem(tr("270°"), Rotate270);
    rotationCombo->setCurrentIndex(rotationCombo->findData(m_rotationAngle));
    connect(rotationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, rotationCombo](int index) {
                const QVariant data = rotationCombo->itemData(index);
                if (data.isValid())
                    setRotationAngle(static_cast<RotationAngle>(data.toInt()));
            });
    connect(this, &QPDFView::viewStateChanged, tb, [this, directionCombo, rotationCombo] {
        {
            const QSignalBlocker blocker(directionCombo);
            directionCombo->setCurrentIndex(directionCombo->findData(m_spreadReadingDirection));
            directionCombo->setEnabled(m_viewMode == TwoPageSpread);
        }
        {
            const QSignalBlocker blocker(rotationCombo);
            rotationCombo->setCurrentIndex(rotationCombo->findData(m_rotationAngle));
        }
    });
    directionCombo->setEnabled(m_viewMode == TwoPageSpread);
    tb->addWidget(new QLabel(tr(" 방향: "), tb));
    tb->addWidget(directionCombo);
    tb->addWidget(new QLabel(tr(" 회전: "), tb));
    tb->addWidget(rotationCombo);
    tb->addSeparator();

    // 검색
    auto* searchEdit = new QLineEdit(tb);
    searchEdit->setPlaceholderText(tr("🔍 검색..."));
    searchEdit->setMaximumWidth(180);
    connect(searchEdit, &QLineEdit::returnPressed, this, [this, searchEdit]{
        searchText(searchEdit->text());
    });
    tb->addWidget(searchEdit);
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "pdf.searchNext"),
            &QAction::triggered, this, &QPDFView::searchNext);
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "pdf.searchPrev"),
            &QAction::triggered, this, &QPDFView::searchPrev);
    tb->addSeparator();

    auto* tocAction = MV_CREATE_TOOLBAR_ACTION(tb, "pdf.toggleToc");
    tocAction->setChecked(m_sidebarPos != SidebarHidden);
    connect(this, &QPDFView::viewStateChanged, tocAction, [this, tocAction] {
        tocAction->setChecked(m_sidebarPos != SidebarHidden);
    });
    connect(tocAction, &QAction::triggered, this, [this]{
        setSidebarPosition(m_sidebarPos == SidebarHidden ? SidebarLeft : SidebarHidden);
    });

    tb->addSeparator();
    auto* annotationToolsAction = MV_CREATE_TOOLBAR_ACTION(tb, "pdf.toggleAnnotations");
    annotationToolsAction->setChecked(m_annotationToolBarVisible);
    connect(annotationToolsAction, &QAction::triggered, this, [this](bool checked) {
        setAnnotationToolBarVisible(checked);
    });
    connect(this, &QPDFView::viewStateChanged, annotationToolsAction, [this, annotationToolsAction] {
        annotationToolsAction->setChecked(m_annotationToolBarVisible);
    });

    return tb;
}

QToolBar* QPDFView::createAuxiliaryToolBar()
{
    auto* tb = new QToolBar(this);
    tb->setMovable(false);
    tb->setIconSize(QSize(20, 20));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->setVisible(m_annotationToolBarVisible && isToolBarVisible());
    m_annotationToolBar = tb;
    m_auxiliaryToolBar = tb;
    connect(tb, &QObject::destroyed, this, [this, tb] {
        if (m_annotationToolBar == tb)
            m_annotationToolBar = nullptr;
        if (m_auxiliaryToolBar == tb)
            m_auxiliaryToolBar = nullptr;
    });

    auto* toolGroup = new QActionGroup(tb);
    toolGroup->setExclusive(true);

    const auto addToolAction = [this, tb, toolGroup](const char* key, AnnotationTool tool) {
        auto* action = ToolbarIcons::createAction(tb, QString::fromLatin1(key), __FILE__, __LINE__);
        action->setCheckable(true);
        action->setChecked(m_annotationTool == tool);
        toolGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, tool] {
            setAnnotationTool(tool);
        });
        connect(this, &QPDFView::viewStateChanged, action, [this, action, tool] {
            action->setChecked(m_annotationTool == tool);
        });
    };

    addToolAction("pdf.annot.select", AnnotationNone);
    addToolAction("pdf.annot.highlight", AnnotationHighlight);
    addToolAction("pdf.annot.ink", AnnotationInk);
    addToolAction("pdf.annot.freeText", AnnotationFreeText);
    addToolAction("pdf.annot.underline", AnnotationUnderline);
    addToolAction("pdf.annot.strikeOut", AnnotationStrikeOut);
    addToolAction("pdf.annot.note", AnnotationNote);
    tb->addSeparator();

    auto* colorAction = MV_CREATE_TOOLBAR_ACTION(tb, "widget.colorButton");
    colorAction->setToolTip(tr("어노테이션 색상 선택"));
    connect(colorAction, &QAction::triggered, this, [this] {
        const QColor chosen = QColorDialog::getColor(m_annotationColor, this, tr("어노테이션 색상"), QColorDialog::ShowAlphaChannel);
        if (chosen.isValid()) {
            m_annotationColor = chosen;
            emit viewStateChanged();
        }
    });

    auto* thicknessSpin = new QDoubleSpinBox(tb);
    thicknessSpin->setRange(0.5, 64.0);
    thicknessSpin->setSingleStep(0.5);
    thicknessSpin->setDecimals(1);
    thicknessSpin->setSuffix(tr(" pt"));
    thicknessSpin->setValue(m_annotationThickness);
    thicknessSpin->setToolTip(tr("형광펜/선 굵기"));
    connect(thicknessSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_annotationThickness = value;
    });
    tb->addWidget(new QLabel(tr(" 굵기: "), tb));
    tb->addWidget(thicknessSpin);

    auto* fontCombo = new QFontComboBox(tb);
    fontCombo->setCurrentFont(m_annotationFont);
    fontCombo->setMaximumWidth(180);
    fontCombo->setToolTip(tr("자유 텍스트 글꼴"));
    connect(fontCombo, &QFontComboBox::currentFontChanged, this, [this](const QFont& font) {
        m_annotationFont = font;
    });
    tb->addWidget(new QLabel(tr(" 글꼴: "), tb));
    tb->addWidget(fontCombo);

    auto* fontSizeSpin = new QDoubleSpinBox(tb);
    fontSizeSpin->setRange(4.0, 96.0);
    fontSizeSpin->setSingleStep(1.0);
    fontSizeSpin->setDecimals(0);
    fontSizeSpin->setSuffix(tr(" pt"));
    fontSizeSpin->setValue(m_annotationFontSize);
    fontSizeSpin->setToolTip(tr("자유 텍스트 크기"));
    connect(fontSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_annotationFontSize = value;
    });
    tb->addWidget(new QLabel(tr(" 크기: "), tb));
    tb->addWidget(fontSizeSpin);

    return tb;
}

// ═══════════════════════════════════════════════════════════
// 이벤트
// ═══════════════════════════════════════════════════════════
void QPDFView::wheelEvent(QWheelEvent* event)
{
    auto scrollViewportByWheel = [this](QWheelEvent* e) -> bool {
        if (!m_scrollArea) return false;

        QScrollBar* vbar = m_scrollArea->verticalScrollBar();
        if (!vbar || vbar->maximum() <= vbar->minimum())
            return false;

        int scrollDelta = e->pixelDelta().y();
        if (scrollDelta == 0) {
            const int angleY = e->angleDelta().y();
            if (angleY == 0) return false;
            const int steps = angleY / 120;
            scrollDelta = (steps != 0 ? steps : (angleY > 0 ? 1 : -1)) * qMax(24, vbar->singleStep() * 3);
        }

        const int oldValue = vbar->value();
        const int newValue = qBound(vbar->minimum(), oldValue - scrollDelta, vbar->maximum());
        if (newValue == oldValue)
            return false;

        vbar->setValue(newValue);
        return true;
    };

    if (event->modifiers() & Qt::ControlModifier) {
        setZoomLevel(m_zoomLevel + (event->angleDelta().y() > 0 ? 10.0 : -10.0));
        event->accept();
    } else {
        if (!m_document || m_pageCount <= 0) {
            scrollViewportByWheel(event);
            QBaseView::wheelEvent(event);
            return;
        }

        const int deltaY = event->angleDelta().y();
        if (deltaY == 0) {
            if (!scrollViewportByWheel(event))
                QBaseView::wheelEvent(event);
            return;
        }

        if (m_viewMode == ContinuousScroll) {
            if (!scrollViewportByWheel(event))
                QBaseView::wheelEvent(event);
            else
                event->accept();
            return;
        }

        const bool movingToLowerPage  = deltaY > 0;
        const bool movingToHigherPage = deltaY < 0;
        const bool atFirstPage = (m_currentPage <= 0);
        const bool atLastPage  = (m_currentPage >= m_pageCount - 1);

        if ((movingToLowerPage && atFirstPage) || (movingToHigherPage && atLastPage)) {
            m_wheelRemainder = 0;
            scrollViewportByWheel(event);
            event->accept();
            return;
        }

        if ((m_wheelRemainder > 0 && deltaY < 0) || (m_wheelRemainder < 0 && deltaY > 0))
            m_wheelRemainder = 0;

        m_wheelRemainder += deltaY;

        while (m_wheelRemainder >= 120) {
            previousPage();
            m_wheelRemainder -= 120;
        }
        while (m_wheelRemainder <= -120) {
            nextPage();
            m_wheelRemainder += 120;
        }

        event->accept();
    }
}

void QPDFView::resizeEvent(QResizeEvent* event)
{
    QBaseView::resizeEvent(event);
    if (m_zoomMode != CustomZoom) { m_pageCache.clear(); renderCurrentPage(); }
}

bool QPDFView::canCopy() const
{
    return hasTextSelection() || hasImageSelection();
}

bool QPDFView::copySelection()
{
    if(ClipboardRestricted()){emit ClipboardRequested("copy");return true;}
    if (!canCopy())
        return false;

    if (auto* clipboard = QApplication::clipboard()) {
        if (hasTextSelection()) {
            clipboard->setText(m_selectedText);
            return true;
        }
        if (hasImageSelection()) {
            clipboard->setImage(m_selectedImage.image);
            return true;
        }
    }
    return false;
}

bool QPDFView::eventFilter(QObject* watched, QEvent* event)
{
    if (m_scrollArea && (watched == m_scrollArea || watched == m_pageLabel
        || watched == m_scrollArea->viewport() || watched == m_scrollArea->verticalScrollBar()
        || watched == m_scrollArea->horizontalScrollBar())
        && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)
        && handleNavigationKey(static_cast<QKeyEvent*>(event))) return true;
    if ((watched == m_pageLabel || watched == (m_scrollArea ? m_scrollArea->viewport() : nullptr)) && m_scrollArea) {
        if (event->type() == QEvent::Leave) {
            updateHoverCursor(QPoint(-1, -1), Qt::NoButton);
            return QBaseView::eventFilter(watched, event);
        }

        auto* watchedWidget = qobject_cast<QWidget*>(watched);
        auto* mouseEvent = dynamic_cast<QMouseEvent*>(event);
        if (mouseEvent && watchedWidget) {
            const QPoint viewPos = watchedWidget->mapTo(this, mouseEvent->pos());
            const QPoint viewportPos = watchedWidget->mapTo(m_scrollArea->viewport(), mouseEvent->pos());
            switch (event->type()) {
                case QEvent::MouseButtonPress:
                    if (handleMousePress(viewPos, viewportPos, mouseEvent->button(), mouseEvent->modifiers()))
                        return true;
                    break;
                case QEvent::MouseMove:
                    if (handleMouseMove(viewPos, viewportPos, mouseEvent->buttons()))
                        return true;
                    break;
                case QEvent::MouseButtonRelease:
                    if (handleMouseRelease(viewPos, viewportPos, mouseEvent->button()))
                        return true;
                    break;
                default:
                    break;
            }
        }
    }
    return QBaseView::eventFilter(watched, event);
}

bool QPDFView::handleAnnotationMousePress(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButton button)
{
    if (m_annotationTool == AnnotationNone || !m_document || button != Qt::LeftButton)
        return false;

    int pageIndex = -1;
    QPointF pdfPoint;
    if (!mapViewPointToPage(viewPos, &pageIndex, &pdfPoint))
        return false;

    m_annotationDragging = true;
    m_annotationAnchorPage = pageIndex;
    m_annotationAnchorViewportPos = viewportPos;
    m_annotationAnchorPdfPoint = pdfPoint;
    m_annotationInkPoints.clear();
    if (m_annotationTool == AnnotationInk)
        m_annotationInkPoints.append(pdfPoint);
    else if (m_textSelectionBand && m_annotationTool != AnnotationNote) {
        m_textSelectionBand->setGeometry(QRect(viewportPos, QSize(1, 1)));
        m_textSelectionBand->show();
    }

    updateHoverCursor(viewPos, Qt::LeftButton);
    return true;
}

bool QPDFView::handleAnnotationMouseMove(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButtons buttons)
{
    if (m_annotationTool == AnnotationNone || !m_document)
        return false;

    if (!(buttons & Qt::LeftButton) || !m_annotationDragging) {
        updateHoverCursor(viewPos, buttons);
        return m_annotationTool != AnnotationNone;
    }

    if (m_annotationTool == AnnotationInk) {
        int pageIndex = -1;
        QPointF pdfPoint;
        if (mapViewPointToPage(viewPos, &pageIndex, &pdfPoint) && pageIndex == m_annotationAnchorPage)
            m_annotationInkPoints.append(pdfPoint);
    } else if (m_textSelectionBand && m_annotationTool != AnnotationNote) {
        m_textSelectionBand->setGeometry(QRect(m_annotationAnchorViewportPos, viewportPos).normalized());
    }

    updateHoverCursor(viewPos, buttons);
    return true;
}

bool QPDFView::handleAnnotationMouseRelease(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButton button)
{
    if (m_annotationTool == AnnotationNone || !m_document || button != Qt::LeftButton || !m_annotationDragging)
        return false;

    const int pageIndex = m_annotationAnchorPage;
    m_annotationDragging = false;
    if (m_textSelectionBand)
        m_textSelectionBand->hide();

    if (pageIndex < 0)
        return false;

    if (m_annotationTool == AnnotationNote) {
        int releasePage = -1;
        QPointF pdfPoint;
        if (!mapViewPointToPage(viewPos, &releasePage, &pdfPoint) || releasePage != pageIndex)
            pdfPoint = m_annotationAnchorPdfPoint;
        updateHoverCursor(viewPos, Qt::NoButton);
        return addNoteAnnotationAt(pageIndex, pdfPoint);
    }

    if (m_annotationTool == AnnotationInk) {
        int releasePage = -1;
        QPointF pdfPoint;
        if (mapViewPointToPage(viewPos, &releasePage, &pdfPoint) && releasePage == pageIndex)
            m_annotationInkPoints.append(pdfPoint);
        updateHoverCursor(viewPos, Qt::NoButton);
        return commitAnnotationChange(m_annotationManager.addInkStroke(pageIndex,
                                                                       m_annotationInkPoints,
                                                                       m_annotationColor,
                                                                       m_annotationThickness));
    }

    PdfRect pdfRect;
    const QRect selectionRectInViewport = QRect(m_annotationAnchorViewportPos, viewportPos).normalized();
    if (selectionRectInViewport.width() >= 3 && selectionRectInViewport.height() >= 3) {
        qreal scaleRatio = 1.0;
        const QList<PageViewportInfo> pages = currentViewportPages(scaleRatio);
        for (const PageViewportInfo& pageInfo : pages) {
            if (pageInfo.pageIndex != pageIndex)
                continue;

            const QRectF pageRectInViewport(
                m_scrollArea->viewport()->mapFrom(this, pageInfo.rect.topLeft().toPoint()),
                pageInfo.rect.size());
            const QRectF intersected = pageRectInViewport.intersected(selectionRectInViewport);
            if (!intersected.isValid() || intersected.isEmpty())
                continue;

            const QRectF localRect = QRectF(intersected.topLeft() - pageRectInViewport.topLeft(), intersected.size());
            pdfRect = mapImageRectToPdfRect(localRect, pageIndex, scaleRatio);
            break;
        }
    }

    if (!pdfRect.isValid()) {
        if ((m_annotationTool == AnnotationHighlight
             || m_annotationTool == AnnotationUnderline
             || m_annotationTool == AnnotationStrikeOut)
            && m_selectedTextPage == pageIndex
            && m_selectedTextBounds.isValid()) {
            pdfRect = m_selectedTextBounds;
        } else if (m_annotationTool == AnnotationFreeText) {
            pdfRect = {m_annotationAnchorPdfPoint.x(),
                       m_annotationAnchorPdfPoint.x() + 180.0,
                       m_annotationAnchorPdfPoint.y() - 60.0,
                       m_annotationAnchorPdfPoint.y()};
        }
    }

    updateHoverCursor(viewPos, Qt::NoButton);
    if (!pdfRect.isValid())
        return false;

    if (m_annotationTool == AnnotationFreeText)
        return addFreeTextAnnotationFromRect(pageIndex, pdfRect);

    return addMarkupAnnotationFromRect(pageIndex, pdfRect);
}

bool QPDFView::handleMousePress(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButton button, Qt::KeyboardModifiers /*modifiers*/)
{
    if (handleAnnotationMousePress(viewPos, viewportPos, button))
        return true;

    if (!m_document || button != Qt::LeftButton)
        return false;

    int pageIndex = -1;
    QPointF pdfPoint;
    if (!mapViewPointToPage(viewPos, &pageIndex, &pdfPoint))
        return false;

    m_mouseSelecting = true;
    m_dragSelectingText = false;
    m_selectionAnchorPage = pageIndex;
    m_selectionAnchorViewPos = viewPos;
    m_selectionAnchorViewportPos = viewportPos;
    updateHoverCursor(viewPos, Qt::LeftButton);
    if (m_textSelectionBand)
        m_textSelectionBand->hide();

    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (page) {
        m_formManager->handleMousePress(page, pdfPoint);
        FPDF_ClosePage(page);
    }
    return true;
}

bool QPDFView::handleMouseMove(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButtons buttons)
{
    if (handleAnnotationMouseMove(viewPos, viewportPos, buttons))
        return true;

    if (!m_document)
        return false;

    if (!(buttons & Qt::LeftButton)) {
        updateHoverCursor(viewPos, buttons);
        return false;
    }

    if (m_mouseSelecting && !m_dragSelectingText) {
        if ((viewportPos - m_selectionAnchorViewportPos).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragSelectingText = true;
            if (m_textSelectionBand)
                m_textSelectionBand->show();
            updateHoverCursor(viewPos, buttons);
        }
    }

    if (m_dragSelectingText && m_textSelectionBand) {
        m_textSelectionBand->setGeometry(QRect(m_selectionAnchorViewportPos, viewportPos).normalized());
        updateHoverCursor(viewPos, buttons);
        return true;
    }

    int pageIndex = -1;
    QPointF pdfPoint;
    if (mapViewPointToPage(viewPos, &pageIndex, &pdfPoint)) {
        FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
        if (page) {
            m_formManager->handleMouseMove(page, pdfPoint);
            FPDF_ClosePage(page);
        }
    }
    return m_mouseSelecting;
}

bool QPDFView::handleMouseRelease(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButton button)
{
    if (handleAnnotationMouseRelease(viewPos, viewportPos, button))
        return true;

    if (!m_document || button != Qt::LeftButton || !m_mouseSelecting)
        return false;

    const bool dragSelection = m_dragSelectingText;
    m_mouseSelecting = false;
    m_dragSelectingText = false;

    if (dragSelection && m_textSelectionBand) {
        const QRect selectionRectInViewport = m_textSelectionBand->geometry().normalized();
        m_textSelectionBand->hide();

        qreal scaleRatio = 1.0;
        const QList<PageViewportInfo> pages = currentViewportPages(scaleRatio);
        for (const PageViewportInfo& pageInfo : pages) {
            const QRectF pageRectInViewport(
                m_scrollArea->viewport()->mapFrom(this, pageInfo.rect.topLeft().toPoint()),
                pageInfo.rect.size());
            const QRectF intersected = pageRectInViewport.intersected(selectionRectInViewport);
            if (!intersected.isValid() || intersected.isEmpty())
                continue;

            const QRectF localRect = QRectF(intersected.topLeft() - pageRectInViewport.topLeft(), intersected.size());
            const PdfRect pdfRect = mapImageRectToPdfRect(localRect, pageInfo.pageIndex, scaleRatio);
            if (!pdfRect.isValid())
                continue;

            FPDF_PAGE page = FPDF_LoadPage(m_document, pageInfo.pageIndex);
            if (!page)
                continue;

            FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
            if (!textPage) {
                FPDF_ClosePage(page);
                continue;
            }

            const int textLength = FPDFText_GetBoundedText(textPage,
                                                           pdfRect.left,
                                                           pdfRect.top,
                                                           pdfRect.right,
                                                           pdfRect.bottom,
                                                           nullptr,
                                                           0);
            if (textLength > 0) {
                QVector<unsigned short> buffer(textLength + 1, 0);
                FPDFText_GetBoundedText(textPage,
                                        pdfRect.left,
                                        pdfRect.top,
                                        pdfRect.right,
                                        pdfRect.bottom,
                                        buffer.data(),
                                        static_cast<int>(buffer.size()));
                m_selectedText = QString::fromUtf16(buffer.constData()).trimmed();
                m_selectedTextPage = pageInfo.pageIndex;
                m_selectedTextBounds = pdfRect;
                m_selectedImage = {};
                FPDFText_ClosePage(textPage);
                FPDF_ClosePage(page);
                m_pageCache.clear();
                renderCurrentPage();
                updateCopyAvailability();
                updateHoverCursor(viewPos, Qt::NoButton);
                return true;
            }

            FPDFText_ClosePage(textPage);
            FPDF_ClosePage(page);
        }

        clearSelectionState();
        m_pageCache.clear();
        renderCurrentPage();
        updateCopyAvailability();
        updateHoverCursor(viewPos, Qt::NoButton);
        return true;
    }

    if (selectImageAtViewPoint(viewPos)) {
        m_pageCache.clear();
        renderCurrentPage();
        updateCopyAvailability();
        updateHoverCursor(viewPos, Qt::NoButton);
        return true;
    }

    int pageIndex = -1;
    QPointF pdfPoint;
    if (mapViewPointToPage(viewPos, &pageIndex, &pdfPoint)) {
        FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
        if (page) {
            m_formManager->handleMouseRelease(page, pdfPoint);
            FPDF_ClosePage(page);
        }
    }

    clearSelectionState();
    m_pageCache.clear();
    renderCurrentPage();
    updateCopyAvailability();
    updateHoverCursor(viewPos, Qt::NoButton);
    return true;
}

void QPDFView::mousePressEvent(QMouseEvent* event)
{
    if (!handleMousePress(event->pos(), m_scrollArea ? m_scrollArea->viewport()->mapFrom(this, event->pos()) : event->pos(),
                          event->button(), event->modifiers()))
        QBaseView::mousePressEvent(event);
}

void QPDFView::mouseReleaseEvent(QMouseEvent* event)
{
    if (!handleMouseRelease(event->pos(), m_scrollArea ? m_scrollArea->viewport()->mapFrom(this, event->pos()) : event->pos(),
                            event->button()))
        QBaseView::mouseReleaseEvent(event);
}

void QPDFView::mouseMoveEvent(QMouseEvent* event)
{
    if (!handleMouseMove(event->pos(), m_scrollArea ? m_scrollArea->viewport()->mapFrom(this, event->pos()) : event->pos(),
                         event->buttons()))
        QBaseView::mouseMoveEvent(event);
}

void QPDFView::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::Copy)) {
        if (copySelection()) {
            event->accept();
            return;
        }
    }

    if (m_document && m_formManager->handle()) {
        FPDF_PAGE page = FPDF_LoadPage(m_document, m_currentPage);
        if (page) {
            const bool handled = !event->text().isEmpty()
                ? m_formManager->handleChar(page, event->text().at(0))
                : m_formManager->handleKeyDown(page, event->key(), event->modifiers());
            FPDF_ClosePage(page);
            if (handled) { renderCurrentPage(); event->accept(); return; }
        }
    }

    if (handleNavigationKey(event)) return;
    QBaseView::keyPressEvent(event);
}

bool QPDFView::handleNavigationKey(QKeyEvent* event)
{
    const auto modifiers=event->modifiers() & ~Qt::KeypadModifier;
    if (!m_document || (modifiers != Qt::NoModifier
        && !(modifiers == Qt::ControlModifier
            && (event->key() == Qt::Key_Home || event->key() == Qt::Key_End)))) return false;
    switch (event->key()) {
        case Qt::Key_PageDown: case Qt::Key_PageUp: case Qt::Key_Home: case Qt::Key_End:
        case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right: break;
        default: return false;
    }
    event->accept();
    if (event->type() == QEvent::ShortcutOverride) return true;
    switch (event->key()) {
        case Qt::Key_PageDown: nextPage(); break;
        case Qt::Key_PageUp:   previousPage(); break;
        case Qt::Key_Home:     goToPage(0); break;
        case Qt::Key_End:      goToPage(m_pageCount - 1); break;
        case Qt::Key_Up: m_scrollArea->verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub); break;
        case Qt::Key_Down: m_scrollArea->verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepAdd); break;
        case Qt::Key_Left: m_scrollArea->horizontalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub); break;
        case Qt::Key_Right: m_scrollArea->horizontalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepAdd); break;
    }
    return true;
}

QList<QRectF> QPDFView::visiblePageRects(qreal& scaleRatio) const
{
    QList<QRectF> rects;
    const QList<PageViewportInfo> pages = currentViewportPages(scaleRatio);
    for (const PageViewportInfo& page : pages)
        rects.append(page.rect);
    return rects;
}


void QPDFView::renderCurrentPage()
{
    if(renderPending_)return;renderPending_=true;
    // PDFium handles remain on their owner thread. Coalesce resize/navigation
    // requests instead of sharing non-thread-safe handles with worker threads.
    QTimer::singleShot(0,this,[this]{renderPending_=false;renderCurrentPageNow();});
}
