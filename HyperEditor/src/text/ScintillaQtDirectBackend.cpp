#include "text/ScintillaQtDirectBackend.h"

#include "core/ThemeManager.h"
#include "text/ScintillaEditorSettings.h"
#include "text/TextLexerRegistry.h"
#include "commonLibs/document/lexerKeywords.hpp"

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QEvent>
#include <QFile>
#include <QFontMetrics>
#include <QTextStream>
#include <QWidget>

#include <ScintillaEditBase.h>
#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>

namespace {

using MessageInt = unsigned int;
constexpr int kSciSetLexer = 4001;
constexpr int kSciSetLexerLanguage = 4006;
constexpr sptr_t kInvalidPosition = static_cast<sptr_t>(-1);
constexpr int kBraceHighlightIndicatorId = 24;
constexpr int kBraceBadLightIndicatorId = 25;

MessageInt sciMessage(int value)
{
	return static_cast<MessageInt>(value);
}

int changeHistoryFlagsForMode(const ScintillaEditorSettings::ChangeHistoryMode mode)
{
	switch (mode) {
		case ScintillaEditorSettings::ChangeHistoryMarkers:
			return SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS;
		case ScintillaEditorSettings::ChangeHistoryIndicators:
			return SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_INDICATORS;
		case ScintillaEditorSettings::ChangeHistoryBoth:
			return SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS | SC_CHANGE_HISTORY_INDICATORS;
		case ScintillaEditorSettings::ChangeHistoryOff:
		default:
			return SC_CHANGE_HISTORY_ENABLED;
	}
}

void writeLexerTrace(const QString& displayName, const QString& lexerKey, const bool applied)
{
	const QString tracePath = QString::fromLocal8Bit(qgetenv("MV_TEXT_LEXER_TRACE_FILE")).trimmed();
	if (tracePath.isEmpty())
		return;

	QFile traceFile(tracePath);
	if (!traceFile.open(QIODevice::Append | QIODevice::Text))
		return;

	QTextStream stream(&traceFile);
	stream << displayName << '|'
		   << lexerKey << '|'
		   << (applied ? QStringLiteral("applied") : QStringLiteral("failed"))
		   << Qt::endl;
}

bool isBraceCharacter(const sptr_t ch)
{
	switch (static_cast<char>(ch)) {
		case '(':
		case ')':
		case '[':
		case ']':
		case '{':
		case '}':
			return true;
		default:
			return false;
	}
}

void applyLineSpacingScale(ScintillaEditBase* editor, const QFont& font, const double scale)
{
	if (!editor)
		return;

	const double safeScale = qBound(1.0, scale, 3.0);
	const QFontMetrics metrics(font);
	const int baseLineHeight = qMax(1, metrics.lineSpacing());
	const int targetLineHeight = qMax(baseLineHeight, qRound(baseLineHeight * safeScale));
	const int extraPixels = qMax(0, targetLineHeight - baseLineHeight);
	const int extraAscent = extraPixels / 2;
	const int extraDescent = extraPixels - extraAscent;

	editor->send(sciMessage(SCI_SETEXTRAASCENT), extraAscent);
	editor->send(sciMessage(SCI_SETEXTRADESCENT), extraDescent);
}

sptr_t positionFromLineIndex(ScintillaEditBase* editor, const int line, const int index)
{
	if (!editor)
		return 0;

	const sptr_t lineCount = qMax<sptr_t>(1, editor->send(sciMessage(SCI_GETLINECOUNT)));
	const sptr_t safeLine = qBound<sptr_t>(0, static_cast<sptr_t>(line), lineCount - 1);
	const sptr_t linePosition = editor->send(sciMessage(SCI_POSITIONFROMLINE), safeLine);
	const sptr_t safeIndex = qMax<sptr_t>(0, static_cast<sptr_t>(index));
	const sptr_t documentPosition = editor->send(sciMessage(SCI_POSITIONRELATIVECODEUNITS), linePosition, safeIndex);
	const sptr_t lineEnd = editor->send(sciMessage(SCI_GETLINEENDPOSITION), safeLine);
	return qBound<sptr_t>(linePosition, documentPosition, lineEnd);
}

void positionToLineIndex(ScintillaEditBase* editor, const sptr_t position, int& line, int& index)
{
	if (!editor) {
		line = 0;
		index = 0;
		return;
	}

	const sptr_t safePosition = qBound<sptr_t>(0, position, editor->send(sciMessage(SCI_GETLENGTH)));
	line = static_cast<int>(editor->send(sciMessage(SCI_LINEFROMPOSITION), safePosition));
	const sptr_t linePosition = editor->send(sciMessage(SCI_POSITIONFROMLINE), line);
	index = static_cast<int>(editor->send(sciMessage(SCI_COUNTCODEUNITS), linePosition, safePosition));
}

} // namespace

ScintillaQtDirectBackend::ScintillaQtDirectBackend(QWidget* editorParent, QObject* parent)
	: ITextEditorBackend(parent)
	, m_editor(new ScintillaEditBase(editorParent))
{
	m_editorFont = m_editor->font();

	// 리사이즈 시 깜빡임 방지: 시스템 배경 지움 비활성화
	// Scintilla paints the viewport only. The scroll area must still erase
	// exposed edges and the corner between its two scroll bars.
	m_editor->viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
	m_editor->viewport()->setAttribute(Qt::WA_NoSystemBackground);
	m_editor->setAutoFillBackground(true);

	// Windows DirectWrite 렌더링: 안티앨리어싱 격자 아티팩트 방지
	m_editor->send(sciMessage(SCI_SETTECHNOLOGY), 4 );
	m_editor->send(sciMessage(SCI_SETBUFFEREDDRAW), 0);
	m_editor->send(sciMessage(SCI_SETFONTQUALITY),
		static_cast<sptr_t>(Scintilla::FontQuality::QualityAntialiased));
	m_editor->send(sciMessage(SCI_SETIDLESTYLING), SC_IDLESTYLING_ALL);

	// 멀티 페이즈 드로잉: 배경을 연속된 영역으로 그려 글자 간 갭 방지
	m_editor->send(sciMessage(SCI_SETPHASESDRAW), SC_PHASES_MULTIPLE);
	// 레이아웃 캐시: 문서 전체 캐시로 서브픽셀 위치 일관성 유지
	m_editor->send(sciMessage(SCI_SETLAYOUTCACHE), SC_CACHE_PAGE);
	// 선택 레이어: 텍스트 아래 반투명 배경으로 그려 원래 lexer 글자가 계속 보이게 한다.
	m_editor->send(sciMessage(SCI_SETSELECTIONLAYER), SC_LAYER_UNDER_TEXT);
    m_editor->send(sciMessage(SCI_SETMULTIPLESELECTION), 1);
    m_editor->send(sciMessage(SCI_SETADDITIONALSELECTIONTYPING), 1);
    m_editor->send(sciMessage(SCI_SETMULTIPASTE), SC_MULTIPASTE_EACH);
    // End and Shift+End stop at the current wrapped display line.
    m_editor->send(sciMessage(SCI_ASSIGNCMDKEY), SCK_END, SCI_LINEENDDISPLAY);
    m_editor->send(sciMessage(SCI_ASSIGNCMDKEY), SCK_END | (SCMOD_SHIFT << 16), SCI_LINEENDDISPLAYEXTEND);
	// 사각형 선택 활성화
	m_editor->send(sciMessage(SCI_SETVIRTUALSPACEOPTIONS), SCVS_RECTANGULARSELECTION);

	// 제어문자/공백/EOL 기본 숨김
	m_editor->send(sciMessage(SCI_SETVIEWWS), SCWS_INVISIBLE);
	m_editor->send(sciMessage(SCI_SETVIEWEOL), 0);
	m_editor->send(sciMessage(SCI_SETCONTROLCHARSYMBOL), static_cast<sptr_t>(' '));

	m_editor->send(sciMessage(SCI_SETCODEPAGE), Scintilla::CpUtf8);
	m_editor->send(sciMessage(SCI_SETMARGINTYPEN), 0, static_cast<sptr_t>(Scintilla::MarginType::Number));
	configureBraceHighlightIndicators();
	configureCodeFolding(m_codeFoldingEnabled);
	updateLineNumberMargin();
	connect(m_editor, &QObject::destroyed, this, [this] {
		m_currentLexer = nullptr;
		m_editor = nullptr;
	});

	connect(m_editor, &ScintillaEditBase::savePointChanged,
			this, [this](bool dirty) {
				m_forcedModified = dirty;
				emit modificationChanged(dirty);
			});
    // Folding, markers and styling are view changes, not document edits.
    connect(m_editor, &ScintillaEditBase::modified, this,
        [this](Scintilla::ModificationFlags Type, Scintilla::Position Position, Scintilla::Position,
               Scintilla::Position LinesAdded, const QByteArray& ChangedText, Scintilla::Position,
               Scintilla::FoldLevel, Scintilla::FoldLevel) {
            const bool Inserted = (static_cast<int>(Type) & SC_MOD_INSERTTEXT) != 0;
            const bool Deleted = (static_cast<int>(Type) & SC_MOD_DELETETEXT) != 0;
            if (!Inserted && !Deleted) return;
            if (characterCount_ >= 0) {
                const int Delta = static_cast<int>(QString::fromUtf8(ChangedText).size());
                characterCount_ += Inserted ? Delta : -Delta;
                if (characterCount_ < 0) characterCount_ = -1;
            }
            syncLineCountState(true);
            // Undo and redo arrive here too, so line maps kept by hosts stay in step.
            if (LinesAdded != 0) emit linesShifted(static_cast<int>(Send(SCI_LINEFROMPOSITION, Position)), static_cast<int>(LinesAdded));
            emit textChanged();
        });
	connect(m_editor, &ScintillaEditBase::updateUi,
			this, [this](Scintilla::Update Updated) {
				updateBraceHighlight();
				emitCursorAndSelectionState();
				if ((static_cast<int>(Updated) & static_cast<int>(Scintilla::Update::VScroll)) != 0)
					emit viewportScrolled();
			});
	connect(m_editor, &ScintillaEditBase::marginClicked,
			this, [this](Scintilla::Position position, Scintilla::KeyMod, int margin) {
				if (!m_editor || !m_codeFoldingEnabled || margin != 1)
					return;
				const auto line = static_cast<sptr_t>(m_editor->send(sciMessage(SCI_LINEFROMPOSITION), position));
				m_editor->send(sciMessage(SCI_TOGGLEFOLD), line);
				// Folding moves the text below without a VScroll update.
				emit viewportScrolled();
			});
	// A new height moves the line at any fixed height ratio.
	m_editor->installEventFilter(this);
	m_lastKnownLineCount = lineCount();
}

