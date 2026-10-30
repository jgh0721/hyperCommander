#pragma once
#include "documentStatusBar.hpp"
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

  protected:
    bool eventFilter(QObject *, QEvent *) override;

  private:
    void mode(const QString &Mode);
    void report(const QJsonObject &Value);
    void sampleSource();
    void syncControls();
    void sendPosition(int Line, bool Scroll, double Ratio = .2);
    void saveToggle(const QString &Key, bool Value);
    QTextView *source_;
    QString path_, mode_, status_, heading_, error_;
    bool editor_ = false, readOnly_ = false, composing_ = false;
    QToolBar *toolbar_;
    QToolBar *sourceTools_ = nullptr;
    QWidget *outlinePanel_;
    QTreeWidget *outline_;
    QSplitter *splitter_;
    MarkdownProcessView *preview_;
    QLabel *message_;
    QMap<QString, QAction *> actions_;
    QTimer debounce_, sample_;
    QElapsedTimer suppress_;
    QJsonObject options_;
    QJsonArray problemDetails_;
    bool previewActive_ = false;
    qint64 revision_ = 0, rendered_ = -1;
    int lastLine_ = -1, lastTop_ = -1, progress_ = 0, problems_ = 0;
    double zoom_ = 1;
};
} // namespace hc::document
