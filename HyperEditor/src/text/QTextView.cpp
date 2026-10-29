#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "UniqueLibs/qtMessageDialog.hpp"
#include <Scintilla.h>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QCompleter>
#include "text/ColumnRulerWidget.h"
#include "text/FindReplaceWidget.h"
#include "text/TextSaveDialog.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "thirdparty/scintilla-qt/PlatQtTextRendering.hpp"
#include "core/ToolbarIcons.h"
#include "text/ITextEditorBackend.h"
#include "text/TextEditorBackendFactory.h"
#include "text/TextLexerRegistry.h"
#include "text/TextShadowBackupStore.h"
#include "utils/EncodingDetector.h"
#include "utils/FileLoadHelper.h"

#include <QAbstractScrollArea>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QComboBox>
#include <QFontDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include "UniqueLibs/qtDialogFrame.hpp"
#include <fmstyle/ThemeManager.h>
#include <QCheckBox>
#include <QLabel>
#include <QSpinBox>
#include <QLineEdit>
#include <QAction>
#include <QPushButton>
#include <QScrollBar>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QThreadPool>
#include <QUuid>

#include <limits>
#include <utility>

namespace {

struct TextOpenResult {
    bool success = false;
    QString filePath;
    QString errorMessage;
    QString text;
    TextFileSession session;
    TextFileSession::OpenMode openMode = TextFileSession::OpenMode::Full;
};

bool matchesCurrentSelection(ITextEditorBackend* editor,
                             const QString& findText,
                             const bool regex,
                             const bool caseSensitive)
{
    if (!editor || regex || !editor->hasSelectedText())
        return false;

    return editor->selectedText().compare(findText,
                                          caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0;
}

void moveCursorPastSelection(ITextEditorBackend* editor, bool forward)
{
    if (!editor)
        return;

    int lineFrom = 0;
    int indexFrom = 0;
    int lineTo = 0;
    int indexTo = 0;
    editor->getSelectionRange(lineFrom, indexFrom, lineTo, indexTo);
    editor->setCursorPosition(forward ? lineTo : lineFrom,
                              forward ? indexTo : indexFrom);
}

void moveCursorToSearchBoundary(ITextEditorBackend* editor, bool forward)
{
    if (!editor)
        return;

    if (forward) {
        editor->setCursorPosition(0, 0);
        return;
    }

    editor->setCursorPosition(qMax(0, editor->lineCount() - 1), std::numeric_limits<int>::max());
}

QString preferredSaveEncoding()
{
    AppSettings settings;
    const QString encoding = settings.value("textView/saveEncoding", QStringLiteral("UTF-8")).toString().trimmed();
    return encoding.isEmpty() ? QStringLiteral("UTF-8") : encoding;
}

TextSaveDialog::BomMode preferredSaveBomMode()
{
    AppSettings settings;
    const int mode = qBound(0, settings.value("textView/saveBomMode", 1).toInt(), 2);
    return static_cast<TextSaveDialog::BomMode>(mode);
}

bool includeBomForPreferredSaveMode()
{
    return preferredSaveBomMode() != TextSaveDialog::BomMode::Exclude;
}

} // namespace

// ═══════════════════════════════════════════════════════════
// 생성 / 소멸
// ═══════════════════════════════════════════════════════════
QTextView::QTextView(QWidget* parent)
    : QBaseView(parent)
{
    m_editorSettings = ScintillaEditorSettings::standard();
    loadPersistedEditorPreferences();

    // 눈금자 위젯 (에디터 위에 배치)
    m_ruler = new ColumnRulerWidget(this); m_ruler->setObjectName("columnRuler");
    if (layout())
        layout()->addWidget(m_ruler);

    // 설정 파일에서 눈금자 표시 여부 읽기 (기본: on)
    AppSettings settings;
    const bool rulerVisible = settings.value("textView/showRuler", true).toBool();
    m_ruler->setVisible(rulerVisible);
    m_hotExitUntitledId = newHotExitUntitledId();
    applyHotExitSettingsFromPreferences();

    m_hotExitTimer = new QTimer(this);
    m_hotExitTimer->setInterval(2000);
    m_hotExitTimer->setSingleShot(false);
    connect(m_hotExitTimer, &QTimer::timeout, this, &QTextView::onHotExitBackupTimer);

    ensureEditorBackend();
    m_hotExitEnabled = false;
    connect(m_editor, &ITextEditorBackend::textChanged, this, &QTextView::ContentChanged);

    // 찾기/바꾸기 위젯 (눈금자 아래, 에디터 위에 배치)
    setupFindWidget();

    // 단축키 액션 등록
    auto* findAction = new QAction(this);
    findAction->setObjectName(QStringLiteral("text.find"));
    // The workspace shortcut router owns this binding.
    connect(findAction, &QAction::triggered, this, [this]{
        if (m_findWidget && m_findWidget->isVisible() && !m_findWidget->isReplaceMode()) {
            hideFindBar();
        } else {
            showFindBar(false);
        }
    });
    addAction(findAction);

    auto* replaceAction = new QAction(this);
    replaceAction->setObjectName(QStringLiteral("text.replace"));
    // The workspace shortcut router owns this binding.
    connect(replaceAction, &QAction::triggered, this, [this]{
        if (m_findWidget && m_findWidget->isVisible() && m_findWidget->isReplaceMode()) {
            hideFindBar();
        } else {
            showFindBar(true);
        }
    });
    addAction(replaceAction);

    auto* findNextAction = new QAction(this);
    findNextAction->setObjectName(QStringLiteral("text.findNext"));
    // The workspace shortcut router owns this binding.
    connect(findNextAction, &QAction::triggered, this, &QTextView::performFindNext);
    addAction(findNextAction);

    auto* findPrevAction = new QAction(this);
    findPrevAction->setObjectName(QStringLiteral("text.findPrev"));
    // The workspace shortcut router owns this binding.
    connect(findPrevAction, &QAction::triggered, this, &QTextView::performFindPrev);
    addAction(findPrevAction);

    auto* goToLineAction = new QAction(this);
    goToLineAction->setObjectName(QStringLiteral("text.goToLine"));
    // The workspace shortcut router owns this binding.
    connect(goToLineAction, &QAction::triggered, this, &QTextView::showGoToLineDialog);
    addAction(goToLineAction);
}

QTextView::~QTextView() = default;

// ═══════════════════════════════════════════════════════════
// 파일 관리
// ═══════════════════════════════════════════════════════════
bool QTextView::openFile(const QString& filePath)
{
    Q_UNUSED(filePath);
    return false;
}

bool QTextView::saveFile(const QString& filePath)
{
    Q_UNUSED(filePath);
    return false;
}

bool QTextView::saveWithEncoding(const QString& filePath, const QString& encoding, bool includeBom)
{
    Q_UNUSED(filePath); Q_UNUSED(encoding); Q_UNUSED(includeBom); emit SaveRequested(); return false;
}

bool QTextView::saveFileAs()
{
    emit SaveRequested(); return false;
}

void QTextView::closeFile()
{
    m_openRequestId.fetch_add(1);
    endLoading();
    const bool shuttingDown = isShuttingDown();
    clearCurrentMatchHighlight();
    clearExcludedRanges();
    const bool keepHotExitBackup = shouldUseHotExitForCurrentFile() && isModified();
    if (keepHotExitBackup)
        writeHotExitBackupNow(true);
    else
        discardHotExitBackup();
    if (!shuttingDown && m_editor)
        m_editor->clear();
    m_fileSession.clear();
    m_filePath.clear();
    m_encoding.clear();
    m_detectedEncoding.clear();
    m_document = ScintillaDocument();
    m_editorSettings = ScintillaEditorSettings::standard();
    applyPersistedEditorPreferences(m_editorSettings);
    if (!shuttingDown) {
        applyEditorSettings();
        if (m_editor)
            m_editor->applyLanguage(QStringLiteral("None"));
    }
    m_cachedLineCount = 1;
    m_cachedCurrentLine = 1;
    m_cachedCurrentColumn = 1;
    m_hotExitDirty = false;
    m_hotExitUntitledId = newHotExitUntitledId();
    updateMetrics();
    emit fileClosed();
    emit statusChanged();
    updateCopyAvailability();
}

bool QTextView::isModified() const { return m_editor && m_editor->isModified(); }

QStringList QTextView::supportedExtensions() const
{
    return TextLexerRegistry::instance().supportedExtensions();
}

bool QTextView::opensFileAsynchronously() const
{
    return true;
}

// ═══════════════════════════════════════════════════════════
// 인코딩
// ═══════════════════════════════════════════════════════════
QString QTextView::detectedEncoding() const  { return m_detectedEncoding; }
QString QTextView::currentEncoding() const   { return m_encoding; }

QString QTextView::currentEncodingDisplayName() const
{
    const QString normalized = m_encoding.trimmed().toUpper();
    if (normalized == QStringLiteral("UTF-8")
        || normalized == QStringLiteral("UTF-16")
        || normalized == QStringLiteral("UTF-16LE")
        || normalized == QStringLiteral("UTF-16BE")) {
        return tr("%1 (%2)")
            .arg(m_encoding,
                 m_fileSession.hasBom() ? tr("BOM 있음") : tr("BOM 없음"));
    }

    return m_encoding;
}

void QTextView::reloadWithEncoding(const QString& encoding)
{
    emit EncodingRequested(encoding);
}

QStringList QTextView::availableEncodings() { return EncodingDetector::availableEncodings(); }

bool QTextView::saveWithEncodingDialog(const QString& initialFilePath)
{
    if (!m_editor || isLoading())
        return false;

    if (m_fileSession.isTruncated()) {
        hc::ui::MessageDialog::information(this,
                                 tr("저장 불가"),
                                 tr("제한 모드로 연 파일은 현재 저장할 수 없습니다. 파일을 전체 로드로 다시 열어 주세요."));
        return false;
    }

    TextSaveDialog dialog(this);
    dialog.setWindowTitle(tr("다른 이름으로 저장"));
    dialog.setDialogFilters({tr("텍스트 파일 (*.*)")}, tr("텍스트 파일 (*.*)"));
    dialog.setInitialFilePath(initialFilePath.isEmpty() ? tr("새 텍스트 파일.txt") : initialFilePath);
    dialog.setEncodingOptions(availableEncodings(),
                              preferredSaveEncoding(),
                              currentFileHasBom(),
                              preferredSaveBomMode());

    if (dialog.exec() != QDialog::Accepted)
        return false;

    const auto options = dialog.selectedOptions();
    if (options.filePath.isEmpty())
        return false;

    return saveWithEncoding(options.filePath,
                            options.encoding.isEmpty() ? QStringLiteral("UTF-8") : options.encoding,
                            dialog.selectedBomIncluded());
}

bool QTextView::currentFileHasBom() const
{
    return m_fileSession.hasBom();
}

// ═══════════════════════════════════════════════════════════
// 글꼴
// ═══════════════════════════════════════════════════════════
void QTextView::setEditorFont(const QFont& font)
{
    if (!m_editor)
        return;

    m_editor->setEditorFont(font);
    applyEditorSettings();
    m_editor->applyLanguage(m_document.language());
    m_editor->applyThemeColors(currentTheme() == Dark);
    if (m_ruler)
        m_ruler->setEditorFont(font);

    // Applying a theme or a preference draft must never persist a font.
}

QFont QTextView::editorFont() const { return m_editor ? m_editor->editorFont() : QFont(); }

double QTextView::lineSpacingScale() const
{
    return m_editorSettings.lineSpacingScale;
}

void QTextView::setLineSpacingScale(double scale)
{
    scale = qBound(1.0, scale, 3.0);
    if (qFuzzyCompare(m_editorSettings.lineSpacingScale, scale))
        return;

    m_editorSettings.lineSpacingScale = scale;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue("textView/lineSpacing", scale);
}

// ═══════════════════════════════════════════════════════════
// 구문 강조
// ═══════════════════════════════════════════════════════════
void QTextView::setLanguage(const QString& language)
{
    const QString normalizedLanguage = language.trimmed().isEmpty() ? QStringLiteral("None") : language.trimmed();
    m_document.setLanguage(normalizedLanguage);
    if (m_editor)
        m_editor->applyLanguage(normalizedLanguage);

    emit languageChanged(normalizedLanguage);
    emit statusChanged();
}

QString QTextView::currentLanguage() const { return m_document.language(); }

void QTextView::autoDetectLanguage()
{
    setLanguage(TextLexerRegistry::instance().languageForFileName(m_filePath));
}

// ═══════════════════════════════════════════════════════════
// 편집
// ═══════════════════════════════════════════════════════════
void QTextView::setReadOnly(bool on)
{
    if (!m_editor)
        return;

    if (m_editor->isReadOnly() == on)
        return;

    m_editor->setReadOnly(on);
    emit statusChanged();
}
bool QTextView::isReadOnly() const   { return m_editor && m_editor->isReadOnly(); }
bool QTextView::isLimitedPreviewMode() const { return m_document.isPreviewOnly(); }
bool QTextView::isContentTruncated() const { return m_document.isTruncated(); }

QString QTextView::contentLoadModeText() const
{
    if (m_document.isPreviewOnly()) {
        return m_document.isTruncated()
            ? tr("제한 모드 (부분 로드)")
            : tr("제한 모드");
    }

    return tr("전체 로드");
}

bool QTextView::isHotExitEnabled() const
{
    return m_hotExitEnabled;
}

void QTextView::setHotExitEnabled(bool enabled)
{
    if (m_hotExitEnabled == enabled) {
        if (!enabled)
            discardHotExitBackup();
        return;
    }

    m_hotExitEnabled = enabled;
    if (!m_hotExitEnabled) {
        if (m_hotExitTimer)
            m_hotExitTimer->stop();
        m_hotExitDirty = false;
        discardHotExitBackup();
        return;
    }

    if (isModified())
        scheduleHotExitBackup();
}

bool QTextView::openHotExitBackup(const QString& untitledId)
{
    if (!m_hotExitEnabled || !ensureEditorBackend())
        return false;

    TextShadowBackupStore::Snapshot snapshot;
    if (!TextShadowBackupStore::loadUntitledSnapshot(untitledId, &snapshot) || !snapshot.isUntitled)
        return false;

    closeFile();
    m_hotExitUntitledId = snapshot.untitledId.trimmed().isEmpty()
        ? newHotExitUntitledId()
        : snapshot.untitledId.trimmed();
    m_filePath.clear();
    setDisplayTitle(snapshot.displayTitle.trimmed().isEmpty() ? tr("제목없음") : snapshot.displayTitle.trimmed());
    emit titleChanged(title());

    m_fileSession.clear();
    m_document = ScintillaDocument();
    m_document.setPreviewOnly(false);
    m_document.setTruncated(false);
    m_document.setLineEnding(snapshot.lineEnding);
    m_encoding = snapshot.encoding.trimmed().isEmpty() ? preferredSaveEncoding() : snapshot.encoding.trimmed();
    m_detectedEncoding = snapshot.detectedEncoding;

    m_editorSettings = ScintillaEditorSettings::standard();
    applyPersistedEditorPreferences(m_editorSettings);
    applyEditorSettings();
    m_editor->setText(snapshot.text);
    m_editor->setModified(true);
    m_editor->restoreViewState(snapshot.caretPosition, snapshot.firstVisibleLine);
    m_hotExitDirty = true;
    updateMetrics();
    autoDetectLanguage();
    emit encodingChanged(m_encoding);
    emit statusChanged();
    updateCopyAvailability();
    return true;
}

void QTextView::flushHotExitBackup()
{
    if (shouldUseHotExitForCurrentFile() && isModified())
        writeHotExitBackupNow(true);
}

void QTextView::discardHotExitBackup()
{
    if (!m_filePath.isEmpty())
        TextShadowBackupStore::deleteSnapshot(m_filePath);
    else if (!m_hotExitUntitledId.isEmpty())
        TextShadowBackupStore::deleteUntitledSnapshot(m_hotExitUntitledId);
    m_hotExitDirty = false;
}

bool QTextView::shouldUseHotExitForCurrentFile() const
{
    return m_hotExitEnabled
        && m_editor
        && !m_document.isPreviewOnly()
        && !m_document.isTruncated()
        && !isLoading();
}

void QTextView::scheduleHotExitBackup()
{
    if (!shouldUseHotExitForCurrentFile())
        return;

    m_hotExitDirty = true;
    if (m_hotExitTimer && !m_hotExitTimer->isActive())
        m_hotExitTimer->start();
}

void QTextView::writeHotExitBackupNow(bool synchronous)
{
    Q_UNUSED(synchronous); // The workspace owns recovery and lifetime fencing.
}

void QTextView::onHotExitBackupTimer()
{
    if (!m_hotExitDirty) {
        if (m_hotExitTimer)
            m_hotExitTimer->stop();
        return;
    }

    if (!isModified()) {
        discardHotExitBackup();
        if (m_hotExitTimer)
            m_hotExitTimer->stop();
        return;
    }

    writeHotExitBackupNow();
}

void QTextView::applyHotExitSettingsFromPreferences()
{
    m_hotExitEnabled = false;
}

bool QTextView::canCopy() const
{
    return m_editor && m_editor->hasSelectedText();
}

bool QTextView::copySelection()
{
    if(ClipboardRestricted())return false;
    if (!canCopy())
        return false;

    m_editor->copy();
    return true;
}

void QTextView::goToLine(int line)
{
    goToPosition(line, 1);
}

void QTextView::goToPosition(int line, int column)
{
    if (!m_editor)
        return;

    const int safeLine = qBound(1, line, lineCount());
    const int safeColumn = qMax(1, column);
    m_editor->setCursorPosition(safeLine - 1, safeColumn - 1);
}

int  QTextView::characterCount() const
{
    return m_editor ? m_editor->CharacterCount() : 0;
}

int QTextView::tabWidth() const
{
    return m_editorSettings.tabWidth;
}

void QTextView::setTabWidth(int width)
{
    width = qBound(1, width, 16);
    if (m_editorSettings.tabWidth == width)
        return;
    m_editorSettings.tabWidth = width;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue("textView/tabWidth", width);
}

bool QTextView::useTabs() const
{
    return m_editorSettings.useTabs;
}

void QTextView::setUseTabs(bool use)
{
    if (m_editorSettings.useTabs == use)
        return;
    m_editorSettings.useTabs = use;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue("textView/useTabs", use);
    emit statusChanged();
}

bool QTextView::indentationGuidesVisible() const
{
    return m_editorSettings.showIndentationGuides;
}

void QTextView::setIndentationGuidesVisible(bool visible)
{
    if (m_editorSettings.showIndentationGuides == visible)
        return;
    m_editorSettings.showIndentationGuides = visible;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue("textView/showIndentationGuides", visible);
}

ScintillaEditorSettings::IndentGuideStyle QTextView::indentGuideStyle() const
{
    return m_editorSettings.indentGuideStyle;
}

void QTextView::setIndentGuideStyle(ScintillaEditorSettings::IndentGuideStyle style)
{
    style = static_cast<ScintillaEditorSettings::IndentGuideStyle>(
        qBound(static_cast<int>(ScintillaEditorSettings::IndentGuideReal),
               static_cast<int>(style),
               static_cast<int>(ScintillaEditorSettings::IndentGuideLookBoth)));
    if (m_editorSettings.indentGuideStyle == style)
        return;
    m_editorSettings.indentGuideStyle = style;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue("textView/indentGuideStyle", static_cast<int>(style));
}

int  QTextView::selectedCharacterCount() const
{
    return m_editor ? m_editor->SelectedCharacterCount() : 0;
}

int  QTextView::lineCount() const    { return qMax(1, m_cachedLineCount); }
int  QTextView::currentLine() const  { return qMax(1, m_cachedCurrentLine); }
int  QTextView::currentColumn() const { return qMax(1, m_cachedCurrentColumn); }

// ═══════════════════════════════════════════════════════════
// 검색 / 치환
// ═══════════════════════════════════════════════════════════
void QTextView::findText(const QString& text,
                         bool regex,
                         bool caseSensitive,
                         bool wholeWords,
                         bool forward)
{
    if (!m_editor || text.isEmpty())
        return;

    m_searchOptions.regex = regex;
    m_searchOptions.caseSensitive = caseSensitive;
    m_searchOptions.wholeWords = wholeWords;
    m_searchOptions.forward = forward;

    m_editor->findFirst(text, regex, caseSensitive, wholeWords, true, forward);
}

void QTextView::replaceText(const QString& find,
                            const QString& replace,
                            bool regex,
                            bool caseSensitive,
                            bool wholeWords,
                            bool forward)
{
    if (!m_editor || find.isEmpty())
        return;

    m_searchOptions.regex = regex;
    m_searchOptions.caseSensitive = caseSensitive;
    m_searchOptions.wholeWords = wholeWords;
    m_searchOptions.forward = forward;

    if (matchesCurrentSelection(m_editor, find, regex, caseSensitive)) {
        m_editor->replace(replace);
        moveCursorPastSelection(m_editor, forward);
        return;
    }

    if (m_editor->findFirst(find, regex, caseSensitive, wholeWords, true, forward)) {
        m_editor->replace(replace);
        moveCursorPastSelection(m_editor, forward);
    }
}

void QTextView::replaceAll(const QString& find,
                           const QString& replace,
                           bool regex,
                           bool caseSensitive,
                           bool wholeWords,
                           bool forward)
{
    Q_UNUSED(forward);if(!m_editor||find.isEmpty()||m_editor->isReadOnly())return;
    auto* Backend=static_cast<ScintillaQtDirectBackend*>(m_editor.data());
    QSignalBlocker Signals(this);Backend->Send(SCI_BEGINUNDOACTION);
    const int Flags=(regex?(SCFIND_REGEXP|SCFIND_CXX11REGEX):0)|(caseSensitive?SCFIND_MATCHCASE:0)|(wholeWords?SCFIND_WHOLEWORD:0);
    int Position=0;
    while(Position<=m_editor->documentLength()){
        m_editor->setSearchTargetRange(Position,m_editor->documentLength());
        const int Found=m_editor->searchInTarget(find,Flags);if(Found<0)break;
        const int End=m_editor->targetEnd();m_editor->replaceInTarget(replace);Position=m_editor->targetEnd();
        if(End==Found){if(Position>=m_editor->documentLength())break;Position=int(Backend->Send(SCI_POSITIONAFTER,Position));}
    }
    Backend->Send(SCI_ENDUNDOACTION);Signals.unblock();emit ContentChanged();
}

QTextView::LineEnding QTextView::detectedLineEnding() const { return fromDocumentLineEnding(m_document.lineEnding()); }

void QTextView::setLineEnding(LineEnding e)
{
    if (fromDocumentLineEnding(m_document.lineEnding()) == e)
        return;

    m_document.setLineEnding(toDocumentLineEnding(e));
    if (!m_editor)
        return;

    m_editor->setLineEnding(toDocumentLineEnding(e), true);
    m_editor->setModified(true);
    emit statusChanged();
}

bool QTextView::isWordWrapEnabled() const { return m_editorSettings.wrapMode != ScintillaEditorSettings::WrapNone; }
int QTextView::WordWrapMode() const { return m_editorSettings.wrapMode; }
void QTextView::setWordWrap(bool Enabled) { SetWordWrapMode(Enabled ? lastWrapMode_ : 0); }
void QTextView::SetWordWrapMode(int Mode)
{
    Mode = qBound(0, Mode, 3);
    if (WordWrapMode() == Mode) return;
    m_editorSettings.wrapMode = static_cast<ScintillaEditorSettings::WrapMode>(Mode);
    if (Mode) lastWrapMode_ = Mode;
    applyEditorSettings();
    AppSettings Settings;
    Settings.setValue("textView/wordWrapMode", Mode);
    Settings.setValue("textView/wordWrapLastMode", lastWrapMode_);
    Settings.setValue("textView/wordWrap", Mode != 0);
}
void QTextView::FoldAll(bool Contract) { if (m_editor && m_editorSettings.showCodeFolding) m_editor->FoldAll(Contract); }

ScintillaEditorSettings::FontRenderingMode QTextView::fontRenderingMode() const
{
    return m_editorSettings.fontRendering;
}

void QTextView::setFontRenderingMode(ScintillaEditorSettings::FontRenderingMode mode)
{
    if (m_editorSettings.fontRendering == mode)
        return;
    m_editorSettings.fontRendering = mode;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue("textView/fontRendering", static_cast<int>(mode));
}

bool QTextView::isRulerVisible() const
{
    return m_ruler && m_ruler->isVisible();
}

void QTextView::setRulerVisible(bool visible)
{
    if (m_ruler)
        m_ruler->setVisible(visible);

    AppSettings settings;
    settings.setValue("textView/showRuler", visible);
}

bool QTextView::isWhitespaceVisible() const
{
    return m_editorSettings.showWhitespace;
}

void QTextView::setWhitespaceVisible(bool visible)
{
    if (m_editorSettings.showWhitespace == visible)
        return;
    m_editorSettings.showWhitespace = visible;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue("textView/showWhitespace", visible);
}

bool QTextView::isCodeFoldingEnabled() const
{
    return m_editorSettings.showCodeFolding;
}

void QTextView::setCodeFoldingEnabled(bool enabled)
{
    if (m_editorSettings.showCodeFolding == enabled)
        return;

    m_editorSettings.showCodeFolding = enabled;
    applyEditorSettings();
    if (m_editor)
        m_editor->applyLanguage(m_document.language());
    if (m_ruler && m_editor)
        m_ruler->setLeftMarginWidth(m_editor->leftMarginWidth());

    AppSettings settings;
    settings.setValue("textView/showCodeFolding", enabled);
}

bool QTextView::isBraceHighlightEnabled() const
{
    return m_editorSettings.braceMatching;
}

void QTextView::setBraceHighlightEnabled(bool enabled)
{
    if (m_editorSettings.braceMatching == enabled)
        return;

    m_editorSettings.braceMatching = enabled;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue("textView/braceHighlight", enabled);
}

ScintillaEditorSettings::ChangeHistoryMode QTextView::changeHistoryMode() const
{
    if (!m_editor)
        return m_editorSettings.changeHistoryMode;

    const int flags = m_editor->changeHistoryFlags();
    const bool hasMarkers = (flags & 2) != 0;
    const bool hasIndicators = (flags & 4) != 0;
    if (hasMarkers && hasIndicators)
        return ScintillaEditorSettings::ChangeHistoryBoth;
    if (hasMarkers)
        return ScintillaEditorSettings::ChangeHistoryMarkers;
    if (hasIndicators)
        return ScintillaEditorSettings::ChangeHistoryIndicators;
    return ScintillaEditorSettings::ChangeHistoryOff;
}

void QTextView::setChangeHistoryMode(ScintillaEditorSettings::ChangeHistoryMode mode)
{
    if (m_editorSettings.changeHistoryMode == mode && (!m_editor || changeHistoryMode() == mode))
        return;

    m_editorSettings.changeHistoryMode = mode;
    applyEditorSettings();

    AppSettings settings;
    settings.setValue("textView/changeHistoryMode", static_cast<int>(mode));
}

void QTextView::setTheme(Theme theme)
{
    QBaseView::setTheme(theme);
    if (m_editor) {
        m_editor->applyThemeColors(theme == Dark);
        updateSearchIndicatorColors();
    }
}

// ═══════════════════════════════════════════════════════════
// 인쇄용 렌더링
// ═══════════════════════════════════════════════════════════
void QTextView::renderPage(QPainter* painter, int /*pageIndex*/, const QRectF& rect)
{
    // 단순 텍스트 인쇄
    painter->drawText(rect, Qt::TextWordWrap, m_editor ? m_editor->text() : QString());
}

int QTextView::pageCount() const { return 1; }

// ═══════════════════════════════════════════════════════════
// 도구모음
// ═══════════════════════════════════════════════════════════
QToolBar* QTextView::createToolBar()
{
    auto* tb = QBaseView::createToolBar();
    if (!m_editor)
        return tb;

    // 언어 선택
    auto* langCombo = new QComboBox(tb);
    langCombo->setObjectName("textLanguage");
    langCombo->setMaxVisibleItems(24);
    langCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    langCombo->setMinimumContentsLength(12);
    langCombo->setEditable(true);
    langCombo->setInsertPolicy(QComboBox::NoInsert);
    langCombo->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    langCombo->addItems(TextLexerRegistry::instance().displayNames());
    langCombo->setCurrentText(m_document.language());
    connect(langCombo, &QComboBox::activated, this, [this, langCombo] { setLanguage(langCombo->currentText()); });
    connect(this, &QTextView::languageChanged, langCombo, [langCombo](const QString& Language) {
        const QSignalBlocker Block(langCombo); langCombo->setCurrentText(Language);
    });
    tb->addWidget(new QLabel(tr(" 언어: "), tb));
    tb->addWidget(langCombo);

    tb->addSeparator();

    // 인코딩 선택
    auto* encCombo = new QComboBox(tb);
    encCombo->addItems(availableEncodings());
    encCombo->setCurrentText(m_encoding);
    connect(encCombo, &QComboBox::currentTextChanged, this, &QTextView::reloadWithEncoding);
    tb->addWidget(new QLabel(tr(" 인코딩: "), tb));
    tb->addWidget(encCombo);

    tb->addSeparator();

    // 줄바꿈
    auto* eolCombo = new QComboBox(tb);
    eolCombo->setAccessibleName(tr("줄끝 형식"));
    eolCombo->addItem("CRLF", CRLF);
    eolCombo->addItem("LF",   LF);
    eolCombo->addItem("CR",   CR);
    eolCombo->setCurrentIndex(eolCombo->findData(detectedLineEnding()));
    connect(eolCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, eolCombo](int idx) { setLineEnding(static_cast<LineEnding>(eolCombo->itemData(idx).toInt())); });
    tb->addWidget(eolCombo);