ScintillaQtDirectBackend::~ScintillaQtDirectBackend()
{
	clearLexer();
}

QWidget* ScintillaQtDirectBackend::widget() const
{
	return m_editor;
}

QAbstractScrollArea* ScintillaQtDirectBackend::scrollArea() const
{
	return m_editor;
}

void ScintillaQtDirectBackend::applySettings(const ScintillaEditorSettings& settings)
{
	if (!m_editor)
		return;

	m_editor->send(sciMessage(SCI_SETCODEPAGE), settings.useUtf8 ? Scintilla::CpUtf8 : 0);
	m_editor->send(sciMessage(SCI_SETMARGINTYPEN), 0, static_cast<sptr_t>(Scintilla::MarginType::Number));
	m_editor->send(sciMessage(SCI_SETINDENTATIONGUIDES),
		settings.showIndentationGuides ? static_cast<sptr_t>(settings.indentGuideStyle) : static_cast<sptr_t>(Scintilla::IndentView::None));
	m_editor->send(sciMessage(SCI_SETCARETLINEVISIBLE), settings.highlightCurrentLine ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETINDENT), settings.tabWidth);
	m_editor->send(sciMessage(SCI_SETTABWIDTH), settings.tabWidth);
	m_editor->send(sciMessage(SCI_SETUSETABS), settings.useTabs ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETTABINDENTS), settings.autoIndent ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETBACKSPACEUNINDENTS), settings.autoIndent ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETREADONLY), settings.readOnly ? 1 : 0);
	const bool WrapChanged = m_editor->send(sciMessage(SCI_GETWRAPMODE)) != static_cast<sptr_t>(settings.wrapMode);
	m_editor->send(sciMessage(SCI_SETWRAPMODE),
        static_cast<sptr_t>(settings.wrapMode));
	if (WrapChanged) emit viewportScrolled();
    m_editor->send(sciMessage(SCI_SETWRAPVISUALFLAGS), settings.wrapVisualFlags & ScintillaEditorSettings::WrapFlagMask);
    m_editor->send(sciMessage(SCI_SETWRAPINDENTMODE), static_cast<sptr_t>(settings.wrapIndentMode));
	m_codeFoldingEnabled = settings.showCodeFolding;
	configureCodeFolding(m_codeFoldingEnabled);
	const int changeHistoryFlags = changeHistoryFlagsForMode(settings.changeHistoryMode);
	// Keep tracking enabled while its markers are hidden. Toggling preferences
	// must not erase the document's undo history.
	m_editor->send(sciMessage(SCI_SETCHANGEHISTORY), changeHistoryFlags);

	const bool historyMarkersEnabled = (changeHistoryFlags & SC_CHANGE_HISTORY_MARKERS) != 0;
	m_editor->send(sciMessage(SCI_SETMARGINTYPEN), 2, SC_MARGIN_SYMBOL);
	m_editor->send(sciMessage(SCI_SETMARGINMASKN), 2, SC_MASK_HISTORY);
	m_editor->send(sciMessage(SCI_SETMARGINWIDTHN), 2, historyMarkersEnabled ? 4 : 0);
	m_editor->send(sciMessage(SCI_SETMARGINSENSITIVEN), 2, 0);

	m_editor->send(sciMessage(SCI_SETVIEWWS),
		settings.showWhitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
	m_editor->send(sciMessage(SCI_SETVIEWEOL), settings.showWhitespace ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETCONTROLCHARSYMBOL),
		settings.showWhitespace ? 0 : static_cast<sptr_t>(' '));
	m_editor->send(sciMessage(SCI_SETFONTQUALITY),
		static_cast<sptr_t>(settings.fontRendering));
	applyLineSpacingScale(m_editor, m_editorFont.resolve(m_editor->font()), settings.lineSpacingScale);
	m_lineNumberMarginDigits = settings.showLineNumbers ? settings.lineNumberMarginDigits : 0;
	m_braceMatchingEnabled = settings.braceMatching;
	m_highlightCurrentLine = settings.highlightCurrentLine;
	configureBraceHighlightIndicators();
	if (!m_braceMatchingEnabled)
		m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), kInvalidPosition, kInvalidPosition);
	updateBraceHighlight();
	updateLineNumberMargin(m_lineNumberMarginDigits);

	// Display settings do not change lexical state. Preserve idle styling instead
	// of synchronously recolouring the entire document on every settings update.
	m_editor->update();
}

void ScintillaQtDirectBackend::setText(const QString& text)
{
    SetTextUtf8(text.toUtf8());
}
void ScintillaQtDirectBackend::SetTextUtf8(const QByteArray& utf8)
{
	if (!m_editor)
		return;

	const int changeHistoryFlags = static_cast<int>(m_editor->send(sciMessage(SCI_GETCHANGEHISTORY)));
	if (changeHistoryFlags != SC_CHANGE_HISTORY_DISABLED)
		m_editor->send(sciMessage(SCI_SETCHANGEHISTORY), SC_CHANGE_HISTORY_DISABLED);

	characterCount_ = -1;
	m_editor->sends(sciMessage(SCI_SETTEXT), 0, utf8.constData());
	m_editor->send(sciMessage(SCI_EMPTYUNDOBUFFER));
	m_editor->send(sciMessage(SCI_SETSAVEPOINT));
	if (changeHistoryFlags != SC_CHANGE_HISTORY_DISABLED)
		m_editor->send(sciMessage(SCI_SETCHANGEHISTORY), changeHistoryFlags);
	syncLineCountState(false);
	m_forcedModified = false;
}

void ScintillaQtDirectBackend::AttachDocument(qintptr Document)
{
	if (!m_editor || !Document)
		return;

	m_editor->send(sciMessage(SCI_SETDOCPOINTER), 0, Document);
	// Code page and the cached counts belong to the document that was replaced.
	m_editor->send(sciMessage(SCI_SETCODEPAGE), Scintilla::CpUtf8);
	characterCount_ = -1;
	m_forcedModified = m_editor->send(sciMessage(SCI_GETMODIFY)) != 0;
	syncLineCountState(false);
	updateBraceHighlight();
}

QString ScintillaQtDirectBackend::text() const
{
	if (!m_editor)
		return {};

	const sptr_t length = m_editor->send(sciMessage(SCI_GETTEXTLENGTH));
	QByteArray buffer(static_cast<int>(length) + 1, Qt::Uninitialized);
	m_editor->send(sciMessage(SCI_GETTEXT), length + 1, reinterpret_cast<sptr_t>(buffer.data()));
	return QString::fromUtf8(buffer.constData());
}

void ScintillaQtDirectBackend::clear()
{
	if (!m_editor)
		return;

	m_editor->send(sciMessage(SCI_CLEARALL));
	m_forcedModified = false;
	syncLineCountState(false);
	updateBraceHighlight();
}

void ScintillaQtDirectBackend::setModified(bool modified)
{
	if (!m_editor)
		return;

	if (!modified)
		m_editor->send(sciMessage(SCI_SETSAVEPOINT));
	m_forcedModified = modified;
	emit modificationChanged(isModified());
}

bool ScintillaQtDirectBackend::isModified() const
{
	if (!m_editor)
		return false;

	return m_forcedModified || (m_editor->send(sciMessage(SCI_GETMODIFY)) != 0);
}

