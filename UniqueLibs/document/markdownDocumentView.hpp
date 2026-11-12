#pragma once
#include "documentStatusBar.hpp"
#include "epubLineShifts.hpp"
#include "documentEditRange.hpp"
#include "documentToolPanel.hpp"
#include <QPointer>
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QDateTime>
#include <QTimer>
#include <QElapsedTimer>
class QTextView;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class QSplitter;
class QLabel;
class QAction;
namespace hc::document
{
class MarkdownProcessView;
class MarkdownDocumentView final : public QWidget
{
    Q_OBJECT
  public:
    static constexpr int MinZoomPercent = 50, MaxZoomPercent = 300;
    MarkdownDocumentView(QTextView *Source, const QString &Path, bool Editor, QWidget *Parent = nullptr);
    ~MarkdownDocumentView() override;
    // The one-row "markdownToolbar"; while the source is shown it also carries the text editor's
    // replace, word wrap and whitespace actions (SetSourceTools).
    QToolBar *Toolbar() const
    {
        return toolbar_;
    }
    bool SourceVisible() const;
    bool CanRun(const QString &Id) const;
    // Value is used by markdown.zoomSet (percent); without one that command asks for it.
    bool Run(const QString &Id, const QJsonValue &Value = {});
    bool RunPreviewCommand(const QString &Id);
    void ReloadPreferences();
    void SetShortcutBindings(const QJsonObject &Bindings);
    // Takes replace, word wrap and whitespace from the text editor's bar into Toolbar() and keeps
    // that bar hidden. Find stays markdown.find, which serves both panes.
    void SetSourceTools(QToolBar *Tools);
    void Refresh(bool Force = false);
    void Focus();
    void SetReadOnly(bool ReadOnly);
    void SetPath(const QString &Path)
    {
        if (!Path.isEmpty() && path_ != Path)
        {
            path_ = Path;
            Refresh();
        }
    }
    QWidget *InputWidget() const;
    // Fields: "heading" (preview only), "markdown.viewMode", "mdzoom" (not in source mode) and
    // "render". heading, mdzoom and render are pickers.
    QList<DocumentStatusField> StatusFields() const;
    QJsonObject State() const;

    // Data for the status pickers.
    // Outline headings in document order: (text, current source line).
    QList<QPair<QString, int>> Headings() const;
    // Index in Headings() of the heading at the top of the preview, or -1.
    int CurrentHeadingIndex() const;
    // Nesting depth of a heading in the outline; 0 is top level.
    int HeadingDepth(int Index) const;
    void GoToHeading(int Index);
    int PreviewZoomPercent() const;
    void SetPreviewZoom(int Percent);
    int RenderProblemCount() const
    {
        return problems_;
    }
    // Problems of the rendered revision: {line, kind, message}.
    QJsonArray RenderProblems() const
    {
        return problemDetails_;
    }
    // When the shown revision finished rendering; invalid before the first render.
    QDateTime RenderedAt() const
    {
        return renderedAt_;
    }
    // Scrolls the preview to the source caret, showing the preview beside the source when hidden.
    void ShowSourceLineInPreview();
    // 미리보기 전체 화면: 미리보기만 남긴다. 끄면 들어가기 전의 보기 방식과 개요로 돌아간다.
    void SetPreviewOnly(bool On);
    bool IsPreviewOnly() const
    {
        return previewOnly_;
    }
    // 도킹 작업 공간: 개요 칸을 떼어 넘긴다(한 번만). 넘긴 뒤 개요 켜기 · 끄기는 작업 공간의 도구 창을 연다 · 닫는다.
    QList<DocumentToolPanelItem> TakeToolPanels();
    // 옆에 미리보기 열기: 미리보기 칸을 떼어 넘긴다. 보기 방식은 나란히로 두고 바꾸지 못한다. ReturnPreview로 되돌린다.
    QWidget *TakePreview();
    void ReturnPreview();
    bool IsPreviewTaken() const
    {
        return previewTaken_;
    }
  signals:
    void Changed();
    void OpenRequested(const QString &Path);
    void EscapeRequested();
    // markdown.fullScreen: 문서 창이 전체 화면을 켜고 끈다.
    void FullScreenToggleRequested();
    // 넘긴 패널을 보여 달라("outline").
    void ToolPanelRequested(const QString &Kind);

  protected:
    bool eventFilter(QObject *, QEvent *) override;

  public:
    // Asks the preview for measurements used by tests (renderer.js diagnostics()); the reply
    // appears as State()["diagnostics"]. Nothing is measured unless asked.
    void RequestDiagnostics(QJsonObject Request);

