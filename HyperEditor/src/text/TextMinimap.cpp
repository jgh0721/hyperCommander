#include "stdafx.h"
#include "text/TextMinimap.h"

/// 모듈

/// UIs
#include "text/TextMinimapPeek.h"
#include "text/TextMinimapWidget.h"
#include "text/TextOverviewRuler.h"

/// 기타 헤더파일
#include "core/ThemeManager.h"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "text/TextMinimapSource.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <Scintilla.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractScrollArea>
#include <QActionGroup>
#include <QElapsedTimer>
#include <QEvent>
#include <QLoggingCategory>
#include <QMenu>
#include <QScrollBar>
#include <functional>
#include <limits>

namespace
{
    Q_LOGGING_CATEGORY(minimapLog, "hc.text.minimap")

    constexpr qint64 SliceMs = 4;
    // A search over a document up to this size finishes inside the find bar's call, as the old count did.
    constexpr qint64 SynchronousSearchBytes = 4LL * 1024 * 1024;
    // Heading labels are skipped beyond this many lines; such documents use the outline anyway.
    constexpr int HeadingLineLimit = 200000;
    constexpr int CandidateDocuments = 32;
}

TextMinimap::TextMinimap(QTextView* View, ScintillaQtDirectBackend* Backend)
    : QObject(View)
    , view_(View)
    , backend_(Backend)
    , source_(std::make_unique<ScintillaMinimapSource>(Backend))
{
    auto* Area = Backend->scrollArea();
    column_ = new TextMinimapWidget(this, Area);
    column_->hide();
    ruler_ = new TextOverviewRuler(this, Area->verticalScrollBar());
    ruler_->hide();
    peek_ = new TextMinimapPeek(this, View);
    workTimer_.setSingleShot(true);
    workTimer_.setInterval(0);
    connect(&workTimer_, &QTimer::timeout, this, &TextMinimap::work);
    // Idle wrapping and idle styling progress without notifications; this timer only runs while something is shown.
    layoutTimer_.setInterval(200);
    connect(&layoutTimer_, &QTimer::timeout, this, &TextMinimap::layoutCheck);
    headingsTimer_.setSingleShot(true);
    headingsTimer_.setInterval(300);
    connect(&headingsTimer_, &QTimer::timeout, this, &TextMinimap::restartHeadings);
    changesTimer_.setSingleShot(true);
    changesTimer_.setInterval(120);
    connect(&changesTimer_, &QTimer::timeout, this, [this] { restartChanges(false); });
    searchTimer_.setSingleShot(true);
    searchTimer_.setInterval(300);
    connect(&searchTimer_, &QTimer::timeout, this, [this] {
        if (!search_.Active())
            return;
        const auto Key = search_.CurrentKey();
        if (const int Count = RunSearch(QString::fromUtf8(Key.text_), Key.flags_, Key.start_, Key.end_); Count >= 0)
            emit SearchFinished(Count);
    });
    Area->installEventFilter(this);
    Area->viewport()->installEventFilter(this);
    currentDocument_ = source_->DocumentId();
    lineCount_ = source_->LineCount();
    connectBackend();
    connect(&TextMinimapSettingsNotifier::Instance(), &TextMinimapSettingsNotifier::Changed, this, &TextMinimap::ReloadSettings);
    ReloadSettings();
}

TextMinimap::~TextMinimap() = default;

TextMinimapWidget* TextMinimap::Column() const { return column_; }
TextOverviewRuler* TextMinimap::Ruler() const { return ruler_; }
TextMinimapPeek* TextMinimap::Peek() const { return peek_; }
QTextView* TextMinimap::View() const { return view_; }

void TextMinimap::connectBackend()
{
    auto* Backend = backend_.data();
    connect(Backend, &ITextEditorBackend::textEdited, this, &TextMinimap::textEdited);
    connect(Backend, &ITextEditorBackend::linesShifted, this, &TextMinimap::linesShifted);
    connect(Backend, &ScintillaQtDirectBackend::StylesChanged, this, &TextMinimap::stylesChanged);
    connect(Backend, &ScintillaQtDirectBackend::MarkersChanged, this, [this] { if (settings_.changes_) changesTimer_.start(); });
    connect(Backend, &ScintillaQtDirectBackend::DocumentReset, this, &TextMinimap::documentReset);
    connect(Backend, &ITextEditorBackend::viewportScrolled, this, &TextMinimap::viewportChanged);
    connect(Backend, &ITextEditorBackend::selectionChanged, this, &TextMinimap::repaint);
    connect(Backend, &ITextEditorBackend::cursorPositionChanged, this, &TextMinimap::repaint);
    connect(Backend, &ITextEditorBackend::modificationChanged, this, [this] { if (settings_.changes_) changesTimer_.start(); });
    connect(view_.data(), &QTextView::languageChanged, this, &TextMinimap::LanguageChanged);
}