int ScintillaQtDirectBackend::lineCount() const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_GETLINECOUNT))) : 0;
}

void ScintillaQtDirectBackend::setReadOnly(bool readOnly)
{
	if (m_editor)
		m_editor->send(sciMessage(SCI_SETREADONLY), readOnly ? 1 : 0);
}

bool ScintillaQtDirectBackend::isReadOnly() const
{
	return m_editor ? (m_editor->send(sciMessage(SCI_GETREADONLY)) != 0) : false;
}

bool ScintillaQtDirectBackend::hasSelectedText() const
{
	return m_editor ? (m_editor->send(sciMessage(SCI_GETSELECTIONEMPTY)) == 0) : false;
}

QString ScintillaQtDirectBackend::selectedText() const
{
	if (!m_editor || !hasSelectedText())
		return {};

	const sptr_t lengthWithTerminator = m_editor->send(sciMessage(SCI_GETSELTEXT));
	if (lengthWithTerminator <= 1)
		return {};

	QByteArray buffer(static_cast<int>(lengthWithTerminator), Qt::Uninitialized);
	m_editor->send(sciMessage(SCI_GETSELTEXT), 0, reinterpret_cast<sptr_t>(buffer.data()));
	return QString::fromUtf8(buffer.constData());
}

bool ScintillaQtDirectBackend::getSelectionRange(int& lineFrom, int& indexFrom, int& lineTo, int& indexTo) const
{
	if (!m_editor) {
		lineFrom = 0;
		indexFrom = 0;
		lineTo = 0;
		indexTo = 0;
		return false;
	}

	const sptr_t selectionStart = m_editor->send(sciMessage(SCI_GETSELECTIONSTART));
	const sptr_t selectionEnd = m_editor->send(sciMessage(SCI_GETSELECTIONEND));
	positionToLineIndex(m_editor, selectionStart, lineFrom, indexFrom);
	positionToLineIndex(m_editor, selectionEnd, lineTo, indexTo);
	return selectionStart != selectionEnd;
}

void ScintillaQtDirectBackend::setSelectionRange(int lineFrom, int indexFrom, int lineTo, int indexTo)
{
	if (!m_editor)
		return;

	const sptr_t start = positionFromLineIndex(m_editor, lineFrom, indexFrom);
	const sptr_t end = positionFromLineIndex(m_editor, lineTo, indexTo);
	m_editor->send(sciMessage(SCI_SETSEL), start, end);
	updateBraceHighlight();
	emitCursorAndSelectionState();
}

void ScintillaQtDirectBackend::setSelectionByPos(int startPos, int endPos)
{
	if (!m_editor) return;
	m_editor->send(sciMessage(SCI_SETSEL), startPos, endPos);
	updateBraceHighlight();
	emitCursorAndSelectionState();
}

void ScintillaQtDirectBackend::copy()
{
	if (m_editor)
		m_editor->send(sciMessage(SCI_COPY));
}

void ScintillaQtDirectBackend::setCursorPosition(int line, int index)
{
	if (!m_editor)
		return;

	const sptr_t maxLine = qMax<sptr_t>(0, static_cast<sptr_t>(lineCount() - 1));
	const sptr_t safeLine = qMax<sptr_t>(0, qMin<sptr_t>(static_cast<sptr_t>(line), maxLine));
	const sptr_t safeIndex = qMax<sptr_t>(0, static_cast<sptr_t>(index));
	const sptr_t position = positionFromLineIndex(m_editor,
		static_cast<int>(safeLine),
		static_cast<int>(safeIndex));
	m_editor->send(sciMessage(SCI_SETEMPTYSELECTION), position);
	m_editor->send(sciMessage(SCI_SCROLLCARET));
	updateBraceHighlight();
	emitCursorAndSelectionState();
}

bool ScintillaQtDirectBackend::findFirst(const QString& text,
									 bool regex,
									 bool caseSensitive,
									 bool wholeWords,
									 bool wrap,
									 bool forward)
{
	if (!m_editor || text.isEmpty())
		return false;

	int flags = 0;
	if (caseSensitive)
		flags |= SCFIND_MATCHCASE;
	if (wholeWords)
		flags |= SCFIND_WHOLEWORD;
	if (regex)
		flags |= SCFIND_CXX11REGEX | SCFIND_REGEXP;

	int lineFrom = 0;
	int indexFrom = 0;
	int lineTo = 0;
	int indexTo = 0;
	if (getSelectionRange(lineFrom, indexFrom, lineTo, indexTo))
		setSelectionRange(forward ? lineTo : lineFrom,
					  forward ? indexTo : indexFrom,
					  forward ? lineTo : lineFrom,
					  forward ? indexTo : indexFrom);
	else
		m_editor->send(sciMessage(SCI_SETEMPTYSELECTION), m_editor->send(sciMessage(SCI_GETCURRENTPOS)));

	const QByteArray needle = text.toUtf8();
    regexTarget_=(flags & (SCFIND_REGEXP|SCFIND_CXX11REGEX))!=0;
	m_editor->send(sciMessage(SCI_SETSEARCHFLAGS), flags);
	m_editor->send(sciMessage(SCI_SEARCHANCHOR));

	const MessageInt message = sciMessage(forward ? SCI_SEARCHNEXT : SCI_SEARCHPREV);
	if (m_editor->sends(message, flags, needle.constData()) != -1) {
		m_editor->send(sciMessage(SCI_SCROLLCARET));
		return true;
	}

	if (!wrap)
		return false;

	const sptr_t wrapPosition = forward ? 0 : m_editor->send(sciMessage(SCI_GETLENGTH));
	m_editor->send(sciMessage(SCI_SETEMPTYSELECTION), wrapPosition);
	m_editor->send(sciMessage(SCI_SEARCHANCHOR));
	if (m_editor->sends(message, flags, needle.constData()) == -1)
		return false;

	m_editor->send(sciMessage(SCI_SCROLLCARET));
	return true;
}

void ScintillaQtDirectBackend::replace(const QString& replacement)
{
	if (!m_editor)
		return;

	const QByteArray utf8 = replacement.toUtf8();
	m_editor->sends(sciMessage(SCI_REPLACESEL), 0, utf8.constData());
}

int ScintillaQtDirectBackend::countMatches(const QString& text,
										   bool regex,
										   bool caseSensitive,
										   bool wholeWords)
{
	if (!m_editor || text.isEmpty())
		return 0;
	return countMatchesInRange(text, regex, caseSensitive, wholeWords,
							   0, static_cast<int>(m_editor->send(sciMessage(SCI_GETLENGTH))));
}

int ScintillaQtDirectBackend::countMatchesInRange(const QString& text,
												  bool regex,
												  bool caseSensitive,
												  bool wholeWords,
												  int startPos,
												  int endPos)
{
	if (!m_editor || text.isEmpty() || startPos >= endPos)
		return 0;

	int flags = 0;
	if (caseSensitive) flags |= SCFIND_MATCHCASE;
	if (wholeWords)    flags |= SCFIND_WHOLEWORD;
	if (regex)         flags |= SCFIND_CXX11REGEX | SCFIND_REGEXP;

	const QByteArray needle = text.toUtf8();
	int count = 0;
	int searchStart = startPos;

	while (searchStart < endPos) {
		m_editor->send(sciMessage(SCI_SETTARGETSTART), searchStart);
		m_editor->send(sciMessage(SCI_SETTARGETEND), endPos);
		m_editor->send(sciMessage(SCI_SETSEARCHFLAGS), flags);
		const sptr_t pos = m_editor->sends(sciMessage(SCI_SEARCHINTARGET),
										   static_cast<sptr_t>(needle.size()),
										   needle.constData());
		if (pos < 0)
			break;
		++count;
		const int matchEnd = static_cast<int>(m_editor->send(sciMessage(SCI_GETTARGETEND)));
		searchStart = qMax(matchEnd, searchStart + 1);
	}
	return count;
}

void ScintillaQtDirectBackend::setIndicatorStyle(int indicatorId, int style, const QColor& color)
{
	if (!m_editor)
		return;
	m_editor->send(sciMessage(SCI_INDICSETSTYLE), indicatorId, style);
	// Search/exclusion fills must not tint the glyphs drawn above them.
	m_editor->send(sciMessage(SCI_INDICSETUNDER), indicatorId, 1);
	const int sciColor = color.red() | (color.green() << 8) | (color.blue() << 16);
	m_editor->send(sciMessage(SCI_INDICSETFORE), indicatorId, sciColor);
	m_editor->send(sciMessage(SCI_INDICSETALPHA), indicatorId, color.alpha());
	m_editor->send(sciMessage(SCI_INDICSETOUTLINEALPHA), indicatorId, qMax(color.alpha(), 160));
}

void ScintillaQtDirectBackend::applyIndicator(int indicatorId, int startPos, int length)
{
	if (!m_editor || length <= 0)
		return;
	m_editor->send(sciMessage(SCI_SETINDICATORCURRENT), indicatorId);
	m_editor->send(sciMessage(SCI_INDICATORFILLRANGE), startPos, length);
}

