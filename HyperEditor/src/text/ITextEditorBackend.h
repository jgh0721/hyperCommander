#pragma once

#include "text/ScintillaDocument.h"

#include <QObject>
#include <QColor>
#include <QFont>

class QAbstractScrollArea;
class QWidget;
struct ScintillaEditorSettings;

class ITextEditorBackend : public QObject
{
	Q_OBJECT

public:
	explicit ITextEditorBackend(QObject* parent = nullptr)
		: QObject(parent)
	{
	}

	~ITextEditorBackend() override = default;

	virtual QWidget* widget() const = 0;
	virtual QAbstractScrollArea* scrollArea() const = 0;

	virtual void applySettings(const ScintillaEditorSettings& settings) = 0;
	virtual void setText(const QString& text) = 0;
	virtual QString text() const = 0;
	virtual void clear() = 0;
    virtual int CharacterCount() const = 0;
    virtual int SelectedCharacterCount() const = 0;
    virtual QString WordAtCaret() const = 0;
    virtual void FoldAll(bool Contract) = 0;


	virtual void setModified(bool modified) = 0;
	virtual bool isModified() const = 0;
	virtual int lineCount() const = 0;

	virtual void setReadOnly(bool readOnly) = 0;
	virtual bool isReadOnly() const = 0;

	virtual bool hasSelectedText() const = 0;
	virtual QString selectedText() const = 0;
	virtual bool getSelectionRange(int& lineFrom, int& indexFrom, int& lineTo, int& indexTo) const = 0;
	virtual void setSelectionRange(int lineFrom, int indexFrom, int lineTo, int indexTo) = 0;
	virtual void setSelectionByPos(int startPos, int endPos) = 0;
	virtual void copy() = 0;

	virtual void setCursorPosition(int line, int index) = 0;
	virtual bool findFirst(const QString& text,
						   bool regex,
						   bool caseSensitive,
						   bool wholeWords,
						   bool wrap,
						   bool forward) = 0;
	virtual void replace(const QString& replacement) = 0;

	// ── 검색 카운트 / Indicator ──
	virtual int countMatches(const QString& text,
							 bool regex,
							 bool caseSensitive,
							 bool wholeWords) = 0;
	virtual int countMatchesInRange(const QString& text,
									bool regex,
									bool caseSensitive,
									bool wholeWords,
									int startPos,
									int endPos) = 0;
	virtual void setIndicatorStyle(int indicatorId, int style, const QColor& color) = 0;
	virtual void applyIndicator(int indicatorId, int startPos, int length) = 0;
	virtual void clearIndicator(int indicatorId, int startPos, int length) = 0;
	virtual void clearAllIndicator(int indicatorId) = 0;
	virtual int selectionStartPos() const = 0;
	virtual int selectionEndPos() const = 0;
	virtual int documentLength() const = 0;
	virtual int currentPos() const = 0;
	virtual int matchStartPos() const = 0;
	virtual int matchEndPos() const = 0;
	virtual void scrollRangeToView(int startPos, int endPos) = 0;

	// ── 검색 타겟 (replaceAll 지원) ──
	virtual void setSearchTargetRange(int startPos, int endPos) = 0;
	virtual int searchInTarget(const QString& text, int flags) = 0;
	virtual int targetEnd() const = 0;
	virtual void replaceInTarget(const QString& text) = 0;

	virtual void setEditorFont(const QFont& font) = 0;
	virtual QFont editorFont() const = 0;

	virtual int firstVisibleLine() const = 0;
	virtual void restoreViewState(int caretPosition, int firstVisibleLine) = 0;
	// 1-based document line plus the position inside its wrapped display rows, at a height ratio of the view.
	virtual double fractionalLineAtViewportRatio(double ratio) const = 0;
	virtual void scrollFractionalLineToViewportRatio(double fractionalLine, double ratio) = 0;
	virtual bool isScrolledToEnd() const = 0;
	virtual int textHeight(int line) const = 0;
	virtual int leftMarginWidth() const = 0;
	virtual int changeHistoryFlags() const = 0;
	virtual void setLineEnding(ScintillaDocument::LineEnding lineEnding, bool convertExisting) = 0;
	virtual bool applyLanguage(const QString& displayName) = 0;
	virtual void applyThemeColors(bool dark) = 0;

signals:
	void modificationChanged(bool modified);
	void cursorPositionChanged(int line, int index);
	void linesChanged();
	void textChanged();
	void selectionChanged();
	// Vertical position of the view changed: scrolling, folding, wrapping or a new height.
	void viewportScrolled();
	// 0-based line of an edit and the lines it added (negative when removed).
	void linesShifted(int line, int delta);
};


