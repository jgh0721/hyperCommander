#include "core/QBaseView.h"

#include "core/ThemeManager.h"

#include <QCoreApplication>
#include <QApplication>
#include <QPrintPreviewDialog>
#include <QPrinterInfo>
#include <QFileInfo>
#include <QMetaObject>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QtMath>

// ── 오버레이 위젯 ──
class WatermarkOverlay : public QWidget {
public:
    explicit WatermarkOverlay(QBaseView* parent) : QWidget(parent), m_view(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        if (!m_view || m_view->isShuttingDown()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        qreal scaleRatio = 1.0;
        QList<QRectF> rects = m_view->visiblePageRects(scaleRatio);
        for (const auto& r : rects) {
            m_view->renderWatermark(&painter, r, scaleRatio);
        }
    }
private:
    QBaseView* m_view;
};

// ═══════════════════════════════════════════════════════════
// 생성 / 소멸
// ═══════════════════════════════════════════════════════════
QBaseView::QBaseView(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_watermarkOverlay = new WatermarkOverlay(this);
}

QBaseView::~QBaseView()
{
    m_shuttingDown = true;
    for(auto* Child:findChildren<QObject*>())QObject::disconnect(Child,nullptr,this,nullptr);

    // 대기 중인 QueuedConnection 호출(예: setWatermark 람다)이
    // 소멸 중에 실행되지 않도록 게시된 이벤트를 제거
    if (QCoreApplication::instance())
        QCoreApplication::removePostedEvents(this);

    // m_watermarkOverlay는 this의 자식 위젯이므로
    // QWidget::~QWidget()이 자동으로 삭제합니다.
    // 소멸자에서 접근하지 않음 - QPointer 내부 weakref가 이미 무효화된
    // 상태에서 접근 시 예외 발생 가능.
}

// ═══════════════════════════════════════════════════════════
// 파일 정보
// ═══════════════════════════════════════════════════════════
QString QBaseView::currentFilePath() const { return m_filePath; }

bool QBaseView::opensFileAsynchronously() const
{
    return false;
}

bool QBaseView::canCancelLoading() const
{
    return opensFileAsynchronously() && isLoading();
}

QString QBaseView::title() const
{
    if (!m_displayTitle.isEmpty())
        return m_displayTitle;

    if (m_filePath.isEmpty())
        return tr("제목없음");
    return QFileInfo(m_filePath).fileName();
}

void QBaseView::setDisplayTitle(const QString& title)
{
    m_displayTitle = title;
}

// ═══════════════════════════════════════════════════════════
// 인쇄
// ═══════════════════════════════════════════════════════════
void QBaseView::print(QPrinter* printer)
{
    print(printer, pageRangeFromPrinter(printer, pageCount()));
}

void QBaseView::print(QPrinter* printer, const PrintPageRange& range)
{
    if (!printer) return;

    QPainter painter(printer);
    const QRectF pageRect = printer->pageLayout().paintRectPixels(printer->resolution());

    const int totalPages = pageCount();
    const int firstPageIndex = range.isValid()
        ? qBound(0, range.firstPage - 1, qMax(0, totalPages - 1))
        : 0;
    const int lastPageIndex = range.isValid()
        ? qBound(firstPageIndex, range.lastPage - 1, qMax(0, totalPages - 1))
        : qMax(0, totalPages - 1);

    const int pagesToPrint = lastPageIndex - firstPageIndex + 1;
    const bool showProgress = pagesToPrint > 1;

    if (showProgress)
        beginLoading(tr("인쇄 미리보기 준비 중..."), pagesToPrint);

    for (int i = firstPageIndex; i <= lastPageIndex; ++i) {
        if (i > firstPageIndex) printer->newPage();
        renderPage(&painter, i, pageRect);
        renderWatermark(&painter, pageRect);

        if (showProgress) {
            updateLoadingProgress({}, i - firstPageIndex + 1, pagesToPrint);
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }
    painter.end();

    if (showProgress)
        endLoading();
}

void QBaseView::printPreview()
{
    QPrinter printer(QPrinter::HighResolution);
    QPrintPreviewDialog dlg(&printer, this);

    connect(&dlg, &QPrintPreviewDialog::paintRequested,
            this, [this](QPrinter* p) { print(p); });

    dlg.exec();
}

int QBaseView::printablePageCount() const
{
    return pageCount();
}

void QBaseView::setPrinterBlacklist(const QStringList& names)
{
    m_printerBlacklist = names;
}

bool QBaseView::isPrinterAllowed(const QString& name) const
{
    for (const auto& blocked : m_printerBlacklist) {
        if (name.contains(blocked, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

QBaseView::PrintPageRange QBaseView::pageRangeFromPrinter(const QPrinter* printer, int totalPages)
{
    PrintPageRange range;
    if (!printer || totalPages <= 0)
        return range;

    if (printer->printRange() != QPrinter::PageRange)
        return range;

    const int fromPage = qBound(1, printer->fromPage(), totalPages);
    const int toPage = qBound(fromPage, printer->toPage(), totalPages);
    range.enabled = true;
    range.firstPage = fromPage;
    range.lastPage = toPage;
    return range;
}

bool QBaseView::canCopy() const
{
    return false;
}

bool QBaseView::copySelection()
{
    return false;
}

void QBaseView::beginLoading(const QString& message, int maximum)
{
    m_loadingActive = true;
    m_loadingMessage = message;
    m_loadingValue = 0;
    m_loadingMaximum = qMax(0, maximum);
    emit loadingStateChanged(true, m_loadingMessage, m_loadingValue, m_loadingMaximum);
}

void QBaseView::updateLoadingProgress(const QString& message,
                                      int value,
                                      int maximum,
                                      QEventLoop::ProcessEventsFlags flags)
{
    m_loadingMessage=message; if(value>=0)m_loadingValue=value;if(maximum>=0)m_loadingMaximum=maximum; Q_UNUSED(flags); emit loadingStateChanged(m_loadingActive,m_loadingMessage,m_loadingValue,m_loadingMaximum);
}

void QBaseView::endLoading()
{
    if (!m_loadingActive && m_loadingMessage.isEmpty() && m_loadingMaximum == 0)
        return;

    m_loadingActive = false;
    m_loadingMessage.clear();
    m_loadingValue = 0;
    m_loadingMaximum = 0;
    emit loadingStateChanged(false, {}, 0, 0);
}

bool QBaseView::isLoading() const
{
    return m_loadingActive;
}

void QBaseView::updateCopyAvailability()
{
    const bool available = canCopy();
    if (m_lastCopyAvailability == available)
        return;

    m_lastCopyAvailability = available;
    emit copyAvailabilityChanged(available);
}

// ═══════════════════════════════════════════════════════════
// 워터마크
// ═══════════════════════════════════════════════════════════
void QBaseView::setWatermark(const WatermarkConfig& cfg)
{
    if (m_shuttingDown) return;
    // 강제 모드에서는 사용자가 해제할 수 없음
    if (m_watermark.forced && !cfg.forced && !cfg.enabled) return;
    m_watermark = cfg;

    if (!m_watermarkOverlay) return;

    QMetaObject::invokeMethod(this, [this] {
        if (!m_watermarkOverlay) return;

        m_watermarkOverlay->setGeometry(rect());
        m_watermarkOverlay->setVisible(m_watermark.enabled);

        if (m_watermark.enabled && isVisible()) {
            m_watermarkOverlay->raise();
            m_watermarkOverlay->update();
        }
    }, Qt::QueuedConnection);
}
QBaseView::WatermarkConfig QBaseView::watermark() const   { return m_watermark; }

QList<QRectF> QBaseView::visiblePageRects(qreal& scaleRatio) const {
    scaleRatio = 1.0;
    return { rect() }; // 기본적으로 전체 영역 반환 (텍스트, 마크다운 등 미지원 뷰어 대비용)
}

bool QBaseView::isShuttingDown() const { return m_shuttingDown; }

void QBaseView::renderWatermark(QPainter* painter, const QRectF& pageRect, qreal scaleRatio)
{
    if (m_shuttingDown || !m_watermark.enabled || !painter || !painter->isActive()) return;
    if (!pageRect.isValid() || pageRect.isEmpty()) return;
    if (!qIsFinite(scaleRatio) || scaleRatio <= 0.0) scaleRatio = 1.0;

    painter->save();
    painter->setOpacity(m_watermark.opacity);

    const QPointF center(
        pageRect.x() + pageRect.width()  * m_watermark.relativePos.x(),
        pageRect.y() + pageRect.height() * m_watermark.relativePos.y()
    );

    painter->translate(center);
    painter->rotate(m_watermark.rotation);

    if (!m_watermark.image.isNull()) {
        // 이미지 워터마크
        const QSizeF sz(
            pageRect.width()  * m_watermark.relativeSize.width(),
            pageRect.height() * m_watermark.relativeSize.height()
        );
        if (sz.isEmpty() || sz.width() <= 0.0 || sz.height() <= 0.0) {
            painter->restore();
            return;
        }
        QRectF dst(-sz.width() / 2, -sz.height() / 2, sz.width(), sz.height());
        painter->drawPixmap(dst.toRect(), m_watermark.image.scaled(sz.toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else if (!m_watermark.text.isEmpty()) {
        // 문자열 워터마크 (바둑판, 여러 줄 배치)
        QFont font = painter->font();
        font.setPixelSize(qMax(1, int(pageRect.height() * m_watermark.relativeSize.height() * 0.1 * scaleRatio)));
        painter->setFont(font);
        painter->setPen(QColor(m_watermark.color.red(), m_watermark.color.green(), m_watermark.color.blue(), int(255 * m_watermark.opacity)));

        QFontMetricsF fm(font);
        QRectF textRect = fm.boundingRect(m_watermark.text);

        // 충분한 여백을 포함하여 패턴 크기 계산
        qreal stepX = textRect.width() * 1.5;
        qreal stepY = textRect.height() * 3.0;

        // 회전 등으로 인해 pageRect보다 더 큰 영역을 채워야 할 수 있음
        qreal maxDim = qMax(pageRect.width(), pageRect.height()) * 3.0;

        int countX = qCeil(maxDim / stepX);
        int countY = qCeil(maxDim / stepY);

        for (int i = -countX; i <= countX; ++i) {
            for (int j = -countY; j <= countY; ++j) {
                // 짝수/홀수 줄에 따라 X를 엇갈리게 배치 (벽돌 패턴)
                qreal offsetX = i * stepX + (j % 2 != 0 ? stepX / 2.0 : 0);
                qreal offsetY = j * stepY;
                painter->drawText(QRectF(offsetX - textRect.width() / 2, offsetY - textRect.height() / 2, textRect.width(), textRect.height()),
                                  Qt::AlignCenter, m_watermark.text);
            }
        }
    }

    painter->restore();
}

// ═══════════════════════════════════════════════════════════
// 화면 캡처 방지
// ═══════════════════════════════════════════════════════════
void QBaseView::setScreenCaptureProtectionMode(ScreenProtect::ProtectionMode mode)
{
    if (m_captureProtectionMode == mode)
        return;

    m_captureProtectionMode = mode;
    emit screenCaptureProtectionChanged(mode);
}

ScreenProtect::ProtectionMode QBaseView::screenCaptureProtectionMode() const
{
    return m_captureProtectionMode;
}

void QBaseView::setScreenCaptureProtection(bool on)
{
    setScreenCaptureProtectionMode(on
        ? ScreenProtect::ProtectionMode::ExcludeFromCapture
        : ScreenProtect::ProtectionMode::Off);
}

bool QBaseView::isScreenCaptureProtected() const
{
    return m_captureProtectionMode != ScreenProtect::ProtectionMode::Off;
}

// ═══════════════════════════════════════════════════════════
// 도구모음
// ═══════════════════════════════════════════════════════════
QToolBar* QBaseView::createToolBar()
{
    // MainWindow에서 탭 전환 시 이전 도구모음은 삭제되므로
    // 매번 새로 생성해야 함 (m_toolBar 캐싱하지 않음)
    auto* tb = new QToolBar(this);
    tb->setMovable(false);
    tb->setIconSize(QSize(20, 20));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->setVisible(m_toolBarVisible);
    m_toolBar = tb;
    connect(tb, &QObject::destroyed, this, [this, tb] {
        if (m_toolBar == tb) m_toolBar = nullptr;
    });
    return tb;
}

QToolBar* QBaseView::createAuxiliaryToolBar()
{
    return nullptr;
}

void QBaseView::setToolBarVisible(bool visible)
{
    m_toolBarVisible = visible;
    if (m_toolBar) m_toolBar->setVisible(visible);
    if (m_auxiliaryToolBar) m_auxiliaryToolBar->setVisible(visible);
}

bool QBaseView::isToolBarVisible() const { return m_toolBarVisible; }

// ═══════════════════════════════════════════════════════════
// 탭
// ═══════════════════════════════════════════════════════════
void QBaseView::setTabEnabled(bool on) { m_tabEnabled = on; }
bool QBaseView::isTabEnabled() const   { return m_tabEnabled; }

// ═══════════════════════════════════════════════════════════
// 테마
// ═══════════════════════════════════════════════════════════
void QBaseView::setTheme(Theme theme)
{
    m_theme = theme;
    applyThemeStyleSheet(theme);
}

QBaseView::Theme QBaseView::currentTheme() const { return m_theme; }

void QBaseView::applyThemeStyleSheet(Theme theme)
{
    Q_UNUSED(theme);setStyleSheet({});setPalette(parentWidget() ? parentWidget()->palette() : QApplication::palette());
}

void QBaseView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_shuttingDown) return;
    if (m_watermarkOverlay) {
        m_watermarkOverlay->resize(size());
    }
}