  private:
    enum class RenderState
    {
        Waiting,
        Rendering,
        Manual,
        Current,
        Partial
    };
    // One anchor message per frame; held keys repeat faster than that.
    static constexpr int SampleFrameMs = 16;
    // Upper bound of the delay that follows the last render's duration.
    static constexpr int MaxRenderDelayMs = 1500;
    // How long input may hold back a due render.
    static constexpr int MaxInputDeferMs = 2000;
    // Source bytes up to which edits render by themselves, and up to which the preview renders at all. An edit
    // sends only its bytes; the renderer still parses the whole text, so larger documents refresh on request.
    static constexpr qint64 AutoRenderBytes = 8LL * 1024 * 1024;
    static constexpr qint64 PreviewBytes = 16LL * 1024 * 1024;
    // Delay after an edit: the configured debounce, longer while renders take long.
    int renderDelay() const;
    // Keys queued for this thread, or typed within the configured debounce.
    bool inputPending() const;
    // The debounce expired: render unless the user is still typing.
    void renderWhenIdle();
    void scheduleSample();
    // The outline of the rendered revision's headings_, down to outlineDepth_.
    void rebuildOutline();
    void mode(const QString &Mode);
    bool previewOnly_ = false;
    QString modeBeforePreviewOnly_;
    void alignPreview();
    void report(const QJsonObject &Value);
    // Source -> preview: runs on editor signals, never on a timer.
    void sampleSource();
    // Only tracks which pane owns the native keyboard focus.
    void pollFocus();
    // The source anchor: (line, column) of the caret, or of the first character visible at the
    // left edge of the nearest row, and the screen height of that row as a preview offset.
    QJsonObject sourceScrollAnchor() const;
    // Preview -> source: puts the character's row at the screen height of the preview offset.
    void placeSource(double Line, int Column, double Offset, bool MoveCaret);
    double previewOffsetForSourceY(double Y) const;
    double sourceYForPreviewOffset(double Offset) const;
    // Lines of the source <-> lines of the revision the preview renders.
    double toPreviewLine(double Line) const;
    double fromPreviewLine(double Line) const;
    void syncControls();
    // Line is in the previewed revision. Reveal scrolls the preview to it even with scroll sync on.
    void sendPosition(double Line, bool Scroll, const QJsonObject &Anchor = {}, bool Reveal = false);
    void saveToggle(const QString &Key, bool Value);
    void focusSource();
    // Line is in the previewed revision.
    void goToHeadingLine(int Line);
    // Context menus. Value is a renderer "context" report: x, y in CSS px, selection, link, fragment, line, column.
    void showPreviewMenu(const QJsonObject &Value);
    void showOutlineMenu(const QPoint &Position);
    // Line is in the previewed revision; Offset is the CSS px height in the preview to keep.
    void showInSource(double Line, int Column, double Offset);
    void openLink(const QString &Url, const QString &Fragment);
    QString renderText() const;
    QString renderDetail() const;
    QTextView *source_;
    QString path_, mode_, status_, heading_, error_;
    bool editor_ = false, readOnly_ = false, composing_ = false;
    QToolBar *toolbar_;
    // Text editor actions shown in toolbar_ only while the source is visible.
    QList<QAction *> sourceActions_;
    QWidget *outlinePanel_;
    QWidget *previewPanel_;
    // 작업 공간에 넘긴 칸(이 화면이 지울 때 함께 지운다).
    QList<QPointer<QWidget>> takenPanels_;
    bool panelsTaken_ = false, previewTaken_ = false;
    QString modeBeforeTaken_;
    QTreeWidget *outline_;
    QSplitter *splitter_;
    MarkdownProcessView *preview_;
    QLabel *message_;
    QMap<QString, QAction *> actions_;
    QTimer debounce_, sample_, sampleSoon_;
    QElapsedTimer suppress_;
    // lastInput_: last key or IME event in the source. deferredSince_: a due render first waited
    // for input. renderClock_: the render request in flight. lastSampleSent_: last anchor message.
    QElapsedTimer lastInput_, deferredSince_, renderClock_, lastSampleSent_;
    int debounceMs_ = 250, outlineDepth_ = 6;
    qint64 lastRenderMs_ = 0;
    // The renderer keeps the text of helperRevision_ (sentText_ here); edits_ are the bytes changed in the
    // editor since then, so a request carries only those. A request with the same text, path and options
    // is not sent again: the preview already shows it, or is rendering it.
    QByteArray sentText_;
    DocumentEditRange edits_;
    qintptr editsDocument_ = 0;
    qint64 helperRevision_ = -1, fullRevision_ = -1, editRenders_ = 0, resyncs_ = 0;
    // The editor's text is the one the renderer holds (an edit undone back to it).
    bool sourceMatchesSent() const;
    QString sentPath_;
    QJsonObject sentOptions_;
    bool sentValid_ = false;
    QJsonObject options_, previewScroll_, lastSourceAnchor_, diagnostics_, bindings_;
    QJsonArray problemDetails_, headings_;
    // Top-level blocks the last render built and kept: {reused, built}.
    QJsonObject renderBlocks_;
    QDateTime renderedAt_;
    RenderState render_ = RenderState::Waiting;
    bool previewActive_ = false;
    qint64 revision_ = 0, rendered_ = -1, renderRequested_ = -1, scrollSerial_ = 0, anchorSyncs_ = 0;
    qintptr lastPosition_ = -1;
    int lastLine_ = -1, progress_ = 0, problems_ = 0;
    double zoom_ = 1;
    // Line edits since the last render request; the preview still shows that revision.
    QList<LineShift> shifts_;
    bool shiftsOverflow_ = false;
};
} // namespace hc::document