// ── 설정 · 형태 ─────────────────────────────────────────────────────────

void TextMinimap::ReloadSettings()
{
    settings_ = TextMinimapSettings::Load();
    reloadPalette();
    styleColors_.clear();
    renderer_.Clear();
    updateMode(true);
    restartHeadings();
    restartChanges(false);
    repaint();
}

void TextMinimap::ThemeChanged()
{
    reloadPalette();
    styleColors_.clear();
    renderer_.Clear();
    repaint();
}

void TextMinimap::LanguageChanged()
{
    styleColors_.clear();
    renderer_.Clear();
    restartHeadings();
    repaint();
}

void TextMinimap::reloadPalette()
{
    auto& Theme = ThemeManager::instance();
    const auto Color = [&Theme](const char* Key, const QColor& Fallback) {
        const QColor Value = Theme.color(QStringLiteral("text.minimap.") + QLatin1String(Key));
        return Value.isValid() ? Value : Fallback;
    };
    const QColor Foreground = Theme.color(QStringLiteral("text.foreground"));
    const auto Fade = [&Foreground](int Alpha) { QColor Value = Foreground; Value.setAlpha(Alpha); return Value; };
    palette_.background_ = Color("background", Theme.color(QStringLiteral("text.background")));
    palette_.separator_ = Color("separator", Fade(40));
    palette_.slider_ = Color("slider", Fade(26));
    palette_.sliderHover_ = Color("sliderHover", Fade(41));
    palette_.sliderActive_ = Color("sliderActive", Fade(56));
    palette_.bracket_ = Color("bracket", Foreground);
    palette_.search_ = Color("search", QColor(0xF2, 0xC9, 0x4C));
    palette_.selection_ = Color("selection", QColor(0x3F, 0x74, 0xCC, 110));
    palette_.caret_ = Color("caret", Foreground);
    palette_.error_ = Color("error", QColor(0xF2, 0x76, 0x6B));
    palette_.warning_ = Color("warning", QColor(0xF2, 0xBA, 0x52));
    palette_.changeModified_ = Color("changeModified", QColor(0xFF, 0x80, 0x00));
    palette_.changeSaved_ = Color("changeSaved", QColor(0x00, 0xA0, 0x00));
    palette_.outline_ = Color("outline", Fade(102));
    palette_.label_ = Color("label", Foreground);
    palette_.labelMinor_ = Color("labelMinor", Fade(190));
    palette_.pending_ = palette_.outline_;
}

QColor TextMinimap::StyleColor(int Style) const
{
    if (styleColors_.isEmpty())
        styleColors_ = QVector<QColor>(256);
    auto& Slot = styleColors_[qBound(0, Style, 255)];
    if (!Slot.isValid() && backend_) {
        const auto Bgr = backend_->Send(SCI_STYLEGETFORE, quintptr(qBound(0, Style, 255)));
        Slot = QColor(int(Bgr & 0xFF), int((Bgr >> 8) & 0xFF), int((Bgr >> 16) & 0xFF));
        Slot.setAlpha(Dark() ? 225 : 205);
    }
    return Slot;
}

QFont TextMinimap::EditorFont() const { return backend_ ? backend_->editorFont() : QFont(); }
bool TextMinimap::Dark() const { return ThemeManager::instance().currentTheme() == ThemeManager::Dark; }