    // 탭 간격
    auto* tabSpin = new QSpinBox(tb);
    tabSpin->setAccessibleName(tr("탭 간격"));
    tabSpin->setRange(1, 16);
    tabSpin->setValue(tabWidth());
    tabSpin->setPrefix(tr("Tab: "));
    tabSpin->setToolTip(tr("탭 간격 (칸)"));
    connect(tabSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &QTextView::setTabWidth);
    tb->addWidget(tabSpin);

    tb->addSeparator();

    // 글꼴
    connect(MV_CREATE_TOOLBAR_ACTION(tb, "text.font"),
            &QAction::triggered, this, [this]{
        bool ok;
        QFont font = QFontDialog::getFont(&ok, editorFont(), this);
        if (ok) {
            setEditorFont(font);
            AppSettings Settings;
            Settings.setValue("textView/fontFamily", font.family());
            Settings.setValue("textView/fontSize", qBound(6, font.pointSize(), 72));
        }
    });

    // 읽기 전용
    tb->addSeparator();
    auto* Wrap = new QComboBox(tb); Wrap->setObjectName("wordWrapMode"); Wrap->setAccessibleName(tr("자동 줄바꿈"));
    Wrap->addItems({tr("줄바꿈 없음"), tr("단어 단위"), tr("문자 단위"), tr("공백 단위")});
    Wrap->setCurrentIndex(WordWrapMode()); tb->addWidget(Wrap);
    connect(Wrap, &QComboBox::currentIndexChanged, this, &QTextView::SetWordWrapMode);

