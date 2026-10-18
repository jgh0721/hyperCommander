#pragma once

#include "text/ITextEditorBackend.h"

#include <QFont>
#include <QPointer>

class ScintillaEditBase;

namespace Scintilla {
class ILexer5;
}

struct ScintillaEditorSettings;

class ScintillaQtDirectBackend : public ITextEditorBackend
{
	Q_OBJECT

public:
    qintptr Send(unsigned int Message, quintptr W=0, qintptr L=0) const;
	explicit ScintillaQtDirectBackend(QWidget* editorParent, QObject* parent = nullptr);
	~ScintillaQtDirectBackend() override;

	QWidget* widget() const override;
	QAbstractScrollArea* scrollArea() const override;

	void applySettings(const ScintillaEditorSettings& settings) override;
	void setText(const QString& text) override;
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
	int textHeight(int line) const override;
	int leftMarginWidth() const override;
	int changeHistoryFlags() const override;
	void setLineEnding(ScintillaDocument::LineEnding lineEnding, bool convertExisting) override;
	bool applyLanguage(const QString& displayName) override;
	void applyThemeColors(bool dark) override;

private:
    mutable int characterCount_ = -1;
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
	bool                m_darkTheme = false;
	bool                m_codeFoldingEnabled = true;
	bool                m_braceMatchingEnabled = true;
	bool                m_highlightCurrentLine = true;
	bool                m_forcedModified = false;
    bool regexTarget_ = false;
	int                 m_lineNumberMarginDigits = 0;
	int                 m_lastKnownLineCount = 1;
};

