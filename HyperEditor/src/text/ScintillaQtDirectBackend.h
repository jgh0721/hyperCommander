#pragma once

#include "text/ITextEditorBackend.h"

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QRect>

class QPaintEvent;
class ScintillaEditBase;

namespace Scintilla {
class ILexer5;
}

struct ScintillaEditorSettings;

class ScintillaQtDirectBackend : public ITextEditorBackend
{
	Q_OBJECT

public:
    // Current line marking shared by HyperEditor and HyperViewer. Nothing fills the line; 1 px rules run
    // above and below its text area and the line number cell is tinted. Inactive colors apply without focus.
    struct CurrentLineColors
    {
        QColor frame;
        QColor gutter;
        QColor number;
        QColor frameInactive;
        QColor gutterInactive;
        QColor numberInactive;
    };

    qintptr Send(unsigned int Message, quintptr W=0, qintptr L=0) const;
    // The character a scroll anchor names for Position. A space or tab has no counterpart on a rendered
    // page (one at a soft wrap hangs at the end of the previous rendered line), so the nearest other
    // character on the same display row stands for it, the next one first; Position when there is none.
    qintptr RowTextCharacter(qintptr Position) const;
    // Replaces the theme colors until the next applyThemeColors(), e.g. for a settings preview.
    void SetCurrentLineColors(const CurrentLineColors& Colors);
    // Keeps Left/Right logical pixels of the scroll area free beside the viewport (minimap column). Scintilla lays
    // out, wraps and scrolls inside the smaller viewport; the reserved strip belongs to the caller's widgets.
    void SetViewportReserve(int Left, int Right);
    // History markers stay queryable (SCI_MARKERGET) in every change history mode except Off. In the body-only mode
    // they are defined as SC_MARK_EMPTY so they neither take a margin nor tint the line background.
    bool ChangeHistoryQueryable() const;
	explicit ScintillaQtDirectBackend(QWidget* editorParent, QObject* parent = nullptr);
	~ScintillaQtDirectBackend() override;

	QWidget* widget() const override;
	QAbstractScrollArea* scrollArea() const override;

	void applySettings(const ScintillaEditorSettings& settings) override;
	void setText(const QString& text) override;
    void SetTextUtf8(const QByteArray& Bytes);
    // Shows another Scintilla document in this view; the caller owns its reference.
    void AttachDocument(qintptr Document);
	QString text() const override;
	void clear() override;
    int CharacterCount() const override;
    int SelectedCharacterCount() const override;
    QString WordAtCaret() const override;
    void FoldAll(bool Contract) override;


	void setModified(bool modified) override;
	bool isModified() const override;
	int lineCount() const override;

	void setReadOnly(bool readOnly) override;
	bool isReadOnly() const override;

	bool hasSelectedText() const override;
	QString selectedText() const override;
	bool getSelectionRange(int& lineFrom, int& indexFrom, int& lineTo, int& indexTo) const override;
	void setSelectionRange(int lineFrom, int indexFrom, int lineTo, int indexTo) override;
	void setSelectionByPos(int startPos, int endPos) override;
	void copy() override;

	void setCursorPosition(int line, int index) override;
	bool findFirst(const QString& text,
					   bool regex,
					   bool caseSensitive,
					   bool wholeWords,
					   bool wrap,
					   bool forward) override;
	void replace(const QString& replacement) override;

	int countMatches(const QString& text,
					 bool regex,
					 bool caseSensitive,
					 bool wholeWords) override;
	int countMatchesInRange(const QString& text,
							bool regex,
							bool caseSensitive,
							bool wholeWords,
							int startPos,
							int endPos) override;
	void setIndicatorStyle(int indicatorId, int style, const QColor& color) override;
	void applyIndicator(int indicatorId, int startPos, int length) override;
	void clearIndicator(int indicatorId, int startPos, int length) override;
	void clearAllIndicator(int indicatorId) override;
	int selectionStartPos() const override;
	int selectionEndPos() const override;
	int documentLength() const override;
	int currentPos() const override;
	int matchStartPos() const override;
	int matchEndPos() const override;
	void scrollRangeToView(int startPos, int endPos) override;

	void setSearchTargetRange(int startPos, int endPos) override;
	int searchInTarget(const QString& text, int flags) override;
	int targetEnd() const override;
	void replaceInTarget(const QString& text) override;

	void setEditorFont(const QFont& font) override;
	QFont editorFont() const override;

	int firstVisibleLine() const override;
	void restoreViewState(int caretPosition, int firstVisibleLine) override;
	double fractionalLineAtViewportRatio(double ratio) const override;
	void scrollFractionalLineToViewportRatio(double fractionalLine, double ratio) override;
	bool isScrolledToEnd() const override;
	int textHeight(int line) const override;
	int leftMarginWidth() const override;
	int changeHistoryFlags() const override;
	void setLineEnding(ScintillaDocument::LineEnding lineEnding, bool convertExisting) override;
	bool applyLanguage(const QString& displayName) override;
	void applyThemeColors(bool dark) override;

signals:
    // Lexer output changed for [Position, Position + Length). Coalesced to one signal per event loop turn.
    void StylesChanged(qint64 Position, qint64 Length);
    // Markers were added or removed; Line is the first line touched in this turn.
    void MarkersChanged(int Line);
    void IndicatorsChanged(qint64 Position, qint64 Length);
    // The view shows other text: SetTextUtf8() replaced it or AttachDocument() switched documents.
    void DocumentReset();

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct PendingRange
    {
        qint64 start = -1;
        qint64 end = -1;
        void Add(qint64 Start, qint64 End) { start = start < 0 ? Start : qMin(start, Start); end = qMax(end, End); }
        bool Empty() const { return start < 0; }
    };
    void queueDecorationSignals();
    void flushDecorationSignals();
    PendingRange pendingStyles_;
    PendingRange pendingIndicators_;
    int pendingMarkerLine_ = -1;
    bool decorationFlushQueued_ = false;
    bool historyQueryable_ = false;
    // Viewport rectangles of the current line. Wrapped sub-lines are included.
    struct CurrentLineBand
    {
        int line = -1;
        QRect text;
        QRect number;
        bool operator==(const CurrentLineBand&) const = default;
    };

    CurrentLineBand currentLineBand() const;
    void refreshCurrentLine(bool Repaint);
    void paintCurrentLine(const QPaintEvent* Event);

    mutable int characterCount_ = -1;
    CurrentLineColors currentLineColors_;
    CurrentLineBand currentLineBand_;
	int visibleAnchorLine(int line) const;
	void configureCodeFolding(bool enabled);
	void configureBraceHighlightIndicators();
	void syncLineCountState(bool emitSignal);
	void updateLineNumberMargin(int minimumDigits = 0);
	void emitCursorAndSelectionState();
	void updateBraceHighlight();
	void clearLexer();
	void applySyntaxStyles(bool dark);
	void setKeywordsForLexer(const QString& lexerKey);

	QPointer<ScintillaEditBase> m_editor;
	Scintilla::ILexer5*         m_currentLexer = nullptr; // Non-owning after SCI_SETILEXER while the editor is alive; cleared when the editor dies.
	QFont               m_editorFont;
	QString             m_currentLexerKey;
    QString             m_currentLanguageName;
	bool                m_darkTheme = false;
	bool                m_codeFoldingEnabled = true;
	bool                m_braceMatchingEnabled = true;
	bool                m_highlightCurrentLine = true;
	bool                m_forcedModified = false;
    bool regexTarget_ = false;
	int                 m_lineNumberMarginDigits = 0;
	int                 m_lastKnownLineCount = 1;
};