void ScintillaQtDirectBackend::clearIndicator(int indicatorId, int startPos, int length)
{
	if (!m_editor || length <= 0)
		return;
	m_editor->send(sciMessage(SCI_SETINDICATORCURRENT), indicatorId);
	m_editor->send(sciMessage(SCI_INDICATORCLEARRANGE), startPos, length);
}

void ScintillaQtDirectBackend::clearAllIndicator(int indicatorId)
{
	if (!m_editor)
		return;
	const int docLen = static_cast<int>(m_editor->send(sciMessage(SCI_GETLENGTH)));
	clearIndicator(indicatorId, 0, docLen);
}

int ScintillaQtDirectBackend::selectionStartPos() const
{
	if (!m_editor) return 0;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETSELECTIONSTART)));
}

int ScintillaQtDirectBackend::selectionEndPos() const
{
	if (!m_editor) return 0;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETSELECTIONEND)));
}

int ScintillaQtDirectBackend::documentLength() const
{
	if (!m_editor) return 0;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETLENGTH)));
}

int ScintillaQtDirectBackend::currentPos() const
{
	if (!m_editor) return 0;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETCURRENTPOS)));
}

int ScintillaQtDirectBackend::matchStartPos() const
{
	if (!m_editor) return -1;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETSELECTIONSTART)));
}

int ScintillaQtDirectBackend::matchEndPos() const
{
	if (!m_editor) return -1;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETSELECTIONEND)));
}

void ScintillaQtDirectBackend::scrollRangeToView(int startPos, int endPos)
{
	if (!m_editor)
		return;

	const sptr_t start = qMax<sptr_t>(0, static_cast<sptr_t>(startPos));
	const sptr_t end = qMax(start, static_cast<sptr_t>(endPos));
	m_editor->send(sciMessage(SCI_SCROLLRANGE), start, end);
}

void ScintillaQtDirectBackend::setSearchTargetRange(int startPos, int endPos)
{
	if (!m_editor) return;
	m_editor->send(sciMessage(SCI_SETTARGETSTART), startPos);
	m_editor->send(sciMessage(SCI_SETTARGETEND), endPos);
}

int ScintillaQtDirectBackend::searchInTarget(const QString& text, int flags)
{
    if(!m_editor)return -1;regexTarget_=(flags&(SCFIND_REGEXP|SCFIND_CXX11REGEX))!=0;
    m_editor->send(sciMessage(SCI_SETSEARCHFLAGS),flags);const auto Bytes=text.toUtf8();
    return int(m_editor->sends(sciMessage(SCI_SEARCHINTARGET),Bytes.size(),Bytes.constData()));
}

int ScintillaQtDirectBackend::targetEnd() const
{
	if (!m_editor) return -1;
	return static_cast<int>(m_editor->send(sciMessage(SCI_GETTARGETEND)));
}

void ScintillaQtDirectBackend::replaceInTarget(const QString& text)
{
	if (!m_editor) return;
	const QByteArray utf8 = text.toUtf8();
	m_editor->sends(sciMessage(regexTarget_?SCI_REPLACETARGETRE:SCI_REPLACETARGET), utf8.size(), utf8.constData());
}

void ScintillaQtDirectBackend::setEditorFont(const QFont& font)
{
	if (!m_editor)
		return;

	m_editorFont = font;
	m_editor->setFont(font);

	const QByteArray family = font.family().toUtf8();
	const int pointSize = qMax(1, font.pointSize() > 0 ? font.pointSize() : font.pointSizeF() > 0.0 ? qRound(font.pointSizeF()) : 10);
	m_editor->sends(sciMessage(SCI_STYLESETFONT), STYLE_DEFAULT, family.constData());
	m_editor->send(sciMessage(SCI_STYLESETSIZE), STYLE_DEFAULT, pointSize);
	m_editor->sends(sciMessage(SCI_STYLESETFONT), STYLE_LINENUMBER, family.constData());
	m_editor->send(sciMessage(SCI_STYLESETSIZE), STYLE_LINENUMBER, pointSize);
	// Propagate the font while restoring colors cleared by SCI_STYLECLEARALL.
	// Preference reloads must preserve gutter, indent and lexer styles.
	applyThemeColors(m_darkTheme);
	updateLineNumberMargin(m_lineNumberMarginDigits);
}

QFont ScintillaQtDirectBackend::editorFont() const
{
	return m_editorFont;
}

int ScintillaQtDirectBackend::firstVisibleLine() const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_GETFIRSTVISIBLELINE))) : 0;
}

void ScintillaQtDirectBackend::restoreViewState(int caretPosition, int firstVisibleLine)
{
	if (!m_editor)
		return;

	const sptr_t documentLength = m_editor->send(sciMessage(SCI_GETLENGTH));
	const sptr_t safeCaret = qBound<sptr_t>(0, static_cast<sptr_t>(caretPosition), documentLength);
	m_editor->send(sciMessage(SCI_GOTOPOS), safeCaret);

	const int currentFirstVisibleLine = static_cast<int>(m_editor->send(sciMessage(SCI_GETFIRSTVISIBLELINE)));
	m_editor->send(sciMessage(SCI_LINESCROLL), 0, qMax(0, firstVisibleLine) - currentFirstVisibleLine);
	updateBraceHighlight();
	emitCursorAndSelectionState();
}

double ScintillaQtDirectBackend::fractionalLineAtViewportRatio(double ratio) const
{
	if (!m_editor)
		return 1.0;
	// SCI_POSITIONFROMPOINT takes pixels, so wrapping and folding are already applied.
	const int Height = qMax(1, m_editor->viewport()->height());
	const sptr_t Y = static_cast<sptr_t>(Height * qBound(0.0, ratio, 1.0));
	const sptr_t Position = Send(SCI_POSITIONFROMPOINT, 0, Y);
	if (Position < 0)
		return static_cast<double>(Send(SCI_DOCLINEFROMVISIBLE, Send(SCI_GETFIRSTVISIBLELINE)) + 1);
	const int Line = static_cast<int>(Send(SCI_LINEFROMPOSITION, Position));
	const sptr_t Top = Send(SCI_POINTYFROMPOSITION, 0, Send(SCI_POSITIONFROMLINE, Line));
	const int Row = qMax(1, textHeight(Line));
	const int Rows = qMax(1, static_cast<int>(Send(SCI_WRAPCOUNT, Line)));
	return Line + 1 + qBound(0.0, static_cast<double>(Y - Top) / (Rows * Row), 0.999);
}

void ScintillaQtDirectBackend::scrollFractionalLineToViewportRatio(double fractionalLine, double ratio)
{
	if (!m_editor)
		return;
	const int Total = qMax(1, lineCount());
	const double Clamped = qBound(1.0, fractionalLine, static_cast<double>(Total));
	const int Requested = qBound(0, static_cast<int>(Clamped) - 1, Total - 1);
	double Fraction = qBound(0.0, Clamped - static_cast<int>(Clamped), 0.999);
	const int Line = visibleAnchorLine(Requested);
	if (Line != Requested)
		Fraction = 0.0;
	const int Height = qMax(1, m_editor->viewport()->height());
	const int Row = qMax(1, textHeight(Line));
	const int Rows = qMax(1, static_cast<int>(Send(SCI_WRAPCOUNT, Line)));
	// The line's top sits Fraction of its rows above the target height.
	const double Desired = Height * qBound(0.0, ratio, 1.0) - Fraction * Rows * Row;
	// SCI_LINESCROLL counts display rows, wrapped sub-rows included. Rows between here and the target can differ
	// in height, so the landing is measured again; the scroll limits end the loop when the target is out of reach.
	for (int Pass = 0; Pass < 3; ++Pass) {
		const sptr_t Top = Send(SCI_POINTYFROMPOSITION, 0, Send(SCI_POSITIONFROMLINE, Line));
		const int Delta = static_cast<int>(qRound((Top - Desired) / Row));
		if (Delta == 0)
			break;
		Send(SCI_LINESCROLL, 0, Delta);
	}
}

bool ScintillaQtDirectBackend::isScrolledToEnd() const
{
	if (!m_editor)
		return true;
	const sptr_t Last = qMax(0, lineCount() - 1);
	const sptr_t Bottom = Send(SCI_VISIBLEFROMDOCLINE, Last) + qMax<sptr_t>(1, Send(SCI_WRAPCOUNT, Last));
	return Send(SCI_GETFIRSTVISIBLELINE) + Send(SCI_LINESONSCREEN) >= Bottom;
}

int ScintillaQtDirectBackend::visibleAnchorLine(int line) const
{
	// A line hidden by a fold has no screen position; use the header that hides it.
	for (int Depth = 0; Depth < 128 && line > 0 && !Send(SCI_GETLINEVISIBLE, line); ++Depth) {
		const int Parent = static_cast<int>(Send(SCI_GETFOLDPARENT, line));
		if (Parent < 0 || Parent >= line)
			break;
		line = Parent;
	}
	return line;
}