void TextMinimap::updateMode(bool Force)
{
    if (!backend_ || !view_)
        return;
    auto* Area = backend_->scrollArea();
    const int ViewportWidth = Area->viewport()->width() + reserveLeft_ + reserveRight_;
    const auto Next = TextMinimapResolveMode(settings_, source_->LineCount(), source_->Length(), view_->isLimitedPreviewMode(),
                                             ViewportWidth, mode_.narrow_);
    const bool Same = Next.column_ == mode_.column_ && Next.overview_ == mode_.overview_ && Next.narrow_ == mode_.narrow_ && Next.large_ == mode_.large_;
    if (Same && !Force)
        return;
    const bool ColumnChanged = Next.column_ != mode_.column_;
    mode_ = Next;
    const bool ShowColumn = mode_.column_ != TextMinimapMode::Column::Hidden;
    const int Width = ShowColumn ? settings_.width_ : 0;
    reserveLeft_ = settings_.side_ == TextMinimapSettings::Side::Left ? Width : 0;
    reserveRight_ = settings_.side_ == TextMinimapSettings::Side::Right ? Width : 0;
    backend_->SetViewportReserve(reserveLeft_, reserveRight_);
    column_->setVisible(ShowColumn);
    ruler_->setVisible(mode_.overview_);
    placeWidgets();
    if (ColumnChanged || Force) {
        renderer_.Clear();
        restartOutline();
    }
    if (ShowColumn || mode_.overview_) layoutTimer_.start(); else layoutTimer_.stop();
    qCDebug(minimapLog) << "mode column" << int(mode_.column_) << "overview" << mode_.overview_ << "narrow" << mode_.narrow_ << "large" << mode_.large_;
    repaint();
}

void TextMinimap::placeWidgets()
{
    if (!backend_ || !column_->isVisible())
        return;
    auto* Area = backend_->scrollArea();
    const QRect Viewport = Area->viewport()->geometry();
    const int Width = settings_.width_;
    const int X = settings_.side_ == TextMinimapSettings::Side::Left ? Viewport.left() - Width : Viewport.right() + 1;
    const QRect Target(X, Viewport.top(), Width, Viewport.height());
    if (column_->geometry() != Target) {
        const bool HeightChanged = column_->height() != Target.height();
        column_->setGeometry(Target);
        if (HeightChanged && mode_.column_ == TextMinimapMode::Column::Outline)
            restartOutline();
    }
    column_->raise();
}

bool TextMinimap::eventFilter(QObject* Watched, QEvent* Event)
{
    if (backend_ && (Watched == backend_->scrollArea() || Watched == backend_->scrollArea()->viewport())) {
        if (Event->type() == QEvent::Resize || Event->type() == QEvent::Show) {
            updateMode();
            placeWidgets();
        }
    }
    return QObject::eventFilter(Watched, Event);
}

// ── 편집기 신호 ─────────────────────────────────────────────────────────

TextMinimapLineSet& TextMinimap::candidates()
{
    const qintptr Document = source_->DocumentId();
    if (!candidates_.contains(Document)) {
        candidateOrder_.append(Document);
        while (candidateOrder_.size() > CandidateDocuments)
            candidates_.remove(candidateOrder_.takeFirst());
    }
    return candidates_[Document];
}

void TextMinimap::documentReset()
{
    ++revision_;
    renderer_.Clear();
    problems_.clear();
    styleColors_.clear();
    const qintptr Document = source_->DocumentId();
    // SetTextUtf8() keeps the document but replaces its text and empties its history, so its candidate lines are
    // void. AttachDocument() brings back another document (an EPUB resource tab) whose history is still valid: its
    // kept candidates are used, or one sliced scan rebuilds them when it has undo history.
    const bool Switched = Document != currentDocument_;
    currentDocument_ = Document;
    const bool History = backend_ && (backend_->Send(SCI_CANUNDO) != 0 || backend_->Send(SCI_CANREDO) != 0);
    if (!Switched || !History) {
        candidates_.remove(Document);
        candidateOrder_.removeAll(Document);
    }
    changes_.clear();
    lineCount_ = source_->LineCount();
    displayRows_ = -1;
    updateMode(true);
    restartHeadings();
    restartChanges(Switched && History && !candidates_.contains(Document));
    if (search_.Active())
        searchTimer_.start();
    repaint();
}

