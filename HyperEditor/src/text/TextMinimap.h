#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimapHeadings.h"
#include "text/TextMinimapModel.h"
#include "text/TextMinimapRenderer.h"
#include "text/TextMinimapSettings.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들
class QTextView;
class ScintillaQtDirectBackend;
class TextMinimapSource;
class TextMinimapWidget;
class TextOverviewRuler;
class TextMinimapPeek;

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>

class QMenu;

// Minimap of one QTextView: the column beside the text (document thumbnail or outline), the marks on the vertical
// scroll bar and the hover preview. It reads the editor through TextMinimapSource, does all document-wide work in
// 4 ms slices on the UI thread and never changes the caret, the selection or the undo history except for the
// double-click "go to line".
class TextMinimap final : public QObject
{
    Q_OBJECT

public:
    struct Palette
    {
        QColor background_, separator_, slider_, sliderHover_, sliderActive_, bracket_, search_, selection_, caret_;
        QColor error_, warning_, changeModified_, changeSaved_, outline_, label_, labelMinor_, pending_;
    };
    struct ChangeMark
    {
        int line_ = 0;
        TextMinimapChange change_ = TextMinimapChange::None;
    };

    TextMinimap(QTextView* View, ScintillaQtDirectBackend* Backend);
    ~TextMinimap() override;

    // ── 편집기 연결 ──
    void ReloadSettings();
    void ThemeChanged();
    void LanguageChanged();
    void SetProblems(QVector<TextMinimapProblem> Problems);
    // Counts and collects matches. Returns the count when the search finished at once (documents up to 4 MiB),
    // otherwise -1 and SearchFinished() reports it later. Empty text clears the search layer.
    int RunSearch(const QString& Text, int Flags, qint64 Start, qint64 End);
    void ClearSearch();

    // ── 그리기 · 입력용 ──
    const TextMinimapSettings& Settings() const { return settings_; }
    const TextMinimapMode& Mode() const { return mode_; }
    const Palette& Colors() const { return palette_; }
    TextMinimapSource& Source() const { return *source_; }
    TextMinimapRenderer& Renderer() { return renderer_; }
    const TextMinimapOutline& Outline() const { return outline_; }
    const QVector<TextMinimapHeading>& Headings() const { return headings_.Headings(); }
    const TextMinimapSearch& Search() const { return search_; }
    const QVector<ChangeMark>& Changes() const { return changes_; }
    const QVector<TextMinimapProblem>& Problems() const { return problems_; }
    QColor StyleColor(int Style) const;
    QFont EditorFont() const;
    bool Dark() const;
    // Out of line: QPointer needs the complete widget types.
    TextMinimapWidget* Column() const;
    TextOverviewRuler* Ruler() const;
    TextMinimapPeek* Peek() const;
    QTextView* View() const;

    void ScrollRowToCenter(double Row);
    void ScrollToRow(int Row);
    void GoToLine(int Line);
    void ShowPeek(double Row, const QRect& Anchor, int GlobalY);
    void HidePeek();
    // Shared by the view menu, the EPUB menu and the minimap's own context menu.
    static void PopulateMenu(QMenu* Menu, QTextView* View);
    static void ToggleEnabled();

signals:
    void SearchFinished(int Count);

protected:
    bool eventFilter(QObject* Watched, QEvent* Event) override;

private:
    void connectBackend();
    void updateMode(bool Force = false);
    void placeWidgets();
    void documentReset();
    void textEdited(qint64 Position, qint64 Removed, qint64 Inserted);
    void linesShifted(int Line, int Delta);
    void stylesChanged(qint64 Position, qint64 Length);
    void viewportChanged();
    void layoutCheck();
    void reloadPalette();
    void restartOutline();
    void restartHeadings();
    void restartChanges(bool FullScan);
    void scheduleWork();
    void work();
    bool changesStep(const class QElapsedTimer& Clock, qint64 BudgetMs);
    void repaint();
    TextMinimapLineSet& candidates();

    QPointer<QTextView> view_;
    QPointer<ScintillaQtDirectBackend> backend_;
    std::unique_ptr<TextMinimapSource> source_;
    TextMinimapSettings settings_;
    TextMinimapMode mode_;
    Palette palette_;
    TextMinimapRenderer renderer_;
    TextMinimapOutline outline_;
    TextMinimapHeadings headings_;
    TextMinimapSearch search_;
    QVector<ChangeMark> changes_;
    QVector<ChangeMark> changesPending_;
    QVector<TextMinimapProblem> problems_;
    QHash<qintptr, TextMinimapLineSet> candidates_;
    QList<qintptr> candidateOrder_;
    int changeRange_ = 0;
    int changeLine_ = -1;
    bool changesRunning_ = false;
    bool fullScan_ = false;
    mutable QVector<QColor> styleColors_;
    QPointer<TextMinimapWidget> column_;
    QPointer<TextOverviewRuler> ruler_;
    QPointer<TextMinimapPeek> peek_;
    QTimer workTimer_;
    QTimer layoutTimer_;
    QTimer headingsTimer_;
    QTimer changesTimer_;
    QTimer searchTimer_;
    quint64 revision_ = 0;
    qintptr currentDocument_ = 0;
    int displayRows_ = -1;
    int lineCount_ = -1;
    int reserveLeft_ = 0;
    int reserveRight_ = 0;
    qint64 endStyled_ = 0;
};
