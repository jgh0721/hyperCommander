#pragma once

#include "core/QBaseView.h"

#include <QImage>
#include <QJsonObject>
#include "UniqueLibs/document/imageResampler.hpp"
#include <memory>
#include <stop_token>
#include <atomic>
#include <QUndoStack>
#include <QTimer>
#include <QScrollArea>
#include <QLabel>
#include <QRubberBand>

class QImageView : public QBaseView
{
    Q_OBJECT
    std::shared_ptr<QThreadPool> imagePool_ = hc::document::ImageWorkerPool();

public:
    void LoadContent(const QImage& Image, const QString& Path, bool Traverse);
    void SetNavigationSource(const QString& Path);
    void DisplayAnimationFrame(const QImage& Image);
    QImage Content() const { return m_displayImage; }
    QByteArray SerializeContent(const QByteArray& Format = "PNG", int Quality = -1) const;
    void CommitContent();
    bool IsEditing() const { return pendingEdit_; }
    bool IsRendering() const { return rendering_ || previewing_ || pendingEdit_; }
    void CancelWork() { renderStop_->request_stop();previewStop_->request_stop();editStop_->request_stop(); }
    QString RenderError() const { return renderError_; }
    void SetDisplayFilter(const QString& Filter);
    QString DisplayFilter() const { return filter_; }
    bool ApplyCrop(const QRect& Rect);
    bool ApplyResize(const QSize& Size, const QString& Filter);
    bool ApplyComposite(const QImage& Image, const QRectF& Rect, double Angle);
    bool CanUndo() const { return m_undoStack->canUndo(); }
    bool CanRedo() const { return m_undoStack->canRedo(); }
    enum FitMode   { OriginalSize, FitToWindow, FitWidth, FitHeight, StretchToWindow };
    enum EditTool  { None, Crop, FreeDraw, Rectangle, Ellipse, Arrow, TextTool };
    Q_ENUM(FitMode)
    Q_ENUM(EditTool)

    explicit QImageView(QWidget* parent = nullptr);
    ~QImageView() override;

    // ── QBaseView ──
    bool        openImage(const QImage& image, const QString& title = {});
    bool        openBlankCanvas(const QSize& size = QSize(800, 600),
                                const QColor& fillColor = Qt::white,
                                const QString& title = {});
    bool        openFile(const QString& filePath) override;
    bool        saveFile(const QString& filePath) override;
    void        closeFile() override;
    bool        isModified() const override;
    QStringList supportedExtensions() const override;
    bool        opensFileAsynchronously() const override;
    void        setTheme(Theme theme) override;

    // ── 줌/맞춤 ──
    void    setFitMode(FitMode mode);
    FitMode fitMode() const;
    void    setZoomLevel(qreal pct);
    qreal   zoomLevel() const;

    // ── 변환 ──
    void rotateClockwise();
    void rotateCounterClockwise();
    void flipHorizontal();
    void flipVertical();

    // ── 편집 도구 ──
    void     setEditTool(EditTool tool);
    EditTool currentEditTool() const;
    bool     canCopy() const override;
    bool     copySelection() override;
    QJsonObject ClipboardContent() const;
    void     setPenColor(const QColor& color);
    void     setPenWidth(int width);
    void     setPenStyle(Qt::PenStyle style);
    Qt::PenStyle penStyle() const;
    void     setTextPixelSize(int pixelSize);
    int      textPixelSize() const;
    void     undoEdit();
    void     redoEdit();
    void     cropToSelection();

    // ── 디렉토리 / 슬라이드쇼 ──
    void openDirectory(const QString& dirPath);
    void startSlideshow(int intervalMs = 3000);
    void stopSlideshow();
    void nextImage();
    void previousImage();
    // Folder stepping for the workspace shortcut router; false while composing or editing.
    bool StepImage(int Direction);
    QSize originalImageSize() const;
    qreal displayScalePercent() const;
    QString imageFormatName() const;
    QString imageColorDescription() const;
    QPointF imageDpi() const;
    int  imageIndexInDirectory() const;
    int  imageCountInDirectory() const;

    bool saveAs(const QString& filePath, const char* format = nullptr, int quality = -1);
    bool saveFileAs();