void TextMinimap::textEdited(qint64 Position, qint64 Removed, qint64 Inserted)
{
    Q_UNUSED(Removed);
    ++revision_;
    const int First = source_->LineFromPosition(Position);
    const int Last = source_->LineFromPosition(Position + Inserted);
    candidates().Add(First, Last);
    renderer_.InvalidateLines(First, Last);
    // A changed line count arrives next as linesShifted(), which rebuilds the outline and checks the large limit
    // against the old count kept in lineCount_.
    if (mode_.column_ == TextMinimapMode::Column::Outline && source_->LineCount() == lineCount_)
        outline_.Invalidate(source_->RowOfLine(First), source_->RowOfLine(Last + 1) - 1);
    if (settings_.changes_) changesTimer_.start();
    if (settings_.labels_) headingsTimer_.start();
    if (search_.Active()) searchTimer_.start();
    scheduleWork();
    repaint();
}

void TextMinimap::linesShifted(int Line, int Delta)
{
    candidates().Shift(Line, Delta);
    TextMinimapShiftProblems(problems_, Line, Delta);
    for (auto& Mark : changes_)
        if (Mark.line_ > Line) Mark.line_ = qMax(Line, Mark.line_ + Delta);
    renderer_.InvalidateFrom(Line);
    const bool CrossesLimit = (lineCount_ >= settings_.largeLines_) != (source_->LineCount() >= settings_.largeLines_);
    lineCount_ = source_->LineCount();
    if (CrossesLimit)
        updateMode();
    if (mode_.column_ == TextMinimapMode::Column::Outline)
        restartOutline();
}

void TextMinimap::stylesChanged(qint64 Position, qint64 Length)
{
    if (mode_.column_ != TextMinimapMode::Column::Document)
        return;
    renderer_.InvalidateLines(source_->LineFromPosition(Position), source_->LineFromPosition(Position + Length));
    column_->update();
}

void TextMinimap::viewportChanged()
{
    HidePeek();
    layoutCheck();
    repaint();
}

void TextMinimap::layoutCheck()
{
    const int Rows = source_->DisplayRows();
    if (Rows != displayRows_) {
        displayRows_ = Rows;
        if (mode_.column_ == TextMinimapMode::Column::Outline)
            restartOutline();
        repaint();
    }
    const qint64 Styled = source_->EndStyled();
    if (Styled != endStyled_) {
        endStyled_ = Styled;
        if (mode_.column_ == TextMinimapMode::Column::Document)
            column_->update();
    }
}

void TextMinimap::repaint()
{
    if (column_ && column_->isVisible()) column_->update();
    if (ruler_ && ruler_->isVisible()) ruler_->update();
}

// ── 문서 전체 작업 ─────────────────────────────────────────────────────

void TextMinimap::restartOutline()
{
    if (mode_.column_ != TextMinimapMode::Column::Outline) {
        outline_.Reset(0, 1);
        return;
    }
    const int Rows = source_->DisplayRows();
    displayRows_ = Rows;
    const int Bins = qBound(1, qMin(Rows, qMax(1, column_->height() / 2)), 4096);
    outline_.Reset(Bins, Rows);
    scheduleWork();
}

void TextMinimap::restartHeadings()
{
    const bool Wanted = settings_.enabled_ && settings_.labels_ && mode_.column_ != TextMinimapMode::Column::Hidden
        && source_->LineCount() <= HeadingLineLimit;
    headings_.Start(Wanted ? TextMinimapHeadings::KindForLanguage(view_ ? view_->currentLanguage() : QString()) : TextMinimapHeadings::Kind::None,
                    source_->LineCount());
    scheduleWork();
}

void TextMinimap::restartChanges(bool FullScan)
{
    if (!settings_.enabled_ || !settings_.changes_ || !source_->HistoryQueryable() || (!mode_.overview_ && mode_.column_ == TextMinimapMode::Column::Hidden)) {
        changes_.clear();
        changesRunning_ = false;
        return;
    }
    fullScan_ = FullScan;
    changeRange_ = 0;
    changeLine_ = -1;
    changesPending_.clear();
    changesRunning_ = true;
    scheduleWork();
}