    auto* foldingAction = MV_CREATE_TOOLBAR_ACTION(tb, "text.codeFolding");
    foldingAction->setChecked(isCodeFoldingEnabled());
    connect(foldingAction, &QAction::toggled, this, &QTextView::setCodeFoldingEnabled);

    auto* wsCheck = new QCheckBox(tr("¶"), tb);
    wsCheck->setChecked(isWhitespaceVisible());
    wsCheck->setToolTip(tr("탭/공백/개행 문자 표시"));
    connect(wsCheck, &QCheckBox::toggled, this, &QTextView::setWhitespaceVisible);
    tb->addWidget(wsCheck);

    auto* roAction = MV_CREATE_TOOLBAR_ACTION(tb, "text.readOnly");
    roAction->setChecked(isReadOnly());
    connect(roAction, &QAction::toggled, this, &QTextView::setReadOnly);
    connect(this, &QTextView::statusChanged, roAction, [this, roAction] {
        const QSignalBlocker Block(roAction); roAction->setChecked(isReadOnly());
    });

    return tb;
}

bool QTextView::promptOpenModeForFile(const QString& filePath, TextFileSession::OpenMode& openMode) const
{
    const QFileInfo info(filePath);
    if (!TextFileSession::shouldPromptForLargeFile(info.size())) {
        openMode = TextFileSession::OpenMode::Full;
        return true;
    }

    hc::ui::MessageDialog box(const_cast<QTextView*>(this));
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("대용량 파일 열기"));
    box.setText(tr("이 파일은 %1 MB 입니다.").arg(QString::number(static_cast<double>(info.size()) / (1024.0 * 1024.0), 'f', 1)));
    box.setInformativeText(tr("전체 로드는 메모리 사용량이 커질 수 있습니다. 제한 모드는 앞부분만 읽기 전용으로 엽니다."));

    QPushButton* fullButton = box.addButton(tr("전체 로드"), QMessageBox::AcceptRole);
    QPushButton* limitedButton = box.addButton(tr("제한 모드"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == fullButton) {
        openMode = TextFileSession::OpenMode::Full;
        return true;
    }
    if (box.clickedButton() == limitedButton) {
        openMode = TextFileSession::OpenMode::LimitedPreview;
        return true;
    }
    return false;
}

