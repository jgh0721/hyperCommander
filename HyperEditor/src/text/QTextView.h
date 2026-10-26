#pragma once

#include "core/QBaseView.h"
#include "text/ScintillaDocument.h"
#include "text/ScintillaEditorSettings.h"
#include "text/TextFileSession.h"

#include <QPointer>
#include <QVector>
#include <QPair>
#include <atomic>

class ITextEditorBackend;
class FindReplaceWidget;

/// Scintilla 백엔드 기반 텍스트/코드 뷰어
class QTextView : public QBaseView
{
    Q_OBJECT

public:
    void LoadContent(const QString& Text, const QString& Path, const QString& Encoding);
    QString Content() const;
    ITextEditorBackend* Backend() const;
    void ReplaceContent(const QString& Text);
    void CommitContent();
    void ReloadPreferences();
    enum LineEnding { LF, CRLF, CR };
    Q_ENUM(LineEnding)

    explicit QTextView(QWidget* parent = nullptr);
    ~QTextView() override;

    // ── QBaseView ──
    bool        openFile(const QString& filePath) override;
    bool        saveFile(const QString& filePath) override;
    void        closeFile() override;
    bool        isModified() const override;
    QStringList supportedExtensions() const override;
    bool        opensFileAsynchronously() const override;

    // ── 인코딩 ──
    QString       detectedEncoding() const;
    QString       currentEncoding() const;
    QString       currentEncodingDisplayName() const;
    void          reloadWithEncoding(const QString& encoding);
    bool          saveWithEncoding(const QString& filePath, const QString& encoding, bool includeBom);
    bool          saveFileAs();
    static QStringList availableEncodings();

    // ── 글꼴 ──
    void  setEditorFont(const QFont& font);
    QFont editorFont() const;
    double lineSpacingScale() const;
    void setLineSpacingScale(double scale);

    // ── 구문 강조 ──
    void    setLanguage(const QString& language);
    QString currentLanguage() const;
    void    autoDetectLanguage();

    // ── 편집 ──
    void setReadOnly(bool on);
    bool isReadOnly() const;
    bool isLimitedPreviewMode() const;
    bool isContentTruncated() const;
    QString contentLoadModeText() const;
    bool isHotExitEnabled() const;
    void setHotExitEnabled(bool enabled);
    bool openHotExitBackup(const QString& untitledId);
    void flushHotExitBackup();
    void discardHotExitBackup();
    bool canCopy() const override;
    bool copySelection() override;
    void goToLine(int line);
    void goToPosition(int line, int column);
    int  characterCount() const;
    int  tabWidth() const;
    void setTabWidth(int width);
    bool useTabs() const;
    void setUseTabs(bool use);
    bool indentationGuidesVisible() const;
    void setIndentationGuidesVisible(bool visible);
    ScintillaEditorSettings::IndentGuideStyle indentGuideStyle() const;
    void setIndentGuideStyle(ScintillaEditorSettings::IndentGuideStyle style);
    int  selectedCharacterCount() const;
    int  lineCount() const;
    int  currentLine() const;
    int  currentColumn() const;

    // ── 검색 / 치환 ──
    void findText(const QString& text,
                  bool regex = false,
                  bool caseSensitive = false,
                  bool wholeWords = false,
                  bool forward = true);
    void replaceText(const QString& find,
                     const QString& replace,
                     bool regex = false,
                     bool caseSensitive = false,
                     bool wholeWords = false,
                     bool forward = true);
    void replaceAll(const QString& find,
                    const QString& replace,
                    bool regex = false,
                    bool caseSensitive = false,
                    bool wholeWords = false,
                    bool forward = true);

    // ── 찾기/바꾸기 바 ──
    void showFindBar(bool replaceMode = false);
    void hideFindBar();
    bool IsFindBarVisible() const;

    // ── 줄바꿈 ──
    LineEnding detectedLineEnding() const;
    void       setLineEnding(LineEnding ending);
    bool       isWordWrapEnabled() const;
    void       setWordWrap(bool enabled);
    int WordWrapMode() const;
    void SetWordWrapMode(int Mode);
    void FoldAll(bool Contract);

    // ── 폰트 렌더링 ──
    ScintillaEditorSettings::FontRenderingMode fontRenderingMode() const;
    void setFontRenderingMode(ScintillaEditorSettings::FontRenderingMode mode);

    // ── 눈금자 ──
    bool isRulerVisible() const;
    void setRulerVisible(bool visible);

