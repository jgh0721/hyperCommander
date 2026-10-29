#pragma once

#include <QWidget>
#include <QPointer>
#include <QToolBar>
#include <QPrinter>
#include <QPainter>
#include <QEventLoop>

#include <memory>

class QBaseView : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)

public:
    struct PrintPageRange
    {
        bool enabled = false;
        int  firstPage = 1;
        int  lastPage = 1;

        bool isValid() const { return enabled && firstPage > 0 && lastPage >= firstPage; }
    };

    // ── 테마 ──
    enum Theme { Light, Dark };
    Q_ENUM(Theme)

    explicit QBaseView(QWidget* parent = nullptr);
    ~QBaseView() override;

    // ── 파일 관리 (순수 가상) ──
    virtual bool            openFile(const QString& filePath)   = 0;
    virtual bool            saveFile(const QString& filePath = {}) = 0;
    virtual void            closeFile()                         = 0;
    virtual bool            isModified() const                  = 0;
    virtual QStringList     supportedExtensions() const         = 0;
    virtual bool            opensFileAsynchronously() const;
    virtual bool            canCancelLoading() const;

    QString currentFilePath() const;
    QString title() const;

    // ── 인쇄 ──
    virtual void print(QPrinter* printer);
    virtual void print(QPrinter* printer, const PrintPageRange& range);
    virtual void printPreview();
    int     printablePageCount() const;
    void    setPrinterBlacklist(const QStringList& names);
    bool    isPrinterAllowed(const QString& name) const;
    static PrintPageRange pageRangeFromPrinter(const QPrinter* printer, int totalPages);

    void RestrictClipboard(bool Restricted) { clipboardRestricted_=Restricted; }
    bool ClipboardRestricted() const {return clipboardRestricted_;}
    // ── 복사 ──
    virtual bool canCopy() const;
    virtual bool copySelection();

    bool isShuttingDown() const;
    bool isLoading() const;

    // ── 도구모음 ──
    virtual QToolBar* createToolBar();
    virtual QToolBar* createAuxiliaryToolBar();
    void setToolBarVisible(bool visible);
    bool isToolBarVisible() const;

    // ── 탭 ──
    void setTabEnabled(bool on);
    bool isTabEnabled() const;

    // ── 테마 ──
    virtual void setTheme(Theme theme);
    Theme currentTheme() const;

signals:
    void ClipboardRequested(const QString& Mode);
    void SaveRequested();
    void OpenRequested(const QString& Path);
    void fileOpened(const QString& path);
    void fileOpenFailed(const QString& path, const QString& errorMessage);
    void fileClosed();
    void modifiedChanged(bool modified);
    void titleChanged(const QString& title);
    void copyAvailabilityChanged(bool available);
    void loadingStateChanged(bool active, const QString& message, int value, int maximum);

protected:
    // 서브클래스 필수 구현 (인쇄/미리보기용)
    virtual void renderPage(QPainter* painter, int pageIndex, const QRectF& rect) = 0;
    virtual int  pageCount() const = 0;


    void updateCopyAvailability();
    void beginLoading(const QString& message, int maximum = 0);
    void updateLoadingProgress(const QString& message,
                               int value = -1,
                               int maximum = -1,
                               QEventLoop::ProcessEventsFlags flags = QEventLoop::ExcludeUserInputEvents);
    void endLoading();

    void applyThemeStyleSheet(Theme theme);
    void setDisplayTitle(const QString& title);

    // 데이터
    QString         m_filePath;
    QString         m_displayTitle;
    QStringList     m_printerBlacklist;
    QPointer<QToolBar> m_toolBar    = nullptr;
    QPointer<QToolBar> m_auxiliaryToolBar = nullptr;
    Theme           m_theme         = Light;
    bool            m_toolBarVisible= true;
    bool            m_tabEnabled    = true;
    bool            m_lastCopyAvailability = false;
    bool            m_loadingActive = false;
    QString         m_loadingMessage;
    int             m_loadingValue = 0;
    int             m_loadingMaximum = 0;
    bool            m_shuttingDown  = false;
    bool clipboardRestricted_ = false;
};