bool QTextView::ensureEditorBackend()
{
    if (m_editor)
        return true;

    m_editor = TextEditorBackendFactory::createDefault(this, this);
    if (!m_editor || !m_editor->widget()) {
        m_editor = nullptr;
        return false;
    }

    if (layout())
        layout()->addWidget(m_editor->widget());

    connect(m_editor, &QObject::destroyed, this, [this] {
        m_editor = nullptr;
    });
    connect(m_editor, &ITextEditorBackend::modificationChanged,
            this, &QBaseView::modifiedChanged);
    connect(m_editor, &ITextEditorBackend::cursorPositionChanged, this,
            [this](int line, int index) {
                m_cachedCurrentLine = line + 1;
                m_cachedCurrentColumn = index + 1;
                emit statusChanged();
            });
    connect(m_editor, &ITextEditorBackend::linesChanged, this, [this] {
        m_cachedLineCount = qMax(1, m_editor ? m_editor->lineCount() : 1);
        if (m_ruler && m_editor)
            m_ruler->setLeftMarginWidth(m_editor->leftMarginWidth());
        emit statusChanged();
    });
    connect(m_editor, &ITextEditorBackend::textChanged, this, [this] {
        updateMetrics();
        scheduleHotExitBackup();
        emit statusChanged();
    });
    connect(m_editor, &ITextEditorBackend::selectionChanged, this, [this] {
        updateMetrics();
        emit statusChanged();
        updateCopyAvailability();
    });

    if (QAbstractScrollArea* area = m_editor->scrollArea()) {
        if (QScrollBar* horizontalScrollBar = area->horizontalScrollBar()) {
            connect(horizontalScrollBar, &QScrollBar::valueChanged, this, [this, horizontalScrollBar]{
                if (m_ruler) {
                    m_ruler->setScrollOffset(horizontalScrollBar->value());
                    m_ruler->setLeftMarginWidth(m_editor->leftMarginWidth());
                }
            });
        }
    }

    // 눈금자에 에디터 폰트 설정
    if (m_ruler) {
        m_ruler->setEditorFont(m_editor->editorFont());
        m_ruler->setLeftMarginWidth(m_editor->leftMarginWidth());
    }

    applyEditorSettings();
    m_editor->applyThemeColors(currentTheme() == Dark);
    updateMetrics();
    updateCopyAvailability();
    return true;
}

