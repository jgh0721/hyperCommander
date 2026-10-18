#pragma once

#include "core/QBaseView.h"
#include "pdf/PDFAnnotationManager.h"
#include "pdf/PDFFormManager.h"

#include <QFont>
#include <QJsonObject>
#include <QScrollArea>
#include <QLabel>
#include <QSplitter>
#include <QListWidget>
#include <QTreeWidget>
#include <QTabWidget>
#include <QLineEdit>
#include <QCache>
#include <QRubberBand>
#include <QVector>

#include <atomic>

#include <fpdfview.h>
#include <fpdf_formfill.h>

class QPropertyAnimation;
class QTimer;

class QPDFView : public QBaseView
{
    Q_OBJECT

public:
    bool IsRendering() const {return renderPending_;}
    bool LoadContent(const QByteArray& Bytes, const QString& Path);
    QByteArray SerializeContent() const;
    int PageRotation(int Page) const;
    bool AddAnnotation(int Page, const QString& Kind, const QRectF& Rect, const QString& Text = {});
    void CommitContent();
    void ShowAnnotationToolBar(bool Enabled) { setAnnotationToolBarVisible(Enabled); }
    enum ZoomMode   { FitWidth, FitHeight, FitPage, CustomZoom };
    enum ViewMode   { SinglePage, ContinuousScroll, TwoPageSpread };
    enum SidebarPos { SidebarLeft, SidebarRight, SidebarHidden };
    enum SpreadReadingDirection { LeftToRight, RightToLeft };
    enum RotationAngle { Rotate0 = 0, Rotate90 = 90, Rotate180 = 180, Rotate270 = 270 };
    enum AnnotationTool { AnnotationNone, AnnotationHighlight, AnnotationInk, AnnotationFreeText, AnnotationUnderline, AnnotationStrikeOut, AnnotationNote };

    Q_ENUM(ZoomMode)
    Q_ENUM(ViewMode)
    Q_ENUM(SidebarPos)
    Q_ENUM(SpreadReadingDirection)
    Q_ENUM(RotationAngle)
    Q_ENUM(AnnotationTool)

    explicit QPDFView(QWidget* parent = nullptr);
    ~QPDFView() override;

    // ── QBaseView ──
    bool        openFile(const QString& filePath) override;
    bool        saveFile(const QString& filePath) override;
    void        closeFile() override;
    bool        isModified() const override;
    QStringList supportedExtensions() const override;
    bool        opensFileAsynchronously() const override;
    QList<QRectF> visiblePageRects(qreal& scaleRatio) const override;
    void        setTheme(Theme theme) override;

    // ── 네비게이션 ──
    int  currentPage() const;
    int  totalPageCount() const;
    void goToPage(int page);
    void nextPage();
    void previousPage();

    // ── 줌 ──
    void  setZoomMode(ZoomMode mode);
    void  setZoomLevel(qreal percentage);
    ZoomMode zoomMode() const;
    qreal zoomLevel() const;

    // ── 사이드바 ──
    void       setSidebarPosition(SidebarPos pos);
    SidebarPos sidebarPosition() const;

    // ── 뷰 모드 ──
    void     setViewMode(ViewMode mode);
    ViewMode viewMode() const;
    void     setSpreadReadingDirection(SpreadReadingDirection direction);
    SpreadReadingDirection spreadReadingDirection() const;
    void     setRotationAngle(RotationAngle angle);
    RotationAngle rotationAngle() const;
    bool     saveFileAs();

    // ── 검색 ──
    void searchText(const QString& text);
    void searchNext();
    void searchPrev();
    bool hasSearchResults() const;
    int currentSearchResultIndex() const;
    int totalSearchResultCount() const;
    bool hasTextSelection() const;
    int selectedTextLength() const;
    QJsonObject ClipboardContent() const;
    bool hasImageSelection() const;
    bool canCopy() const override;
    bool copySelection() override;

signals:
    void ContentChanged();
    void pageChanged(int page, int total);
    void viewStateChanged();

protected:
    void renderPage(QPainter* painter, int pageIndex, const QRectF& rect) override;
    int  pageCount() const override;
    QToolBar* createToolBar() override;
    QToolBar* createAuxiliaryToolBar() override;