bool ScintillaQtDirectBackend::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == m_editor.data() && event->type() == QEvent::Resize)
		emit viewportScrolled();
	return ITextEditorBackend::eventFilter(watched, event);
}

int ScintillaQtDirectBackend::textHeight(int line) const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_TEXTHEIGHT), line)) : 0;
}

int ScintillaQtDirectBackend::leftMarginWidth() const
{
	if (!m_editor)
		return 0;
	// 모든 마진(줄 번호, 기호 등) 폭 합산
	int total = 0;
	for (int i = 0; i < 5; ++i)
		total += static_cast<int>(m_editor->send(sciMessage(SCI_GETMARGINWIDTHN), i));
	// 텍스트 영역 왼쪽 패딩 (텍스트 시작 위치까지의 추가 간격)
	total += static_cast<int>(m_editor->send(sciMessage(SCI_GETMARGINLEFT)));
	return total;
}

int ScintillaQtDirectBackend::changeHistoryFlags() const
{
	return m_editor ? static_cast<int>(m_editor->send(sciMessage(SCI_GETCHANGEHISTORY))) : 0;
}

void ScintillaQtDirectBackend::setLineEnding(ScintillaDocument::LineEnding lineEnding, bool convertExisting)
{
	if (!m_editor)
		return;

	sptr_t eolMode = static_cast<sptr_t>(Scintilla::EndOfLine::CrLf);
	if (lineEnding == ScintillaDocument::LF)
		eolMode = static_cast<sptr_t>(Scintilla::EndOfLine::Lf);
	else if (lineEnding == ScintillaDocument::CR)
		eolMode = static_cast<sptr_t>(Scintilla::EndOfLine::Cr);

	m_editor->send(sciMessage(SCI_SETEOLMODE), eolMode);
	if (convertExisting)
		m_editor->send(sciMessage(SCI_CONVERTEOLS), eolMode);
}

bool ScintillaQtDirectBackend::applyLanguage(const QString& displayName)
{
	if (!m_editor)
		return false;

	clearLexer();

	const QString lexerKey = TextLexerRegistry::instance().lexerKeyForDisplayName(displayName);
	m_editor->send(sciMessage(kSciSetLexer), SCLEX_NULL);
	if (lexerKey.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
		m_editor->send(sciMessage(SCI_SETILEXER), 0, 0);
		m_editor->sends(sciMessage(kSciSetLexerLanguage), 0, nullptr);
		m_editor->send(sciMessage(SCI_SETPROPERTY),
			reinterpret_cast<uptr_t>("fold"),
			reinterpret_cast<sptr_t>("0"));
		writeLexerTrace(displayName, lexerKey, true);
		configureCodeFolding(false);
		applyThemeColors(m_darkTheme);
		m_editor->send(sciMessage(SCI_CLEARDOCUMENTSTYLE));
		return true;
	}

	const QByteArray lexerName = lexerKey.toUtf8();

	#if defined(MV_DIRECT_SCINTILLA_HAS_LEXILLA_LEXERS)
	m_currentLexer = CreateLexer(lexerName.constData());
	if (!m_currentLexer) {
		m_editor->send(sciMessage(SCI_SETILEXER), 0, 0);
		writeLexerTrace(displayName, lexerKey, false);
		return false;
	}

	m_editor->send(sciMessage(SCI_SETILEXER), 0, reinterpret_cast<sptr_t>(m_currentLexer));
	#else
	m_editor->send(sciMessage(SCI_SETILEXER), 0, 0);
	m_editor->sends(sciMessage(kSciSetLexerLanguage), 0, lexerName.constData());
	#endif

	m_editor->send(sciMessage(SCI_SETPROPERTY),
		reinterpret_cast<uptr_t>("fold"),
		m_codeFoldingEnabled ? reinterpret_cast<sptr_t>("1") : reinterpret_cast<sptr_t>("0"));
	m_editor->send(sciMessage(SCI_SETPROPERTY),
		reinterpret_cast<uptr_t>("fold.compact"),
		reinterpret_cast<sptr_t>("1"));
	configureCodeFolding(m_codeFoldingEnabled);

	m_currentLexerKey = lexerKey;
    m_currentLanguageName = displayName;
	setKeywordsForLexer(displayName);
	applyThemeColors(m_darkTheme);

	// Lexer replacement invalidates styling. Paint the visible range first;
	// SC_IDLESTYLING_ALL completes the rest without a synchronous full pass.
	m_editor->send(sciMessage(SCI_STARTSTYLING), 0);
	m_editor->update();
	const bool applied = m_editor->send(sciMessage(SCI_GETLEXER)) != SCLEX_NULL;
	writeLexerTrace(displayName, lexerKey, applied);
	return applied;
}

void ScintillaQtDirectBackend::configureCodeFolding(bool enabled)
{
	if (!m_editor)
		return;

	m_editor->send(sciMessage(SCI_SETMARGINTYPEN), 1, SC_MARGIN_SYMBOL);
	m_editor->send(sciMessage(SCI_SETMARGINMASKN), 1, SC_MASK_FOLDERS);
	m_editor->send(sciMessage(SCI_SETMARGINSENSITIVEN), 1, enabled ? 1 : 0);
	m_editor->send(sciMessage(SCI_SETMARGINWIDTHN), 1, enabled ? 16 : 0);
	m_editor->send(sciMessage(SCI_SETFOLDFLAGS), SC_FOLDFLAG_LINEAFTER_CONTRACTED);

	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
	m_editor->send(sciMessage(SCI_MARKERDEFINE), SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);

	const auto markerFore = static_cast<sptr_t>(0xFFFFFF);
	const auto markerBack = static_cast<sptr_t>(0x808080);
	for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; ++marker) {
		m_editor->send(sciMessage(SCI_MARKERSETFORE), marker, markerFore);
		m_editor->send(sciMessage(SCI_MARKERSETBACK), marker, markerBack);
	}
}

void ScintillaQtDirectBackend::updateLineNumberMargin(int minimumDigits)
{
	if (!m_editor)
		return;

	if (minimumDigits <= 0) {
		m_editor->send(sciMessage(SCI_SETMARGINWIDTHN), 0, 0);
		return;
	}

	const int digits = qMax(minimumDigits, QString::number(qMax(1, lineCount())).size());
	const QFontMetrics metrics(m_editorFont.resolve(m_editor->font()));
	const int width = metrics.horizontalAdvance(QString(digits, QLatin1Char('9'))) + 12;
	m_editor->send(sciMessage(SCI_SETMARGINWIDTHN), 0, width);
}

void ScintillaQtDirectBackend::configureBraceHighlightIndicators()
{
	if (!m_editor)
		return;

	auto colourToSci = [](const QColor& color) -> sptr_t {
		return static_cast<sptr_t>(color.red() | (color.green() << 8) | (color.blue() << 16));
	};

	Q_UNUSED(m_darkTheme);
	const QColor matchColor = ThemeManager::instance().color(QStringLiteral("text.braceMatch"));
	const QColor badColor = ThemeManager::instance().color(QStringLiteral("text.braceMismatch"));

	// 괄호 매칭은 글자를 덮지 않도록 채움 없는 outline indicator로 표시한다.
	m_editor->send(sciMessage(SCI_INDICSETSTYLE), kBraceHighlightIndicatorId, INDIC_STRAIGHTBOX);
	m_editor->send(sciMessage(SCI_INDICSETFORE), kBraceHighlightIndicatorId, colourToSci(matchColor));
	m_editor->send(sciMessage(SCI_INDICSETALPHA), kBraceHighlightIndicatorId, 0);
	m_editor->send(sciMessage(SCI_INDICSETOUTLINEALPHA), kBraceHighlightIndicatorId, qMax(matchColor.alpha(), 220));

	m_editor->send(sciMessage(SCI_INDICSETSTYLE), kBraceBadLightIndicatorId, INDIC_SQUIGGLE);
	m_editor->send(sciMessage(SCI_INDICSETFORE), kBraceBadLightIndicatorId, colourToSci(badColor));
	m_editor->send(sciMessage(SCI_INDICSETALPHA), kBraceBadLightIndicatorId, badColor.alpha());
	m_editor->send(sciMessage(SCI_INDICSETOUTLINEALPHA), kBraceBadLightIndicatorId, badColor.alpha());

	m_editor->send(sciMessage(SCI_BRACEHIGHLIGHTINDICATOR), 1, kBraceHighlightIndicatorId);
	m_editor->send(sciMessage(SCI_BRACEBADLIGHTINDICATOR), 1, kBraceBadLightIndicatorId);
}

void ScintillaQtDirectBackend::syncLineCountState(bool emitSignal)
{
	if (!m_editor)
		return;

	const int currentLineCount = qMax(1, lineCount());
	const bool lineCountChanged = currentLineCount != m_lastKnownLineCount;
	m_lastKnownLineCount = currentLineCount;
	updateLineNumberMargin(m_lineNumberMarginDigits);
	if (emitSignal && lineCountChanged)
		emit linesChanged();
}

