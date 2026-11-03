#pragma once
#include "documentStatusBar.hpp"
#include "epubLineShifts.hpp"
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QElapsedTimer>
class QTextView;
class QToolBar;
class QTreeWidget;
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
    MarkdownDocumentView(QTextView *Source, const QString &Path, bool Editor, QWidget *Parent = nullptr);
    ~MarkdownDocumentView() override;
    QToolBar *Toolbar() const
    {
        return toolbar_;
    }
    bool SourceVisible() const;
    bool CanRun(const QString &Id) const;
    bool Run(const QString &Id);
    bool RunPreviewCommand(const QString &Id);
    void ReloadPreferences();
    void SetShortcutBindings(const QJsonObject &Bindings);
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
    QList<DocumentStatusField> StatusFields() const;
    QJsonObject State() const;
  signals:
    void Changed();
    void OpenRequested(const QString &Path);
    void EscapeRequested();

  protected:
    bool eventFilter(QObject *, QEvent *) override;

  public:
    // Asks the preview for measurements used by tests (renderer.js diagnostics()); the reply
    // appears as State()["diagnostics"]. Nothing is measured unless asked.
    void RequestDiagnostics(QJsonObject Request);

  private:
    void mode(const QString &Mode);
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
    // Line is in the previewed revision.
    void sendPosition(double Line, bool Scroll, const QJsonObject &Anchor = {});
    void saveToggle(const QString &Key, bool Value);
    QTextView *source_;
    QString path_, mode_, status_, heading_, error_;
    bool editor_ = false, readOnly_ = false, composing_ = false;
    QToolBar *toolbar_;
    QToolBar *sourceTools_ = nullptr;
    QWidget *outlinePanel_;
    QWidget *previewPanel_;
    QTreeWidget *outline_;
    QSplitter *splitter_;
    MarkdownProcessView *preview_;
    QLabel *message_;
    QMap<QString, QAction *> actions_;
    QTimer debounce_, sample_, sampleSoon_;
    QElapsedTimer suppress_;
    QJsonObject options_, previewScroll_, lastSourceAnchor_, diagnostics_;
    QJsonArray problemDetails_;
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