    bool eventFilter(QObject* watched, QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct PdfRect {
        qreal left = 0.0;
        qreal right = 0.0;
        qreal bottom = 0.0;
        qreal top = 0.0;

        bool isValid() const { return right > left && top > bottom; }
    };

    struct SearchResult {
        int pageIndex = -1;
        int startIndex = -1;
        int count = 0;
        QVector<PdfRect> rects;

        bool isValid() const { return pageIndex >= 0 && startIndex >= 0 && count > 0; }
    };

    struct SelectedImageInfo {
        int pageIndex = -1;
        PdfRect bounds;
        QImage image;

        bool isValid() const { return pageIndex >= 0 && bounds.isValid() && !image.isNull(); }
    };

    struct PageViewportInfo {
        int pageIndex = -1;
        QRectF rect;
    };

    struct TextHitInfo {
        bool hit = false;
        int pageIndex = -1;
        int charIndex = -1;
        PdfRect bounds;
    };

    void setupUI();
    void renderCurrentPage();
    void renderCurrentPageNow();
    bool renderPending_ = false;
    void updateSidebar();
    void startThumbnailGeneration();
    void generateNextThumbnail();
    void loadBookmarks();
    void loadBookmarkChildren(FPDF_BOOKMARK parent, QTreeWidgetItem* parentItem);
    QImage renderPageToImage(int pageIndex, qreal scale);
    QSizeF pageSize(int pageIndex) const;
    QSizeF rotatedPageSize(int pageIndex) const;
    QImage renderSpreadImage(int firstPageIndex, qreal scale);
    QImage renderContinuousImage(qreal scale);
    QList<PageViewportInfo> currentViewportPages(qreal& scaleRatio) const;
    QPair<int, int> spreadPagePair(int firstPageIndex) const;
    QRectF mapPdfRectToImageRect(const PdfRect& rect, int pageIndex, qreal scale) const;
    QPointF mapPdfPointToImagePoint(const QPointF& pdfPoint, int pageIndex, qreal scale) const;
    QPointF mapImagePointToPdfPoint(const QPointF& imagePoint, int pageIndex, qreal scale) const;
    PdfRect mapImageRectToPdfRect(const QRectF& imageRect, int pageIndex, qreal scale) const;
    bool mapViewPointToPage(const QPoint& viewPos, int* pageIndex, QPointF* pdfPoint = nullptr) const;
    QVector<SearchResult> findSearchResultsOnPage(int pageIndex) const;
    QVector<SearchResult> findAllSearchResults() const;
    int searchResultIndexFor(const QVector<SearchResult>& results, int pageIndex, int startIndex) const;
    void applySearchResultSelection(const QVector<SearchResult>& results, int resultIndex);
    void updateSearchHighlights(int pageIndex, int activeStartIndex);
    void clearSearchHighlights();
    void revealSearchResult(const SearchResult& result);
    void animateScrollToRect(const QRectF& targetRectInLabel);
    void animateScrollBarTo(QScrollBar* scrollBar, QPropertyAnimation* animation, int targetValue);
    void closeFileInternal(bool emitSignals);
    void teardownUiForDestruction();
    void clearSelectionState();
    TextHitInfo textHitTestAtViewPoint(const QPoint& viewPos) const;
    bool hasTextAtViewPoint(const QPoint& viewPos) const;
    void updateHoverCursor(const QPoint& viewPos, Qt::MouseButtons buttons = Qt::NoButton);
    bool selectImageAtViewPoint(const QPoint& viewPos);
    SelectedImageInfo extractTopmostImageAtPoint(int pageIndex, const QPointF& pdfPoint) const;
    static QImage imageFromPdfBitmap(FPDF_BITMAP bitmap);
    bool handleMousePress(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    bool handleMouseMove(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButtons buttons);
    bool handleMouseRelease(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButton button);
    void setModifiedState(bool modified);
    void setAnnotationTool(AnnotationTool tool);
    void setAnnotationToolBarVisible(bool visible);
    bool handleAnnotationMousePress(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButton button);
    bool handleAnnotationMouseMove(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButtons buttons);
    bool handleAnnotationMouseRelease(const QPoint& viewPos, const QPoint& viewportPos, Qt::MouseButton button);
    bool addMarkupAnnotationFromRect(int pageIndex, const PdfRect& rect);
    bool addFreeTextAnnotationFromRect(int pageIndex, const PdfRect& rect);
    bool addNoteAnnotationAt(int pageIndex, const QPointF& pdfPoint);
    bool commitAnnotationChange(bool success);
    PDFAnnotationManager::PdfRect toAnnotationRect(const PdfRect& rect) const;

    // PDFium
    FPDF_DOCUMENT       m_document      = nullptr;
    FPDF_FORMHANDLE     m_formHandle    = nullptr;
    PDFFormManager*     m_formManager   = nullptr;
    PDFAnnotationManager m_annotationManager;

    // UI
    QSplitter*          m_splitter      = nullptr;
    QTabWidget*         m_sidebarTabs   = nullptr;
    QListWidget*        m_thumbList     = nullptr;
    QTreeWidget*        m_bookmarkTree  = nullptr;
    QScrollArea*        m_scrollArea    = nullptr;
    QLabel*             m_pageLabel     = nullptr;
    QRubberBand*        m_textSelectionBand = nullptr;
    QPointer<QToolBar>  m_annotationToolBar = nullptr;
    QPropertyAnimation* m_horizontalScrollAnimation = nullptr;
    QPropertyAnimation* m_verticalScrollAnimation = nullptr;
    QTimer*             m_thumbnailTimer = nullptr;

    // 상태
    int         m_currentPage   = 0;
    int         m_pageCount     = 0;
    qreal       m_zoomLevel     = 100.0;
    ZoomMode    m_zoomMode      = FitPage;
    ViewMode    m_viewMode      = SinglePage;
    SpreadReadingDirection m_spreadReadingDirection = LeftToRight;
    RotationAngle m_rotationAngle = Rotate0;
    SidebarPos  m_sidebarPos    = SidebarLeft;
    bool        m_modified      = false;
    bool        m_annotationToolBarVisible = false;
    AnnotationTool m_annotationTool = AnnotationNone;
    QColor      m_annotationColor = QColor(255, 235, 59, 128);
    qreal       m_annotationThickness = 8.0;
    QFont       m_annotationFont;
    qreal       m_annotationFontSize = 14.0;

    // 검색
    QString     m_searchText;
    int         m_searchPage    = 0;
    int         m_searchMatchStart = -1;
    int         m_searchMatchCount = 0;
    int         m_searchResultIndex = -1;
    int         m_searchResultTotal = 0;
    QVector<SearchResult> m_searchHighlights;
    int         m_wheelRemainder = 0;

    bool        m_mouseSelecting = false;
    bool        m_annotationDragging = false;
    int         m_annotationAnchorPage = -1;
    QPoint      m_annotationAnchorViewportPos;
    QPointF     m_annotationAnchorPdfPoint;
    QVector<QPointF> m_annotationInkPoints;
    bool        m_dragSelectingText = false;
    int         m_selectionAnchorPage = -1;
    QPoint      m_selectionAnchorViewPos;
    QPoint      m_selectionAnchorViewportPos;
    QString     m_selectedText;
    int         m_selectedTextPage = -1;
    PdfRect     m_selectedTextBounds;
    SelectedImageInfo m_selectedImage;

    QCache<int, QImage> m_pageCache{20};
    QByteArray  m_fileData;     // FPDF_LoadMemDocument 수명 보장용
    std::atomic_int m_openRequestId{0};
    int         m_nextThumbnailPage = 0;
    bool        m_closeFileInProgress = false;
};