    // ── 공백/개행 문자 표시 ──
    bool isWhitespaceVisible() const;
    void setWhitespaceVisible(bool visible);

    // ── 코드 폴딩 ──
    bool isCodeFoldingEnabled() const;
    void setCodeFoldingEnabled(bool enabled);

    // ── 괄호 강조 ──
    bool isBraceHighlightEnabled() const;
    void setBraceHighlightEnabled(bool enabled);

    // ── Change History ──
    ScintillaEditorSettings::ChangeHistoryMode changeHistoryMode() const;
    void setChangeHistoryMode(ScintillaEditorSettings::ChangeHistoryMode mode);

    QToolBar* createToolBar() override;
    void setTheme(Theme theme) override;

signals:
    void ContentChanged();
    void EncodingRequested(const QString& Encoding);
    void encodingChanged(const QString& encoding);
    void languageChanged(const QString& language);
    void statusChanged();

protected:
    void renderPage(QPainter* painter, int pageIndex, const QRectF& rect) override;
    int  pageCount() const override;

private:
    int lastWrapMode_ = ScintillaEditorSettings::WrapChar;
    struct SearchOptions {
        bool regex = false;
        bool caseSensitive = false;
        bool wholeWords = false;
        bool forward = true;
    };

    bool promptOpenModeForFile(const QString& filePath, TextFileSession::OpenMode& openMode) const;
    bool ensureEditorBackend();
    void loadPersistedEditorPreferences();
    void applyPersistedEditorPreferences(ScintillaEditorSettings& settings) const;
    void applyEditorSettings();
    void showGoToLineDialog();
    void updateMetrics();
    static ScintillaDocument::LineEnding toDocumentLineEnding(LineEnding ending);
    static LineEnding fromDocumentLineEnding(ScintillaDocument::LineEnding ending);
    static LineEnding detectLineEnding(const QString& text);
    static QString newHotExitUntitledId();
    bool saveWithEncodingDialog(const QString& initialFilePath = {});
    bool currentFileHasBom() const;
    bool shouldUseHotExitForCurrentFile() const;
    void scheduleHotExitBackup();
    void writeHotExitBackupNow(bool synchronous = false);
    void onHotExitBackupTimer();
    void applyHotExitSettingsFromPreferences();

    QPointer<ITextEditorBackend> m_editor = nullptr;
    class ColumnRulerWidget*  m_ruler = nullptr;
    FindReplaceWidget*        m_findWidget = nullptr;
    TextFileSession       m_fileSession;
    ScintillaDocument     m_document;
    ScintillaEditorSettings m_editorSettings;

    QString             m_encoding;
    QString             m_detectedEncoding;
    QString             m_hotExitUntitledId;
    SearchOptions       m_searchOptions;
    std::atomic_int     m_openRequestId{0};
    int                 m_cachedLineCount = 1;
    int                 m_cachedCurrentLine = 1;
    int                 m_cachedCurrentColumn = 1;
    class QTimer*       m_hotExitTimer = nullptr;
    bool                m_hotExitEnabled = true;
    bool                m_hotExitDirty = false;

    // ── 제외 기능 ──
    static constexpr int kExcludeBackgroundIndicatorId = 20;
    static constexpr int kCurrentMatchIndicatorId = 21;
    static constexpr int kExcludeStrikeIndicatorId = 22;
    static constexpr int kExcludeBorderIndicatorId = 23;
    QVector<QPair<int,int>> m_excludedRanges; // (startPos, length)
    bool m_isSearching = false; // 검색 중 selectionChanged 무시 플래그
    class QTimer* m_searchDebounceTimer = nullptr;
    int m_currentMatchStart = -1;
    int m_currentMatchLength = 0;
    int m_searchResumePos = -1;

    void setupFindWidget();
    void connectFindWidgetSignals();
    void performSearch();
    void performFindNext();
    void performFindPrev();
    void performReplace();
    void performReplaceAll();
    void performExclude();
    void clearExcludedRanges();
    void updateExcludedCount();
    void clearCurrentMatchHighlight();
    void setCurrentMatchHighlight(int startPos, int length, bool scrollToMatch);
    int currentSearchFlags() const;
    bool isExcludedRange(int startPos, int length) const;
    bool findInRange(int rangeStart, int rangeEnd, bool forward, bool wrap, bool fromRangeBoundary = false);
    bool findInSelectionRange(bool forward, bool wrap, bool fromRangeBoundary = false);
    bool hasCurrentMatch() const;
    void updateMatchCount();
};