    // ── 합성 (Composite) ──
    bool startComposite(const QImage& image);

signals:
    void ContentChanged();
    void imageChanged();
    void imagePresented(bool Final);
    void slideshowStateChanged(bool running);
    void viewStateChanged();

protected:
    void renderPage(QPainter* painter, int pageIndex, const QRectF& rect) override;
    int  pageCount() const override;
    QToolBar* createToolBar() override;

    bool eventFilter(QObject* obj, QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    hc::document::ImageRenderRequest renderRequest() const;
    bool startImageEdit(hc::document::ImageRenderRequest Request,const QString& Label);
    bool rendering_ = false, previewing_ = false, pendingEdit_ = false, displayFinal_ = false;
    QString previewKey_, previewRunningKey_, renderRunningKey_;
    QImage previewImage_;
    std::shared_ptr<std::stop_source> previewStop_ = std::make_shared<std::stop_source>();
    std::shared_ptr<std::stop_source> editStop_ = std::make_shared<std::stop_source>();
    QString renderError_;
    std::shared_ptr<std::stop_source> renderStop_ = std::make_shared<std::stop_source>();
    QString filter_ = "auto";
    mutable QString renderedKey_;
    mutable QImage renderedImage_;
    std::uint64_t contentRevision_ = 0, navigationGeneration_ = 0;
    bool directoryLoading_ = false;
    int pendingDirectorySteps_ = 0;
    std::shared_ptr<std::stop_source> directoryStop_ = std::make_shared<std::stop_source>();
    void pushUndoImage();       // 현재 상태를 Undo 스택에 저장
    void setModifiedState(bool modified);
    void updateDisplay();
    QImage scaledImage() const;
    QPoint toImageCoord(const QPoint& widgetPos) const;
    QPoint toWidgetCoord(const QPoint& imagePos) const;
    QPointF toWidgetCoord(const QPointF& imagePos) const;
    QPointF toImageCoord(const QPointF& widgetPos) const;

    // ── 합성 관련 상태 및 처리 ──
    bool m_compositeMode = false;
    QImage m_compositeImage;
    QRectF m_compositeRect;
    double m_compositeRotation = 0.0;

    enum CompositeHandle {
        CompHandleNone, CompHandleMove,
        CompHandleResizeTL, CompHandleResizeTR, CompHandleResizeBL, CompHandleResizeBR,
        CompHandleRotate, CompHandleAccept, CompHandleCancel
    };
    CompositeHandle m_activeCompositeHandle = CompHandleNone;
    QPoint m_compositeDragStartPos;
    QRectF m_compositeDragStartRect;
    double m_compositeDragStartRotation = 0.0;

    void paintCompositeOverlay(QPainter& painter, const QImage& baseImage);
    bool handleCompositeEvent(QEvent* event);
    void finishComposite(bool accept);
    CompositeHandle hitTestCompositeHandles(const QPoint& widgetPos);

    QImage          m_originalImage;
    QImage          m_displayImage;
    QUndoStack*     m_undoStack     = nullptr;

    QScrollArea*    m_scrollArea    = nullptr;
    bool            fitUpdatePending_ = false;
    QLabel*         m_imageLabel    = nullptr;
    QRubberBand*    m_rubberBand    = nullptr;      // Crop 선택 영역

    QStringList     m_dirFiles;
    int             m_dirIndex      = 0;
    QTimer*         m_slideshowTimer= nullptr;
    QString         m_imageFormat;
    std::atomic_int m_openRequestId{0};

    FitMode     m_fitMode   = FitToWindow;
    qreal       m_zoomLevel = 100.0;
    EditTool    m_editTool  = None;
    QColor      m_penColor  = Qt::red;
    int         m_penWidth  = 2;
    Qt::PenStyle m_penStyle = Qt::SolidLine;
    int         m_textPixelSize = 24;
    bool        m_modified  = false;

    QPoint      m_dragStart;        // 그리기/자르기 시작점 (imageLabel 좌표)
    QPoint      m_lastDrawPoint;
    bool        m_drawing = false;
    bool        m_updatingDisplay = false;

    void updateOverlay(const QPoint& curLabelPos);  // 드래그 중 미리보기
};