void QTextView::loadPersistedEditorPreferences()
{
    applyPersistedEditorPreferences(m_editorSettings);
    // Freeze the migrated/default mode before any other preference creates a
    // textView section, so a new profile does not later resemble a legacy one.
    AppSettings Settings;
    if (!Settings.value("textView/wordWrapMode").isValid())
        Settings.setValue("textView/wordWrapMode", int(m_editorSettings.wrapMode));
}

void QTextView::ReloadPreferences()
{
    if (!m_editor) return;
    ThemeManager::instance().ReloadColorOverrides();
    const bool ReadOnly = m_editorSettings.readOnly;
    applyPersistedEditorPreferences(m_editorSettings);
    m_editorSettings.readOnly = ReadOnly;
    if (isLimitedPreviewMode()) {
        m_editorSettings.showCodeFolding = false;
        m_editorSettings.showIndentationGuides = false;
        m_editorSettings.braceMatching = false;
    }
    AppSettings Settings;
    auto Font = editorFont();
    const auto Family = Settings.value("textView/fontFamily", "D2Coding ligature").toString();
    if (!Family.isEmpty()) Font.setFamily(Family);
    const int Size = Settings.value("textView/fontSize", 16).toInt();
    if (Size >= 6 && Size <= 72) Font.setPointSize(Size);
    m_editor->setEditorFont(Font);
    applyEditorSettings();
    if (m_ruler) {
        m_ruler->setEditorFont(Font);
        m_ruler->setVisible(Settings.value("textView/showRuler", true).toBool());
    }
    emit statusChanged();
}

void QTextView::applyPersistedEditorPreferences(ScintillaEditorSettings& settings) const
{
    AppSettings persisted;
    settings.showLineNumbers = persisted.value("textView/showLineNumbers", true).toBool();
    settings.highlightCurrentLine = persisted.value("textView/highlightCurrentLine", true).toBool();
    settings.autoIndent = persisted.value("textView/autoIndent", true).toBool();
    const auto Legacy = persisted.value("textView/wordWrap");
    const int Fallback = Legacy.isValid() ? (Legacy.toBool() ? 1 : 0) : (persisted.value("textView").isValid() ? 0 : 2);
    settings.wrapMode = static_cast<ScintillaEditorSettings::WrapMode>(qBound(0, persisted.value("textView/wordWrapMode", Fallback).toInt(), 3));
    settings.wrapVisualFlags = persisted.value("textView/wrapVisualFlags", 1).toInt() & ScintillaEditorSettings::WrapFlagMask;
    settings.wrapIndentMode = static_cast<ScintillaEditorSettings::WrapIndentMode>(qBound(0, persisted.value("textView/wrapIndentMode", 1).toInt(), 3));
    settings.tabWidth = qBound(1, persisted.value("textView/tabWidth", 4).toInt(), 16);
    settings.useTabs = persisted.value("textView/useTabs", true).toBool();
    settings.showIndentationGuides = persisted.value("textView/showIndentationGuides", true).toBool();
    const int indentGuideValue = persisted.value("textView/indentGuideStyle",
                                                 static_cast<int>(ScintillaEditorSettings::IndentGuideReal)).toInt();
    settings.indentGuideStyle = static_cast<ScintillaEditorSettings::IndentGuideStyle>(
        qBound(static_cast<int>(ScintillaEditorSettings::IndentGuideReal),
               indentGuideValue,
               static_cast<int>(ScintillaEditorSettings::IndentGuideLookBoth)));
    settings.showWhitespace = persisted.value("textView/showWhitespace", false).toBool();
    settings.showCodeFolding = persisted.value("textView/showCodeFolding", true).toBool();
    settings.braceMatching = persisted.value("textView/braceHighlight", true).toBool();
    settings.lineSpacingScale = qBound(1.0, persisted.value("textView/lineSpacing", 1.1).toDouble(), 3.0);
    const int fontRenderingValue = persisted.value("textView/fontRendering",
                                                   static_cast<int>(ScintillaEditorSettings::Antialiased)).toInt();
    settings.fontRendering = static_cast<ScintillaEditorSettings::FontRenderingMode>(
        qBound(static_cast<int>(ScintillaEditorSettings::Default),
               fontRenderingValue,
               static_cast<int>(ScintillaEditorSettings::LcdOptimized)));

    const int changeHistoryValue = persisted.value("textView/changeHistoryMode",
                                                   static_cast<int>(ScintillaEditorSettings::ChangeHistoryBoth)).toInt();
    settings.changeHistoryMode = static_cast<ScintillaEditorSettings::ChangeHistoryMode>(
        qBound(static_cast<int>(ScintillaEditorSettings::ChangeHistoryOff),
               changeHistoryValue,
               static_cast<int>(ScintillaEditorSettings::ChangeHistoryBoth)));
}