void ScintillaQtDirectBackend::emitCursorAndSelectionState()
{
	if (!m_editor)
		return;

	const sptr_t position = m_editor->send(sciMessage(SCI_GETCURRENTPOS));
	int line = 0;
	int column = 0;
	positionToLineIndex(m_editor, position, line, column);
	emit cursorPositionChanged(line, column);
	emit selectionChanged();
}

void ScintillaQtDirectBackend::updateBraceHighlight()
{
	if (!m_editor)
		return;

	if (!m_braceMatchingEnabled) {
		m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), kInvalidPosition, kInvalidPosition);
		m_editor->send(sciMessage(SCI_BRACEBADLIGHT), kInvalidPosition);
		return;
	}

	const sptr_t currentPos = m_editor->send(sciMessage(SCI_GETCURRENTPOS));
	sptr_t bracePos = currentPos;
	sptr_t braceChar = m_editor->send(sciMessage(SCI_GETCHARAT), bracePos);
	if (!isBraceCharacter(braceChar) && currentPos > 0) {
		bracePos = currentPos - 1;
		braceChar = m_editor->send(sciMessage(SCI_GETCHARAT), bracePos);
	}

	if (!isBraceCharacter(braceChar)) {
		m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), kInvalidPosition, kInvalidPosition);
		m_editor->send(sciMessage(SCI_BRACEBADLIGHT), kInvalidPosition);
		return;
	}

	sptr_t matchPos = m_editor->send(sciMessage(SCI_BRACEMATCH), bracePos, 0);
	if (matchPos == kInvalidPosition)
		matchPos = m_editor->send(sciMessage(SCI_BRACEMATCHNEXT), bracePos, currentPos);
	if (matchPos == kInvalidPosition) {
		m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), kInvalidPosition, kInvalidPosition);
		m_editor->send(sciMessage(SCI_BRACEBADLIGHT), bracePos);
		return;
	}

	m_editor->send(sciMessage(SCI_BRACEBADLIGHT), kInvalidPosition);
	m_editor->send(sciMessage(SCI_BRACEHIGHLIGHT), bracePos, matchPos);
}

void ScintillaQtDirectBackend::clearLexer()
{
	if (!m_currentLexer)
		return;

	// SCI_SETILEXER에 넘긴 lexer의 수명은 Scintilla가 관리한다.
	// 백엔드는 편집기가 살아 있는 동안에만 분리 요청을 보내고,
	// 편집기가 먼저 파괴된 경우에는 포인터만 무효화한다.
	if (m_editor)
		m_editor->send(sciMessage(SCI_SETILEXER), 0, 0);
	m_currentLexer = nullptr;
	m_currentLexerKey.clear();
    m_currentLanguageName.clear();
}

void ScintillaQtDirectBackend::applyThemeColors(bool dark)
{
	if (!m_editor)
		return;

	Q_UNUSED(dark);
	auto colourToSci = [](const QColor& color) -> sptr_t {
		return static_cast<sptr_t>(color.red() | (color.green() << 8) | (color.blue() << 16));
	};

	auto& theme = ThemeManager::instance();
	const QColor bg = theme.color(QStringLiteral("text.background"));
	const QColor fg = theme.color(QStringLiteral("text.foreground"));
	const QColor marginBg = theme.color(QStringLiteral("text.marginBackground"));
	const QColor marginFg = theme.color(QStringLiteral("text.marginForeground"));
	const QColor selection = theme.color(QStringLiteral("text.selection"));
	const QColor selectionForeground = theme.color(QStringLiteral("text.selectionForeground"));
	const QColor caretLine = theme.color(QStringLiteral("text.currentLine"));
	const QColor caret = theme.color(QStringLiteral("text.caret"));
	const QColor foldMarker = theme.color(QStringLiteral("text.foldMarker"));
	const QColor indentGuide = theme.color(QStringLiteral("text.indentGuide"));

	m_editor->send(sciMessage(SCI_STYLESETFORE), STYLE_DEFAULT, colourToSci(fg));
	m_editor->send(sciMessage(SCI_STYLESETBACK), STYLE_DEFAULT, colourToSci(bg));
	m_editor->send(sciMessage(SCI_STYLECLEARALL));
	m_editor->send(sciMessage(SCI_STYLESETFORE), STYLE_INDENTGUIDE, colourToSci(indentGuide));
	m_editor->send(sciMessage(SCI_STYLESETBACK), STYLE_INDENTGUIDE, colourToSci(bg));

	m_editor->send(sciMessage(SCI_STYLESETFORE), STYLE_LINENUMBER, colourToSci(marginFg));
	m_editor->send(sciMessage(SCI_STYLESETBACK), STYLE_LINENUMBER, colourToSci(marginBg));

	m_editor->send(sciMessage(SCI_SETCARETFORE), colourToSci(caret));
	m_editor->send(sciMessage(SCI_SETADDITIONALCARETFORE), colourToSci(caret));
	m_editor->send(sciMessage(SCI_SETCARETLINEVISIBLEALWAYS), 1);
	m_editor->send(sciMessage(SCI_SETCARETLINEBACK), colourToSci(caretLine));

	// SCI_SETSELALPHA also switches the layer to OVER_TEXT, which hides text
	// with opaque theme colors. Element RGBA colors leave the layer unchanged.
	const auto selectionRgba = colourToSci(selection) | (static_cast<sptr_t>(selection.alpha()) << 24);
	const auto foregroundRgba = colourToSci(selectionForeground) | (static_cast<sptr_t>(255) << 24);
	for (int element : {SC_ELEMENT_SELECTION_BACK, SC_ELEMENT_SELECTION_ADDITIONAL_BACK,
		SC_ELEMENT_SELECTION_SECONDARY_BACK, SC_ELEMENT_SELECTION_INACTIVE_BACK,
		SC_ELEMENT_SELECTION_INACTIVE_ADDITIONAL_BACK})
		m_editor->send(sciMessage(SCI_SETELEMENTCOLOUR), element, selectionRgba);
	for (int element : {SC_ELEMENT_SELECTION_TEXT, SC_ELEMENT_SELECTION_ADDITIONAL_TEXT,
		SC_ELEMENT_SELECTION_SECONDARY_TEXT, SC_ELEMENT_SELECTION_INACTIVE_TEXT,
		SC_ELEMENT_SELECTION_INACTIVE_ADDITIONAL_TEXT})
		m_editor->send(sciMessage(SCI_SETELEMENTCOLOUR), element, foregroundRgba);
	m_editor->send(sciMessage(SCI_SETSELECTIONLAYER), SC_LAYER_UNDER_TEXT);

	m_editor->send(sciMessage(SCI_SETFOLDMARGINCOLOUR), 1, colourToSci(bg));
	m_editor->send(sciMessage(SCI_SETFOLDMARGINHICOLOUR), 1, colourToSci(bg));
	for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; ++marker) {
		m_editor->send(sciMessage(SCI_MARKERSETFORE), marker, colourToSci(bg));
		m_editor->send(sciMessage(SCI_MARKERSETBACK), marker, colourToSci(foldMarker));
	}

	// 현재 언어의 구문 강조 색상 적용
	m_darkTheme = ThemeManager::instance().currentTheme() == ThemeManager::Dark;
	configureBraceHighlightIndicators();
	applySyntaxStyles(m_darkTheme);
	m_editor->update();
}