bool TextMinimap::changesStep(const QElapsedTimer& Clock, qint64 BudgetMs)
{
    const auto Visit = [this](int Line) {
        const auto Change = TextMinimapChangeOf(source_->Marks(Line));
        if (Change != TextMinimapChange::None)
            changesPending_.append({Line, Change});
        return Change != TextMinimapChange::None;
    };
    if (fullScan_) {
        const int Lines = source_->LineCount();
        for (int Line = qMax(0, changeLine_ + 1); Line < Lines; ++Line) {
            changeLine_ = Line;
            if ((Line & 255) == 0 && Clock.elapsed() >= BudgetMs)
                return false;
            if (Visit(Line))
                candidates().Add(Line, Line);
        }
    } else {
        const auto Ranges = candidates().Ranges();
        const int Lines = source_->LineCount();
        for (; changeRange_ < Ranges.size(); ++changeRange_) {
            const auto& Range = Ranges[changeRange_];
            for (int Line = qMax(Range.first, changeLine_ + 1); Line <= qMin(Range.second, Lines - 1); ++Line) {
                changeLine_ = Line;
                if ((Line & 255) == 0 && Clock.elapsed() >= BudgetMs)
                    return false;
                Visit(Line);
            }
        }
    }
    changes_ = std::exchange(changesPending_, {});
    changesRunning_ = false;
    return true;
}

void TextMinimap::scheduleWork()
{
    if (!workTimer_.isActive())
        workTimer_.start();
}

void TextMinimap::work()
{
    QElapsedTimer Clock;
    Clock.start();
    bool More = false;
    if (search_.Active() && !search_.Complete()) {
        if (search_.Step(*source_, Clock, SliceMs)) {
            emit SearchFinished(search_.Count());
            repaint();
        } else {
            More = true;
        }
    }
    if (!More && mode_.column_ == TextMinimapMode::Column::Outline && !outline_.Complete()) {
        More = !outline_.Step(*source_, Clock, SliceMs);
        column_->update();
    }
    if (!More && !headings_.Complete()) {
        More = !headings_.Step(*source_, Clock, SliceMs);
        if (!More) repaint();
    }
    if (!More && changesRunning_) {
        More = !changesStep(Clock, SliceMs);
        if (!More) repaint();
    }
    if (More)
        workTimer_.start();
}

// ── 검색 · 문제 ────────────────────────────────────────────────────────

int TextMinimap::RunSearch(const QString& Text, int Flags, qint64 Start, qint64 End)
{
    if (Text.isEmpty()) {
        ClearSearch();
        return 0;
    }
    TextMinimapSearch::Key Key;
    Key.document_ = source_->DocumentId();
    Key.revision_ = revision_;
    Key.text_ = Text.toUtf8();
    Key.flags_ = Flags;
    Key.start_ = Start;
    Key.end_ = End;
    const qint64 Length = source_->Length();
    search_.Start(Key, Length, source_->LineCount());
    if (Length <= SynchronousSearchBytes) {
        QElapsedTimer Clock;
        Clock.start();
        search_.Step(*source_, Clock, std::numeric_limits<qint64>::max());
        repaint();
        return search_.Count();
    }
    scheduleWork();
    repaint();
    return -1;
}

void TextMinimap::ClearSearch()
{
    searchTimer_.stop();
    search_.Clear();
    repaint();
}

void TextMinimap::SetProblems(QVector<TextMinimapProblem> Problems)
{
    problems_ = std::move(Problems);
    repaint();
}

// ── 이동 · 미리보기 ─────────────────────────────────────────────────────

void TextMinimap::ScrollRowToCenter(double Row)
{
    ScrollToRow(int(std::lround(Row - source_->RowsOnScreen() / 2.0)));
}

void TextMinimap::ScrollToRow(int Row)
{
    source_->ScrollToRow(qBound(0, Row, qMax(0, source_->DisplayRows() - 1)));
}

void TextMinimap::GoToLine(int Line)
{
    if (!view_)
        return;
    view_->goToLine(Line + 1);
    if (backend_ && backend_->widget())
        backend_->widget()->setFocus(Qt::MouseFocusReason);
}

void TextMinimap::ShowPeek(double Row, const QRect& Anchor, int GlobalY)
{
    if (!settings_.peek_ || !peek_)
        return;
    const int Line = source_->LineOfRow(qMax(0, int(Row)));
    peek_->ShowAt(Line, Anchor, GlobalY, settings_.side_ == TextMinimapSettings::Side::Right);
}

void TextMinimap::HidePeek()
{
    if (peek_ && peek_->isVisible())
        peek_->hide();
}

// ── 메뉴 ───────────────────────────────────────────────────────────────

void TextMinimap::ToggleEnabled()
{
    auto Settings = TextMinimapSettings::Load();
    Settings.enabled_ = !Settings.enabled_;
    TextMinimapSettings::Store(Settings);
}