void QTextView::applyEditorSettings()
{
    AppSettings Settings;
    const auto RenderingValue = [&Settings](const char* Key, double Default, double Minimum, double Maximum) {
        bool Valid = false; const double Value = Settings.value(Key, Default).toDouble(&Valid);
        return Valid && std::isfinite(Value) && Value >= Minimum && Value <= Maximum ? Value : Default;
    };
    mrst::scintilla::setTextMask(Settings.value("textView/mask/enabled", false).toBool(),
        RenderingValue("textView/mask/gamma", 1.2, .5, 3.0), RenderingValue("textView/mask/strength", 1.1, .5, 2.0));
    if (m_editor) m_editor->widget()->update();
    lastWrapMode_ = m_editorSettings.wrapMode != ScintillaEditorSettings::WrapNone ? int(m_editorSettings.wrapMode)
        : qBound(1, Settings.value("textView/wordWrapLastMode", 2).toInt(), 3);
    if (auto* Wrap = findChild<QComboBox*>("wordWrapMode")) { const QSignalBlocker Block(Wrap); Wrap->setCurrentIndex(WordWrapMode()); }
    if (m_ruler) {
        QFont RulerFont(Settings.value("textView/rulerFontFamily", "Consolas").toString());
        RulerFont.setPointSize(qBound(6, Settings.value("textView/rulerFontSize", 8).toInt(), 36));
        m_ruler->SetLabelFont(RulerFont);
    }
    if (m_editor)
        m_editor->applySettings(m_editorSettings);
}

void QTextView::showGoToLineDialog()
{
    if (!m_editor)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("줄/열 이동"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* label = new QLabel(tr("이동할 위치를 줄[:열] 형식으로 입력하세요.\n예: 42 또는 42:5"), &dialog);
    layout->addWidget(label);

    auto* edit = new QLineEdit(&dialog);
    edit->setText(QStringLiteral("%1:%2").arg(currentLine()).arg(currentColumn()));
    edit->selectAll();
    edit->setPlaceholderText(tr("줄[:열]"));
    layout->addWidget(edit);

    auto* rangeLabel = new QLabel(tr("현재 문서: 1–%1줄").arg(lineCount()), &dialog);
    layout->addWidget(rangeLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);

    const QRegularExpression pattern(QStringLiteral("^\\s*(\\d+)(?:\\s*[:;,]\\s*(\\d+))?\\s*$"));
    auto updateOkButton = [edit, buttons, pattern] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(pattern.match(edit->text()).hasMatch());
    };
    connect(edit, &QLineEdit::textChanged, &dialog, updateOkButton);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    updateOkButton();
    // Product dialog frame: the instruction becomes the header subtitle, OK · Cancel the footer band.
    label->hide();
    hc::ui::ApplyDialogFrame(&dialog, buttons, {{}, label->text(), fm::ui::glyph::ArrowRight});

    if (dialog.exec() != QDialog::Accepted)
        return;

    const auto match = pattern.match(edit->text());
    if (!match.hasMatch())
        return;

    const int line = match.captured(1).toInt();
    const int column = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
    goToPosition(line, column);
}

void QTextView::updateMetrics()
{
    if (!m_editor)
        return;

    m_cachedLineCount = qMax(1, m_editor->lineCount());

    ScintillaDocument::Metrics metrics;
    metrics.lineCount = m_cachedLineCount;
    metrics.currentLine = qMax(1, m_cachedCurrentLine);
    metrics.currentColumn = qMax(1, m_cachedCurrentColumn);
    metrics.characterCount = characterCount();
    metrics.selectedCharacterCount = selectedCharacterCount();
    m_document.setMetrics(metrics);
}

ScintillaDocument::LineEnding QTextView::toDocumentLineEnding(LineEnding ending)
{
    switch (ending) {
        case LF:
            return ScintillaDocument::LF;
        case CR:
            return ScintillaDocument::CR;
        case CRLF:
        default:
            return ScintillaDocument::CRLF;
    }
}

QTextView::LineEnding QTextView::fromDocumentLineEnding(ScintillaDocument::LineEnding ending)
{
    switch (ending) {
        case ScintillaDocument::LF:
            return LF;
        case ScintillaDocument::CR:
            return CR;
        case ScintillaDocument::CRLF:
        default:
            return CRLF;
    }
}

QTextView::LineEnding QTextView::detectLineEnding(const QString& text)
{
    if (text.contains(QStringLiteral("\r\n")))
        return CRLF;
    if (text.contains(QLatin1Char('\r')))
        return CR;
    return LF;
}

QString QTextView::newHotExitUntitledId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// ═══════════════════════════════════════════════════════════
// 찾기/바꾸기 위젯
// ═══════════════════════════════════════════════════════════
void QTextView::setupFindWidget()
{
    m_findWidget = new FindReplaceWidget(this);
    m_findWidget->setVisible(false);

    if (auto* vbox = qobject_cast<QVBoxLayout*>(layout())) {
        // 눈금자(인덱스 0) 다음에 삽입
        vbox->insertWidget(1, m_findWidget);
    } else if (layout()) {
        layout()->addWidget(m_findWidget);
    }

    connectFindWidgetSignals();
}

void QTextView::connectFindWidgetSignals()
{
    // 검색 디바운스 타이머 (입력 후 400ms 대기)
    m_searchDebounceTimer = new QTimer(this);
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(400);
    connect(m_searchDebounceTimer, &QTimer::timeout, this, &QTextView::performSearch);

    connect(m_findWidget, &FindReplaceWidget::findTextChanged, this, [this](const QString&) {
        clearExcludedRanges();
        m_searchDebounceTimer->start(); // 디바운스: 입력 멈춘 후 400ms 뒤 검색
    });
    connect(m_findWidget, &FindReplaceWidget::findNext, this, &QTextView::performFindNext);
    connect(m_findWidget, &FindReplaceWidget::findPrev, this, &QTextView::performFindPrev);
    connect(m_findWidget, &FindReplaceWidget::replaceRequested, this, &QTextView::performReplace);
    connect(m_findWidget, &FindReplaceWidget::replaceAllRequested, this, &QTextView::performReplaceAll);
    connect(m_findWidget, &FindReplaceWidget::excludeRequested, this, &QTextView::performExclude);
    connect(m_findWidget, &FindReplaceWidget::closed, this, &QTextView::hideFindBar);
    connect(m_findWidget, &FindReplaceWidget::optionsChanged, this, [this] {
        clearExcludedRanges();
        performSearch();
    });
}