void ScintillaQtDirectBackend::applySyntaxStyles(bool dark)
{
	if (!m_editor || m_currentLexerKey.isEmpty())
		return;

	auto colourToSci = [](unsigned int r, unsigned int g, unsigned int b) -> sptr_t {
		return static_cast<sptr_t>(r | (g << 8) | (b << 16));
	};

	// C/C++ 계열 렉서 (cpp, javascript, java, cs 등이 모두 cpp lexer를 사용)
	if (m_currentLexerKey == QStringLiteral("cpp")) {
		if (dark) {
			// VS Code Dark+ 기반 색상
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x6A, 0x99, 0x55)); // comment line
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x6A, 0x99, 0x55)); // comment doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 15, colourToSci(0x6A, 0x99, 0x55)); // comment line doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x56, 0x9C, 0xD6)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 16, colourToSci(0x4E, 0xC9, 0xB0)); // keyword2 (types)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xCE, 0x91, 0x78)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0xC5, 0x86, 0xC0)); // preprocessor
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0xD4, 0xD4, 0xD4)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 11, colourToSci(0x9C, 0xDC, 0xFE)); // identifier
			m_editor->send(sciMessage(SCI_STYLESETFORE), 19, colourToSci(0x4E, 0xC9, 0xB0)); // globalclass
		} else {
			// VS Code Light+ 기반 색상
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x00, 0x80, 0x00)); // comment line
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x00, 0x80, 0x00)); // comment doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 15, colourToSci(0x00, 0x80, 0x00)); // comment line doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 16, colourToSci(0x26, 0x7F, 0x99)); // keyword2 (types)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xA3, 0x15, 0x15)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xA3, 0x15, 0x15)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0xAF, 0x00, 0xDB)); // preprocessor
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0x1e, 0x1e, 0x1e)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 11, colourToSci(0x1e, 0x1e, 0x1e)); // identifier
			m_editor->send(sciMessage(SCI_STYLESETFORE), 19, colourToSci(0x26, 0x7F, 0x99)); // globalclass
		}
	}
	// Python 렉서
	else if (m_currentLexerKey == QStringLiteral("python")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 12, colourToSci(0x6A, 0x99, 0x55)); // comment block
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xCE, 0x91, 0x78)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xCE, 0x91, 0x78)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // triple
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xCE, 0x91, 0x78)); // triple double
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x56, 0x9C, 0xD6)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 14, colourToSci(0x4E, 0xC9, 0xB0)); // keyword2 (builtins)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x4E, 0xC9, 0xB0)); // classname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0xDC, 0xDC, 0xAA)); // defname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0xD4, 0xD4, 0xD4)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 15, colourToSci(0xD7, 0xBA, 0x7D)); // decorator
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 12, colourToSci(0x00, 0x80, 0x00)); // comment block
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xA3, 0x15, 0x15)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xA3, 0x15, 0x15)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xA3, 0x15, 0x15)); // triple
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xA3, 0x15, 0x15)); // triple double
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 14, colourToSci(0x26, 0x7F, 0x99)); // keyword2 (builtins)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x26, 0x7F, 0x99)); // classname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x79, 0x5E, 0x26)); // defname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0x1e, 0x1e, 0x1e)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 15, colourToSci(0xAF, 0x00, 0xDB)); // decorator
		}
	}
	// JSON 렉서
	else if (m_currentLexerKey == QStringLiteral("json")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x6A, 0x99, 0x55)); // line comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x6A, 0x99, 0x55)); // block comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xCE, 0x91, 0x78)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0x9C, 0xDC, 0xFE)); // propertyname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0xCE, 0x91, 0x78)); // stringeol
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xD4, 0xD4, 0xD4)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x56, 0x9C, 0xD6)); // keyword (true/false/null)
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 8,  1);
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x00, 0x80, 0x00)); // line comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x00, 0x80, 0x00)); // block comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xA3, 0x15, 0x15)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0x00, 0x16, 0x80)); // propertyname
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0xA3, 0x15, 0x15)); // stringeol
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0x1e, 0x1e, 0x1e)); // operator
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 8,  1);
		}
	}
	// XML/HTML 렉서
	else if (m_currentLexerKey == QStringLiteral("xml") || m_currentLexerKey == QStringLiteral("hypertext")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x56, 0x9C, 0xD6)); // tag
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x9C, 0xDC, 0xFE)); // attribute
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // string (attr value)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xCE, 0x91, 0x78)); // string (attr value)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 17, colourToSci(0xD4, 0xD4, 0xD4)); // CDATA
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x80, 0x00, 0x00)); // tag
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xFF, 0x00, 0x00)); // attribute
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0x00, 0x00, 0xFF)); // string (attr value)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0x00, 0x00, 0xFF)); // string (attr value)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 17, colourToSci(0x1e, 0x1e, 0x1e)); // CDATA
		}
	}
	// CSS 렉서
	else if (m_currentLexerKey == QStringLiteral("css")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0xD7, 0xBA, 0x7D)); // tag
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x9C, 0xDC, 0xFE)); // class
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x56, 0x9C, 0xD6)); // pseudoclass
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x9C, 0xDC, 0xFE)); // property (attribute)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 13, colourToSci(0xCE, 0x91, 0x78)); // value (string)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 14, colourToSci(0xB5, 0xCE, 0xA8)); // value (number)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x6A, 0x99, 0x55)); // comment
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x80, 0x00, 0x00)); // tag
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x80, 0x00, 0x00)); // class
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x80, 0x00, 0x00)); // pseudoclass
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0xFF, 0x00, 0x00)); // property
			m_editor->send(sciMessage(SCI_STYLESETFORE), 13, colourToSci(0x00, 0x00, 0xFF)); // value (string)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 14, colourToSci(0x09, 0x86, 0x58)); // value (number)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 9,  colourToSci(0x00, 0x80, 0x00)); // comment
		}
	}
	// Bash 렉서
	else if (m_currentLexerKey == QStringLiteral("bash")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0x56, 0x9C, 0xD6)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 4,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0xCE, 0x91, 0x78)); // string (single)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // string (double)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x9C, 0xDC, 0xFE)); // variable (scalar)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xD4, 0xD4, 0xD4)); // operator
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 4,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0xA3, 0x15, 0x15)); // string (single)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xA3, 0x15, 0x15)); // string (double)
			m_editor->send(sciMessage(SCI_STYLESETFORE), 8,  colourToSci(0x00, 0x16, 0x80)); // variable
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0x1e, 0x1e, 0x1e)); // operator
		}
	}
	// SQL 렉서
	else if (m_currentLexerKey == QStringLiteral("sql")) {
		if (dark) {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x6A, 0x99, 0x55)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x6A, 0x99, 0x55)); // comment line
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x6A, 0x99, 0x55)); // comment doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0xB5, 0xCE, 0xA8)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x56, 0x9C, 0xD6)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xCE, 0x91, 0x78)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xCE, 0x91, 0x78)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0xD4, 0xD4, 0xD4)); // operator
		} else {
			m_editor->send(sciMessage(SCI_STYLESETFORE), 1,  colourToSci(0x00, 0x80, 0x00)); // comment
			m_editor->send(sciMessage(SCI_STYLESETFORE), 2,  colourToSci(0x00, 0x80, 0x00)); // comment line
			m_editor->send(sciMessage(SCI_STYLESETFORE), 3,  colourToSci(0x00, 0x80, 0x00)); // comment doc
			m_editor->send(sciMessage(SCI_STYLESETFORE), 4,  colourToSci(0x09, 0x86, 0x58)); // number
			m_editor->send(sciMessage(SCI_STYLESETFORE), 5,  colourToSci(0x00, 0x00, 0xFF)); // keyword
			m_editor->send(sciMessage(SCI_STYLESETBOLD), 5,  1);
			m_editor->send(sciMessage(SCI_STYLESETFORE), 6,  colourToSci(0xA3, 0x15, 0x15)); // string
			m_editor->send(sciMessage(SCI_STYLESETFORE), 7,  colourToSci(0xA3, 0x15, 0x15)); // character
			m_editor->send(sciMessage(SCI_STYLESETFORE), 10, colourToSci(0x1e, 0x1e, 0x1e)); // operator
		}
	}

	// 사용자 테마 토큰을 최종 override로 적용한다. 기본값은 Monokai 팔레트다.
	auto colorKeyForToken = [this](const QString& genericKey) {
		const QString prefix = QStringLiteral("text.lexer.");
		if (!genericKey.startsWith(prefix))
			return genericKey;

		const QString token = genericKey.mid(prefix.size());
        if (const auto* Profile = TextLexerRegistry::instance().profileForDisplayName(m_currentLanguageName)) {
            const auto Specific = QStringLiteral("text.lexer.%1.%2").arg(Profile->languageId, token);
            if (ThemeManager::instance().hasColorOverride(Specific)) return Specific;
        }
		const QString lexerSpecific = QStringLiteral("text.lexer.%1.%2").arg(m_currentLexerKey, token);
		if (ThemeManager::instance().hasColorOverride(lexerSpecific))
			return lexerSpecific;

		if (m_currentLexerKey == QStringLiteral("hypertext")) {
			const QString xmlSpecific = QStringLiteral("text.lexer.xml.%1").arg(token);
			if (ThemeManager::instance().hasColorOverride(xmlSpecific))
				return xmlSpecific;
		}

		return genericKey;
	};
	auto themeColor = [&colorKeyForToken](const QString& key) -> sptr_t {
		const QColor color = ThemeManager::instance().color(colorKeyForToken(key));
		return static_cast<sptr_t>(color.red() | (color.green() << 8) | (color.blue() << 16));
	};
	auto setStyleFore = [this, &themeColor](int style, const QString& key) {
		m_editor->send(sciMessage(SCI_STYLESETFORE), style, themeColor(key));
	};

    // Legacy function lexers do not expose style tags. The registry contains
    // their semantic roles from LexicalStyles.iface; live metadata also covers
    // lexers added by a later Lexilla build.
    const auto* Profile = TextLexerRegistry::instance().profileForDisplayName(m_currentLanguageName);
    auto SetToken = [this, Profile, &setStyleFore](int Style, const QString& Token) {
        if (Token.isEmpty()) return;
        auto Key = QStringLiteral("text.lexer.%1").arg(Token);
        if (Profile) {
            const auto Specific = QStringLiteral("text.lexer.%1.%2").arg(Profile->languageId, Token);
            if (ThemeManager::instance().hasColorOverride(Specific)) Key = Specific;
        }
        setStyleFore(Style, Key);
    };
    if (m_currentLexer) for (int Style = 0; Style < m_currentLexer->NamedStyles(); ++Style) {
        const auto Tags = QString::fromLatin1(m_currentLexer->TagsOfStyle(Style)).split(' ', Qt::SkipEmptyParts);
        QString Token;
        if (Tags.contains("comment")) Token = "comment";
        else if (Tags.contains("string") || Tags.contains("character")) Token = "string";
        else if (Tags.contains("numeric")) Token = "number";
        else if (Tags.contains("preprocessor")) Token = "preprocessor";
        else if (Tags.contains("keyword")) Token = "keyword";
        else if (Tags.contains("operator")) Token = "operator";
        else if (Tags.contains("label")) Token = "function";
        else if (Tags.contains("identifier")) Token = "identifier";
        SetToken(Style, Token);
    }
    if (Profile) for (auto It = Profile->styles.begin(); It != Profile->styles.end(); ++It) SetToken(It.key(), It.value());

	if (m_currentLexerKey == QStringLiteral("cpp")) {
		for (int style : {1, 2, 3, 15}) setStyleFore(style, QStringLiteral("text.lexer.comment"));
		setStyleFore(4, QStringLiteral("text.lexer.number"));
		setStyleFore(5, QStringLiteral("text.lexer.keyword"));
		setStyleFore(16, QStringLiteral("text.lexer.type"));
		setStyleFore(6, QStringLiteral("text.lexer.string"));
		setStyleFore(7, QStringLiteral("text.lexer.string"));
		setStyleFore(9, QStringLiteral("text.lexer.preprocessor"));
		setStyleFore(10, QStringLiteral("text.lexer.operator"));
		setStyleFore(11, QStringLiteral("text.lexer.identifier"));
		setStyleFore(19, QStringLiteral("text.lexer.function"));
	} else if (m_currentLexerKey == QStringLiteral("python")) {
		for (int style : {1, 12}) setStyleFore(style, QStringLiteral("text.lexer.comment"));
		setStyleFore(2, QStringLiteral("text.lexer.number"));
		for (int style : {3, 4, 6, 7}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(5, QStringLiteral("text.lexer.keyword"));
		setStyleFore(14, QStringLiteral("text.lexer.type"));
		setStyleFore(8, QStringLiteral("text.lexer.function"));
		setStyleFore(9, QStringLiteral("text.lexer.function"));
		setStyleFore(10, QStringLiteral("text.lexer.operator"));
		setStyleFore(15, QStringLiteral("text.lexer.preprocessor"));
	} else if (m_currentLexerKey == QStringLiteral("json")) {
		for (int style : {1, 2}) setStyleFore(style, QStringLiteral("text.lexer.comment"));
		setStyleFore(3, QStringLiteral("text.lexer.number"));
		for (int style : {4, 5}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(6, QStringLiteral("text.lexer.function"));
		setStyleFore(7, QStringLiteral("text.lexer.operator"));
		setStyleFore(8, QStringLiteral("text.lexer.keyword"));
	} else if (m_currentLexerKey == QStringLiteral("xml") || m_currentLexerKey == QStringLiteral("hypertext")) {
		setStyleFore(1, QStringLiteral("text.lexer.keyword"));
		setStyleFore(3, QStringLiteral("text.lexer.preprocessor"));
		for (int style : {6, 7}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(9, QStringLiteral("text.lexer.comment"));
		setStyleFore(17, QStringLiteral("text.lexer.identifier"));
	} else if (m_currentLexerKey == QStringLiteral("css")) {
		for (int style : {1, 2, 5}) setStyleFore(style, QStringLiteral("text.lexer.keyword"));
		setStyleFore(8, QStringLiteral("text.lexer.preprocessor"));
		setStyleFore(13, QStringLiteral("text.lexer.string"));
		setStyleFore(14, QStringLiteral("text.lexer.number"));
		setStyleFore(9, QStringLiteral("text.lexer.comment"));
	} else if (m_currentLexerKey == QStringLiteral("bash")) {
		setStyleFore(2, QStringLiteral("text.lexer.comment"));
		setStyleFore(3, QStringLiteral("text.lexer.number"));
		setStyleFore(4, QStringLiteral("text.lexer.keyword"));
		for (int style : {5, 6}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(8, QStringLiteral("text.lexer.variable"));
		setStyleFore(7, QStringLiteral("text.lexer.operator"));
	} else if (m_currentLexerKey == QStringLiteral("sql")) {
		for (int style : {1, 2, 3}) setStyleFore(style, QStringLiteral("text.lexer.comment"));
		setStyleFore(4, QStringLiteral("text.lexer.number"));
		setStyleFore(5, QStringLiteral("text.lexer.keyword"));
		for (int style : {6, 7}) setStyleFore(style, QStringLiteral("text.lexer.string"));
		setStyleFore(10, QStringLiteral("text.lexer.operator"));
	}
}

void ScintillaQtDirectBackend::setKeywordsForLexer(const QString& displayName)
{
	if (!m_editor)
		return;

    const auto* Profile = TextLexerRegistry::instance().profileForDisplayName(displayName);
    const auto lexerKey = Profile ? Profile->lexerKey : QString{};
    for (int Index = 0; Index < 9; ++Index) {
        const auto Words = Profile ? Profile->keywords.value(Index).toUtf8() : QByteArray{};
        m_editor->sends(sciMessage(SCI_SETKEYWORDS), Index, Words.constData());
    }
	// Preserve the shared modern C++/Python lists. Other cpp dialects use
    // their own keywords rather than inheriting the C++ reserved words.
	if (Profile && Profile->languageId == QStringLiteral("cpp")) {
		m_editor->send(sciMessage(SCI_SETKEYWORDS), 0,
			reinterpret_cast<sptr_t>(hc::lexer::cppKeywords));
		m_editor->send(sciMessage(SCI_SETKEYWORDS), 1,
			reinterpret_cast<sptr_t>(hc::lexer::cppTypes));
	}
	else if (lexerKey == QStringLiteral("python")) {
		m_editor->send(sciMessage(SCI_SETKEYWORDS), 0,
			reinterpret_cast<sptr_t>(hc::lexer::pythonKeywords));
		m_editor->send(sciMessage(SCI_SETKEYWORDS), 1,
			reinterpret_cast<sptr_t>(hc::lexer::pythonBuiltins));
	}
	else if (lexerKey == QStringLiteral("sql")) {
		static const char sqlKeywords[] =
			"select from where insert into values update set delete create table alter drop "
			"index view trigger procedure function begin end if else then case when null "
			"not and or in exists between like is join inner outer left right cross on "
			"as order by group having distinct count sum avg min max cast union all top "
			"limit offset declare varchar int integer char text float double date datetime "
			"primary key foreign references default constraint unique check grant revoke "
			"commit rollback transaction exec execute";

		m_editor->send(sciMessage(SCI_SETKEYWORDS), 0,
			reinterpret_cast<sptr_t>(sqlKeywords));
	}
	else if (lexerKey == QStringLiteral("bash")) {
		static const char bashKeywords[] =
			"if then else elif fi case esac for while until do done in function select "
			"time coproc break continue return exit shift export readonly declare local "
			"typeset unset eval exec source alias unalias trap set shopt "
			"echo printf read cd pwd ls cat grep find test true false";

		m_editor->send(sciMessage(SCI_SETKEYWORDS), 0,
			reinterpret_cast<sptr_t>(bashKeywords));
	}
}


int ScintillaQtDirectBackend::CharacterCount() const
{
    if (!m_editor) return 0;
    if (characterCount_ < 0) characterCount_ = static_cast<int>(Send(SCI_COUNTCODEUNITS, 0, Send(SCI_GETLENGTH)));
    return characterCount_;
}
int ScintillaQtDirectBackend::SelectedCharacterCount() const
{
    return m_editor ? static_cast<int>(Send(SCI_COUNTCODEUNITS, Send(SCI_GETSELECTIONSTART), Send(SCI_GETSELECTIONEND))) : 0;
}
QString ScintillaQtDirectBackend::WordAtCaret() const
{
    if (!m_editor) return {};
    const auto Caret = Send(SCI_GETCURRENTPOS);
    const auto Start = Send(SCI_WORDSTARTPOSITION, Caret, 1);
    const auto End = Send(SCI_WORDENDPOSITION, Caret, 1);
    if (End <= Start) return {};
    QByteArray Bytes(End - Start + 1, Qt::Uninitialized);
    Sci_TextRangeFull Range{}; Range.chrg.cpMin = Start; Range.chrg.cpMax = End; Range.lpstrText = Bytes.data();
    Send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<qintptr>(&Range));
    return QString::fromUtf8(Bytes.constData(), End - Start);
}
void ScintillaQtDirectBackend::FoldAll(bool Contract)
{
    if (m_editor && m_codeFoldingEnabled) {
        Send(SCI_COLOURISE, 0, -1);
        Send(SCI_FOLDALL, Contract ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
        emit viewportScrolled();
    }
}
