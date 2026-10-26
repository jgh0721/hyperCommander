#include "core/QBaseView.h"

#include "core/ThemeManager.h"

#include <QCoreApplication>
#include <QApplication>
#include <QPrintPreviewDialog>
#include <QPrinterInfo>
#include <QFileInfo>
#include <QVBoxLayout>

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

}

QBaseView::~QBaseView()
{
    m_shuttingDown = true;
    for(auto* Child:findChildren<QObject*>())QObject::disconnect(Child,nullptr,this,nullptr);

    // 대기 중인 QueuedConnection 호출이
    // 소멸 중에 실행되지 않도록 게시된 이벤트를 제거
    if (QCoreApplication::instance())
        QCoreApplication::removePostedEvents(this);

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

bool QBaseView::isShuttingDown() const { return m_shuttingDown; }

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