void QTextView::showFindBar(bool replaceMode)
{
    if (!m_findWidget)
        return;

    QString Seed;
    if (m_editor && (!m_findWidget->isVisible() || m_editor->widget()->hasFocus())) {
        Seed = m_editor->selectedText();
        if (Seed.isEmpty() || Seed.contains('\n') || Seed.contains('\r')) Seed = m_editor->WordAtCaret();
    }
    // 포커스 이동 전에 선택 범위 저장
    m_isSearching = true; // 설정 중 선택 변경 무시
    if (m_editor) {
        const int selStart = m_editor->selectionStartPos();
        const int selEnd = m_editor->selectionEndPos();
        if (selStart != selEnd) {
            m_findWidget->setSelectionRange(selStart, selEnd);
            m_findWidget->setSearchInSelectionEnabled(true);
            // 시그널 차단하여 optionsChanged → performSearch 연쇄 방지
            const bool blocked = m_findWidget->blockSignals(true);
            m_findWidget->setSearchInSelectionChecked(true);
            m_findWidget->blockSignals(blocked);
        } else {
            m_findWidget->setSearchInSelectionEnabled(false);
        }
    }

    m_findWidget->setReplaceMode(replaceMode);
    if (!Seed.isEmpty()) m_findWidget->SetSearchText(Seed);
    m_findWidget->setVisible(true);
    m_findWidget->focusSearchField();
    m_isSearching = false;
    updateMatchCount();
}

bool QTextView::IsFindBarVisible() const
{
    return m_findWidget && m_findWidget->isVisible();
}

void QTextView::hideFindBar()
{
    if (!m_findWidget)
        return;

    m_findWidget->setVisible(false);
    if (m_searchDebounceTimer)
        m_searchDebounceTimer->stop();
    clearCurrentMatchHighlight();
    clearExcludedRanges();

    // 에디터로 포커스 반환
    if (m_editor && m_editor->widget())
        m_editor->widget()->setFocus();
}

void QTextView::performSearch()
{
    if (!m_editor || !m_findWidget)
        return;

    const QString text = m_findWidget->searchText();
    if (text.isEmpty()) {
        clearCurrentMatchHighlight();
        m_searchResumePos = -1;
        m_findWidget->setMatchCount(0);
        return;
    }

    m_isSearching = true;
    clearCurrentMatchHighlight();
    m_searchResumePos = -1;

    updateMatchCount();

    // 자동 스크롤 옵션이 켜져 있으면 첫 번째 매치로 이동
    if (m_findWidget->isAutoScrollToFirst()) {
        if (m_findWidget->isSearchInSelection()) {
            findInSelectionRange(true, true, true);
        } else {
            findInRange(0, m_editor->documentLength(), true, true, true);
        }
    }

    // updateUi 시그널이 이벤트 루프에서 처리될 수 있으므로 지연 해제
    QTimer::singleShot(0, this, [this] { m_isSearching = false; });
}

void QTextView::performFindNext()
{
    if (!m_findWidget || !m_editor || m_findWidget->searchText().isEmpty())
        return;

    m_isSearching = true;

    if (m_findWidget->isSearchInSelection()) {
        findInSelectionRange(true, true);
    } else {
        findInRange(0, m_editor->documentLength(), true, true);
    }
    QTimer::singleShot(0, this, [this] { m_isSearching = false; });
}

void QTextView::performFindPrev()
{
    if (!m_findWidget || !m_editor || m_findWidget->searchText().isEmpty())
        return;

    m_isSearching = true;

    if (m_findWidget->isSearchInSelection()) {
        findInSelectionRange(false, true);
    } else {
        findInRange(0, m_editor->documentLength(), false, true);
    }
    QTimer::singleShot(0, this, [this] { m_isSearching = false; });
}

void QTextView::performReplace()
{
    if (!m_findWidget || !m_editor)
        return;

    if (m_findWidget->isSearchInSelection()) {
        if (!hasCurrentMatch()) {
            if (!findInSelectionRange(true, true, true))
                return;
        }

        const QString searchText = m_findWidget->searchText();
        if (searchText.isEmpty())
            return;

        const int flags = (m_findWidget->isCaseSensitive() ? 0x04 : 0)
                        | (m_findWidget->isWholeWord() ? 0x02 : 0);
        const int matchStart = m_currentMatchStart;
        const int matchLength = m_currentMatchLength;
        const int delta = m_findWidget->replaceText().size() - searchText.size();

        m_editor->setSearchTargetRange(matchStart, matchStart + matchLength);
        if (m_editor->searchInTarget(searchText, flags) >= 0)
            m_editor->replaceInTarget(m_findWidget->replaceText());

        const int selectionStart = m_findWidget->selectionRangeStart();
        const int selectionEnd = m_findWidget->selectionRangeEnd();
        if (matchStart < selectionEnd)
            m_findWidget->setSelectionRange(selectionStart, selectionEnd + delta);

        clearCurrentMatchHighlight();
        updateMatchCount();
        performSearch();
        return;
    }

    replaceText(m_findWidget->searchText(),
                m_findWidget->replaceText(),
                false,
                m_findWidget->isCaseSensitive(),
                m_findWidget->isWholeWord(),
                true);
    updateMatchCount();
}

void QTextView::performReplaceAll()
{
    if (!m_findWidget || !m_editor)
        return;

    const QString searchStr = m_findWidget->searchText();
    const QString replaceStr = m_findWidget->replaceText();
    if (searchStr.isEmpty())
        return;

    const bool caseSensitive = m_findWidget->isCaseSensitive();
    const bool wholeWord = m_findWidget->isWholeWord();

    // 선택 영역에서 검색 모드
    const bool inSelection = m_findWidget->isSearchInSelection();
    const int rangeStart = inSelection ? m_findWidget->selectionRangeStart() : 0;
    const int rangeEnd = inSelection ? m_findWidget->selectionRangeEnd() : m_editor->documentLength();

    int flags = 0;
    if (caseSensitive) flags |= 0x04; // SCFIND_MATCHCASE
    if (wholeWord)     flags |= 0x02; // SCFIND_WHOLEWORD
    if(m_findWidget->isRegex())flags|=SCFIND_REGEXP|SCFIND_CXX11REGEX;

    // 제외된 범위를 건너뛰며 역순으로 치환 (오프셋 유지 위해)
    // 먼저 모든 매치 위치를 수집
    QVector<QPair<int,int>> matches;
    int pos = rangeStart;
    while (pos < rangeEnd) {
        m_editor->setSearchTargetRange(pos, rangeEnd);
        int found = m_editor->searchInTarget(searchStr, flags);
        if (found < 0) break;
        int matchEnd = m_editor->targetEnd();
        int matchLen = matchEnd - found;
        if (matchLen <= 0) { pos = found + 1; continue; }

        // 제외 목록에 있는지 확인
        bool excluded = false;
        for (const auto& excl : m_excludedRanges) {
            if (found == excl.first && matchLen == excl.second) {
                excluded = true;
                break;
            }
        }
        if (!excluded) {
            matches.append({found, matchLen});
        }
        pos = matchEnd;
    }

    // 역순으로 치환
    for (int i = matches.size() - 1; i >= 0; --i) {
        m_editor->setSearchTargetRange(matches[i].first, matches[i].first + matches[i].second);
        m_editor->searchInTarget(searchStr, flags);
        m_editor->replaceInTarget(replaceStr);
    }

    clearCurrentMatchHighlight();
    clearExcludedRanges();
    updateMatchCount();
}

void QTextView::performExclude()
{
    if (!m_editor || !m_findWidget)
        return;

    const bool searchInSelection = m_findWidget->isSearchInSelection();

    int matchStart = -1;
    int len = 0;
    if (hasCurrentMatch()) {
        matchStart = m_currentMatchStart;
        len = m_currentMatchLength;
    } else {
        const int selStart = m_editor->selectionStartPos();
        const int selEnd = m_editor->selectionEndPos();
        if (selStart == selEnd)
            return;
        matchStart = selStart;
        len = selEnd - selStart;
    }

    if (len <= 0)
        return;

    if (isExcludedRange(matchStart, len)) {
        m_searchResumePos = qMin(m_editor->documentLength(), matchStart + len);
        if (!searchInSelection)
            m_editor->setSelectionByPos(m_searchResumePos, m_searchResumePos);
        clearCurrentMatchHighlight();
        return;
    }

    m_excludedRanges.append({matchStart, len});

    // Indicator로 시각 마킹 (더 진한 배경 + 박스 + 취소선)
    updateSearchIndicatorColors();
    m_editor->applyIndicator(kExcludeBackgroundIndicatorId, matchStart, len);
    m_editor->applyIndicator(kExcludeBorderIndicatorId, matchStart, len);
    m_editor->applyIndicator(kExcludeStrikeIndicatorId, matchStart, len);
    m_searchResumePos = qMin(m_editor->documentLength(), matchStart + len);
    if (!searchInSelection)
        m_editor->setSelectionByPos(m_searchResumePos, m_searchResumePos);
    clearCurrentMatchHighlight();
    updateExcludedCount();
    updateMatchCount();
}