void TextMinimap::PopulateMenu(QMenu* Menu, QTextView* View)
{
    Menu->clear();
    const auto Current = TextMinimapSettings::Load();
    const auto Change = [](auto Apply) {
        auto Settings = TextMinimapSettings::Load();
        Apply(Settings);
        TextMinimapSettings::Store(Settings);
    };
    auto* Show = Menu->addAction(tr("미니맵 표시"));
    Show->setObjectName(QStringLiteral("minimapMenu.show"));
    Show->setCheckable(true);
    Show->setChecked(Current.enabled_);
    connect(Show, &QAction::triggered, Menu, [] { ToggleEnabled(); });
    Menu->addSeparator();
    auto* Heading = Menu->addAction(tr("형태"));
    Heading->setEnabled(false);
    auto* Group = new QActionGroup(Menu);
    const QList<QPair<QString, TextMinimapSettings::Kind>> Kinds{{tr("자동 (권장)"), TextMinimapSettings::Kind::Auto},
        {tr("문서 축소판"), TextMinimapSettings::Kind::Document}, {tr("윤곽"), TextMinimapSettings::Kind::Outline},
        {tr("개요 막대만"), TextMinimapSettings::Kind::Overview}};
    for (const auto& [Label, Kind] : Kinds) {
        auto* Item = Menu->addAction(Label);
        Item->setObjectName(QStringLiteral("minimapMenu.kind.") + TextMinimapSettings::KindName(Kind));
        Item->setCheckable(true);
        Item->setChecked(Current.kind_ == Kind);
        Item->setEnabled(Current.enabled_);
        Group->addAction(Item);
        connect(Item, &QAction::triggered, Menu, [Change, Kind] { Change([Kind](TextMinimapSettings& S) { S.kind_ = Kind; }); });
    }
    Menu->addSeparator();
    const auto Toggle = [&](const QString& Label, const char* Name, bool Checked, std::function<void(TextMinimapSettings&, bool)> Apply) {
        auto* Item = Menu->addAction(Label);
        Item->setObjectName(QLatin1String(Name));
        Item->setCheckable(true);
        Item->setChecked(Checked);
        Item->setEnabled(Current.enabled_);
        connect(Item, &QAction::triggered, Menu, [Change, Apply](bool On) { Change([Apply, On](TextMinimapSettings& S) { Apply(S, On); }); });
    };
    Toggle(tr("머리글 이름표"), "minimapMenu.labels", Current.labels_, [](TextMinimapSettings& S, bool On) { S.labels_ = On; });
    Toggle(tr("호버 미리보기"), "minimapMenu.peek", Current.peek_, [](TextMinimapSettings& S, bool On) { S.peek_ = On; });
    Toggle(tr("왼쪽에 표시"), "minimapMenu.left", Current.side_ == TextMinimapSettings::Side::Left,
           [](TextMinimapSettings& S, bool On) { S.side_ = On ? TextMinimapSettings::Side::Left : TextMinimapSettings::Side::Right; });
    if (View && View->window()) {
        if (auto* Settings = View->window()->findChild<QAction*>(QStringLiteral("app.settings"))) {
            Menu->addSeparator();
            auto* Open = Menu->addAction(tr("미니맵 설정…"));
            Open->setObjectName(QStringLiteral("minimapMenu.settings"));
            connect(Open, &QAction::triggered, Settings, &QAction::trigger);
        }
    }
}

// ── QTextView ─────────────────────────────────────────────────────────

bool QTextView::IsMinimapEnabled() const { return m_minimap && m_minimap->Settings().enabled_; }

void QTextView::SetMinimapEnabled(bool On)
{
    auto Settings = TextMinimapSettings::Load();
    if (Settings.enabled_ == On)
        return;
    Settings.enabled_ = On;
    TextMinimapSettings::Store(Settings);
}

void QTextView::SetProblemLines(const QVector<QPair<int, bool>>& Lines)
{
    if (!m_minimap)
        return;
    QVector<TextMinimapProblem> Problems;
    Problems.reserve(Lines.size());
    for (const auto& [Line, Error] : Lines)
        Problems.append({Line, Error});
    m_minimap->SetProblems(std::move(Problems));
}

TextMinimap* QTextView::Minimap() const { return m_minimap; }