void QTextView::clearExcludedRanges()
{
    if (!m_editor)
        return;

    if (!m_excludedRanges.isEmpty()) {
        m_editor->clearAllIndicator(kExcludeBackgroundIndicatorId);
        m_editor->clearAllIndicator(kExcludeBorderIndicatorId);
        m_editor->clearAllIndicator(kExcludeStrikeIndicatorId);
        m_excludedRanges.clear();
    }

    m_searchResumePos = -1;
    updateExcludedCount();
}

void QTextView::updateExcludedCount()
{
    if (m_findWidget)
        m_findWidget->setExcludedCount(m_excludedRanges.size());
}

void QTextView::updateSearchIndicatorColors()
{
    if (!m_editor)
        return;
    const auto& Colors = fm::style::ThemeManager::instance().colors();
    const auto Alpha = [](QColor Color, int Value) { Color.setAlpha(Value); return Color; };
    // Recolor existing ranges on theme changes without repeating the search.
    m_editor->setIndicatorStyle(kCurrentMatchIndicatorId, 7, Alpha(Colors[fm::style::Token::Warn], 110));
    m_editor->setIndicatorStyle(kExcludeBackgroundIndicatorId, 8, Alpha(Colors[fm::style::Token::DangerBg], 150));
    m_editor->setIndicatorStyle(kExcludeBorderIndicatorId, 7, Alpha(Colors[fm::style::Token::DangerLine], 170));
    m_editor->setIndicatorStyle(kExcludeStrikeIndicatorId, 5, Colors[fm::style::Token::Fg3]);
}

void QTextView::clearCurrentMatchHighlight()
{
    if (!m_editor)
        return;

    if (m_currentMatchStart >= 0 && m_currentMatchLength > 0)
        m_editor->clearIndicator(kCurrentMatchIndicatorId, m_currentMatchStart, m_currentMatchLength);

    m_currentMatchStart = -1;
    m_currentMatchLength = 0;
}

void QTextView::setCurrentMatchHighlight(int startPos, int length, bool scrollToMatch)
{
    if (!m_editor || length <= 0)
        return;

    clearCurrentMatchHighlight();
    updateSearchIndicatorColors();
    m_editor->applyIndicator(kCurrentMatchIndicatorId, startPos, length);
    m_currentMatchStart = startPos;
    m_currentMatchLength = length;

    if (scrollToMatch)
        m_editor->scrollRangeToView(startPos, startPos + length);
}

int QTextView::currentSearchFlags() const
{
    if (!m_findWidget)
        return 0;

    int flags = 0;
    if (m_findWidget->isCaseSensitive())
        flags |= 0x04;
    if (m_findWidget->isWholeWord())
        flags |= 0x02;
    if(m_findWidget->isRegex())flags|=SCFIND_REGEXP|SCFIND_CXX11REGEX;
    return flags;
}

bool QTextView::isExcludedRange(int startPos, int length) const
{
    for (const auto& excludedRange : m_excludedRanges) {
        if (excludedRange.first == startPos && excludedRange.second == length)
            return true;
    }
    return false;
}

bool QTextView::findInRange(int rangeStart, int rangeEnd, bool forward, bool wrap, bool fromRangeBoundary)
{
    if (!m_editor || !m_findWidget)
        return false;

    const QString text = m_findWidget->searchText();
    const bool searchInSelection = m_findWidget->isSearchInSelection();
    if (text.isEmpty()) {
        clearCurrentMatchHighlight();
        m_searchResumePos = -1;
        return false;
    }

    if (rangeStart >= rangeEnd) {
        clearCurrentMatchHighlight();
        return false;
    }

    const int flags = currentSearchFlags();
    const auto clampToRange = [rangeStart, rangeEnd](int pos) {
        return qBound(rangeStart, pos, rangeEnd);
    };

    auto searchOnce = [&](int startPos, int endPos) -> bool {
        int nextStart = startPos;

        while (forward ? (nextStart < endPos) : (nextStart > endPos)) {
            m_editor->setSearchTargetRange(nextStart, endPos);
            const int found = m_editor->searchInTarget(text, flags);
            if (found < 0)
                return false;

            const int matchEnd = m_editor->targetEnd();
            const int matchLength = matchEnd - found;
            if (matchLength <= 0) {
                nextStart = forward ? qMin(endPos, found + 1)
                                    : qMax(endPos, found - 1);
                continue;
            }

            if (isExcludedRange(found, matchLength)) {
                nextStart = forward ? qMin(rangeEnd, found + qMax(1, matchLength))
                                    : qMax(rangeStart, found - 1);
                continue;
            }

            if (!searchInSelection)
                m_editor->setSelectionByPos(found, matchEnd);
            setCurrentMatchHighlight(found, matchLength, true);
            m_searchResumePos = -1;
            return true;
        }

        return false;
    };

    const int selectionAnchor = forward ? m_editor->selectionEndPos()
                                        : m_editor->selectionStartPos();
    int searchFrom = forward ? rangeStart : rangeEnd;
    if (!fromRangeBoundary) {
        if (hasCurrentMatch()) {
            searchFrom = forward ? qMin(rangeEnd, m_currentMatchStart + qMax(1, m_currentMatchLength))
                                 : qMax(rangeStart, m_currentMatchStart - 1);
        } else if (m_searchResumePos >= 0) {
            searchFrom = forward ? clampToRange(m_searchResumePos)
                                 : clampToRange(m_searchResumePos - 1);
        } else {
            searchFrom = searchInSelection
                ? (forward ? rangeStart : rangeEnd)
                : clampToRange(selectionAnchor);
        }
    }

    if (forward) {
        if (searchOnce(searchFrom, rangeEnd))
            return true;
        if (wrap && searchFrom > rangeStart && searchOnce(rangeStart, searchFrom))
            return true;
    } else {
        if (searchOnce(searchFrom, rangeStart))
            return true;
        if (wrap && searchFrom < rangeEnd && searchOnce(rangeEnd, searchFrom))
            return true;
    }

    clearCurrentMatchHighlight();
    m_searchResumePos = -1;
    return false;
}

bool QTextView::findInSelectionRange(bool forward, bool wrap, bool fromRangeBoundary)
{
    if (!m_editor || !m_findWidget || !m_findWidget->isSearchInSelection())
        return false;

    const int rangeStart = m_findWidget->selectionRangeStart();
    const int rangeEnd = m_findWidget->selectionRangeEnd();
    return findInRange(rangeStart, rangeEnd, forward, wrap, fromRangeBoundary);
}

bool QTextView::hasCurrentMatch() const
{
    return m_currentMatchStart >= 0 && m_currentMatchLength > 0;
}

void QTextView::updateMatchCount()
{
    if (!m_findWidget || !m_editor)
        return;

    const QString text = m_findWidget->searchText();
    if (text.isEmpty()) {
        m_findWidget->setMatchCount(0);
        m_findWidget->setExcludedCount(0);
        return;
    }

    int count = 0;
    if (m_findWidget->isSearchInSelection()) {
        count = m_editor->countMatchesInRange(text, m_findWidget->isRegex(),
                    m_findWidget->isCaseSensitive(), m_findWidget->isWholeWord(),
                    m_findWidget->selectionRangeStart(), m_findWidget->selectionRangeEnd());
    } else {
        count = m_editor->countMatches(text, m_findWidget->isRegex(),
                    m_findWidget->isCaseSensitive(), m_findWidget->isWholeWord());
    }
    m_findWidget->setMatchCount(count);
    updateExcludedCount();
}
