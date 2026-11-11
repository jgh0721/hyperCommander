#include "stdafx.h"
#include "markdownDocumentView.hpp"
#include "markdownProcessView.hpp"
#include "documentShortcutRegistry.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "core/AppSettings.h"
#include "core/ToolbarIcons.h"
#include <Scintilla.h>
#include <fmstyle/StyleProps.h>
#include <QActionGroup>
#include <QToolBar>
#include <QToolButton>
#include <QMenu>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeySequence>
#include <QPointer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QHeaderView>
#include <QSplitter>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>
#include <QDesktopServices>
#include <QSignalBlocker>
#include <QApplication>
#include <QAbstractScrollArea>
#include <cmath>
#include <qt_windows.h>

namespace hc::document
{
MarkdownDocumentView::MarkdownDocumentView(QTextView *Source, const QString &Path, bool Editor, QWidget *Parent)
    : QWidget(Parent), source_(Source), path_(Path), editor_(Editor)
{
    setObjectName("markdownDocumentView");
    auto *Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->setSpacing(0);
    // One row: [미리보기 | 소스 | 나란히] | 개요 · 스크롤 동기화 | 축소 · 확대 · 갱신 | 찾기, plus the text
    // editor's tools while the source is shown (SetSourceTools). Icon-only like the other view rows;
    // the modes, 개요 and 스크롤 동기화 keep their names beside the icon.
    toolbar_ = new QToolBar(tr("마크다운"), this);
    toolbar_->setObjectName("markdownToolbar");
    toolbar_->setMovable(false);
    toolbar_->setIconSize(QSize(20, 20));
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto Make = [&](const QString &Id, const QString &Label, bool Check = false, const char *Icon = nullptr) {
        auto *A = toolbar_->addAction(Label);
        A->setObjectName(Id);
        A->setCheckable(Check);
        if (Icon)
        {
            A->setIcon(ToolbarIcons::iconByName(QString::fromLatin1(Icon)));
            // The workspace re-tints named icons when the theme changes.
            A->setProperty("documentIconName", QString::fromLatin1(Icon));
        }
        actions_[Id] = A;
        connect(A, &QAction::triggered, this, [this, Id] { Run(Id); });
        return A;
    };
    const auto Named = [this](QAction *A) {
        if (auto *Button = qobject_cast<QToolButton *>(toolbar_->widgetForAction(A)))
            Button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        return A;
    };
    auto *Modes = new QActionGroup(this);
    QList<QAbstractButton *> Segment;
    for (const auto &[Id, Label, Tip, Icon] :
         {std::tuple{"markdown.preview", tr("미리보기"), tr("미리보기"), "view-preview"},
          std::tuple{"markdown.source", tr("소스"), tr("소스 편집"), "view-source"},
          std::tuple{"markdown.split", tr("나란히"), tr("나란히 보기"), "view-split"}})
    {
        auto *A = Named(Make(QString::fromLatin1(Id), Label, true, Icon));
        A->setToolTip(Tip);
        Modes->addAction(A);
        if (auto *Button = qobject_cast<QAbstractButton *>(toolbar_->widgetForAction(A)))
            Segment << Button;
    }
    fm::style::setSegments(Segment);
    toolbar_->addSeparator();
    Named(Make("markdown.toggleToc", tr("개요"), true, "toc-toggle"))->setToolTip(tr("개요 표시 전환"));
    Named(Make("markdown.syncScroll", tr("스크롤 동기화"), true, "sync"))
        ->setToolTip(tr("소스 캐럿·본문 클릭과 화면 스크롤을 함께 연결합니다."));
    toolbar_->addSeparator();
    Make("markdown.zoomOut", tr("미리보기 축소"), false, "zoom-out");
    Make("markdown.zoomIn", tr("미리보기 확대"), false, "zoom-in");
    Make("markdown.refresh", tr("미리보기 갱신"), false, "refresh");
    toolbar_->addSeparator();
    Make("markdown.find", tr("찾기"), false, "search");
    // Commands without a button: menus, status pickers and shortcuts run them.
    for (const auto &[Id, Label] : {std::pair{"markdown.focusOtherPane", tr("포커스 이동")},
                                    std::pair{"markdown.toggleSource", tr("소스 전환")},
                                    std::pair{"markdown.problems", tr("렌더링 문제")},
                                    std::pair{"markdown.currentHeading", tr("현재 개요 항목")},
                                    std::pair{"markdown.zoomReset", tr("미리보기 원래 크기")},
                                    std::pair{"markdown.zoomSet", tr("미리보기 배율")}})
        toolbar_->removeAction(Make(QString::fromLatin1(Id), Label));
    message_ = new QLabel(this);
    message_->setWordWrap(true);
    message_->hide();
    Layout->addWidget(message_);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);
    outlinePanel_ = new QWidget(splitter_);
    outlinePanel_->setMinimumWidth(120);
    outlinePanel_->setMaximumWidth(420);
    auto *OutlineLayout = new QVBoxLayout(outlinePanel_);
    OutlineLayout->setContentsMargins(0, 0, 0, 0);
    auto *Filter = new QLineEdit(outlinePanel_);
    Filter->setObjectName("markdownOutlineFilter");
    Filter->setPlaceholderText(tr("개요 검색"));
    Filter->setClearButtonEnabled(true);
    OutlineLayout->addWidget(Filter);
    outline_ = new QTreeWidget(outlinePanel_);
    outline_->setHeaderLabel(tr("개요 (활성 문서)"));
    OutlineLayout->addWidget(outline_);
    connect(Filter, &QLineEdit::textChanged, this, [this](const QString &Text) {
        auto Visit = [&](auto &&Self, QTreeWidgetItem *Item) -> bool {
            bool Match = Item->text(0).contains(Text, Qt::CaseInsensitive);
            for (int I = 0; I < Item->childCount(); ++I)
                Match = Self(Self, Item->child(I)) || Match;
            Item->setHidden(!Match);
            if (!Text.isEmpty() && Match)
                Item->setExpanded(true);
            return Match;
        };
        for (int I = 0; I < outline_->topLevelItemCount(); ++I)
            Visit(Visit, outline_->topLevelItem(I));
    });
    splitter_->addWidget(source_);
    previewPanel_ = new QWidget(splitter_);
    auto *PreviewLayout = new QVBoxLayout(previewPanel_);
    PreviewLayout->setContentsMargins(0, 0, 0, 0);
    PreviewLayout->setSpacing(0);
    preview_ = new MarkdownProcessView(previewPanel_);
    PreviewLayout->addWidget(preview_);
    splitter_->addWidget(previewPanel_);
    Layout->addWidget(splitter_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 1);
    splitter_->setSizes({190, 520, 520});
    connect(splitter_, &QSplitter::splitterMoved, this, [this] {
        AppSettings S;
        S.setValue(QString("markdown/layout/") + (editor_ ? "editor" : "viewer"),
                   QString::fromLatin1(splitter_->saveState().toBase64()));
    });
    // Outline lines come from the rendered revision.
    connect(outline_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *Item) { goToHeadingLine(Item->data(0, Qt::UserRole).toInt()); });
    outline_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(outline_, &QWidget::customContextMenuRequested, this, &MarkdownDocumentView::showOutlineMenu);
    debounce_.setSingleShot(true);
    connect(&debounce_, &QTimer::timeout, this, &MarkdownDocumentView::renderWhenIdle);
    connect(source_, &QTextView::ContentChanged, this, [this] {
        ++revision_;
        status_ = tr("갱신 대기");
        render_ = RenderState::Waiting;
        if (options_["autoPreview"].toBool(hc::defaults::edit::ViewValue("markdown/autoPreview").toBool()))
            debounce_.start(renderDelay());
        emit Changed();
    });
    connect(preview_, &MarkdownProcessView::Reported, this, &MarkdownDocumentView::report);
    // Before the preview window attaches, a right click lands on the host widget itself.
    connect(preview_, &QWidget::customContextMenuRequested, this, [this](const QPoint &Position) {
        showPreviewMenu({{"type", "context"}, {"x", Position.x() / qMax(.1, zoom_)}, {"y", Position.y() / qMax(.1, zoom_)}, {"line", 0}});
    });
    source_->Backend()->widget()->installEventFilter(this);
    // The anchor follows the editor's own caret and viewport signals; several in one event
    // loop turn are sent once. Nothing is computed while the editor is idle.
    sampleSoon_.setSingleShot(true);
    connect(&sampleSoon_, &QTimer::timeout, this, &MarkdownDocumentView::sampleSource);
    connect(source_->Backend(), &ITextEditorBackend::cursorPositionChanged, this, &MarkdownDocumentView::scheduleSample);
    connect(source_->Backend(), &ITextEditorBackend::viewportScrolled, this, &MarkdownDocumentView::scheduleSample);
    connect(source_->Backend(), &ITextEditorBackend::linesShifted, this, [this](int Line, int Delta) {
        if (shifts_.size() < 256) shifts_.append({Line, Delta}); else shiftsOverflow_ = true;
    });
    // Bytes changed since the text the renderer holds; a document swapped behind the editor has no common base.
    connect(source_->Backend(), &ITextEditorBackend::textEdited, this, [this](qint64 Position, qint64 Removed, qint64 Inserted) {
        if (editsDocument_ != qintptr(static_cast<ScintillaQtDirectBackend *>(source_->Backend())->Send(SCI_GETDOCPOINTER)))
            edits_.Invalidate();
        else
            edits_.Apply(Position, Removed, Inserted);
    });
    connect(splitter_, &QSplitter::splitterMoved, this, [this] { sampleSoon_.start(0); });
    // The preview's native focus has no Qt signal in this process.
    sample_.setInterval(250);
    connect(&sample_, &QTimer::timeout, this, &MarkdownDocumentView::pollFocus);
    sample_.start();
    ReloadPreferences();
    AppSettings S;
    splitter_->restoreState(QByteArray::fromBase64(
        S.value(QString("markdown/layout/") + (editor_ ? "editor" : "viewer")).toString().toLatin1()));
    // The preview is a foreign WebEngine window: each live resize exposes its backing surface
    // before Chromium has drawn the new layout. Keep both panes still until the handle is released.
    // restoreState also restores opaqueResize, so override old saved layouts here.
    splitter_->setOpaqueResize(false);
    mode(S.value(QString("markdown/") + (editor_ ? "editorMode" : "viewerMode"))
             .toString());
    QTimer::singleShot(0, this, [this] { Refresh(); });
}
MarkdownDocumentView::~MarkdownDocumentView()
{
    source_->Backend()->widget()->removeEventFilter(this);
}
bool MarkdownDocumentView::SourceVisible() const
{
    return mode_ != "preview";
}
QWidget *MarkdownDocumentView::InputWidget() const
{
    return SourceVisible() && !preview_->HasFocus() ? source_->Backend()->widget() : preview_;
}
void MarkdownDocumentView::SetReadOnly(bool Value)
{
    readOnly_ = Value;
    source_->setReadOnly(Value);
    actions_["markdown.source"]->setToolTip(Value ? tr("소스 보기") : tr("소스 편집"));
}
void MarkdownDocumentView::Focus()
{
    auto* Input = InputWidget();
    if (Input == preview_) preview_->FocusInput();
    else focusSource();
}
void MarkdownDocumentView::focusSource()
{
    auto *Input = source_->Backend()->widget();
    Input->setFocus(Qt::OtherFocusReason);
    // The preview is another process's window; its native focus does not follow Qt's.
    const auto Root = GetAncestor(reinterpret_cast<HWND>(window()->effectiveWinId()), GA_ROOT);
    if (Root && GetAncestor(GetForegroundWindow(), GA_ROOT) == Root)
        SetFocus(reinterpret_cast<HWND>(Input->effectiveWinId()));
}
void MarkdownDocumentView::SetSourceTools(QToolBar *Tools)
{
    if (!Tools)
        return;
    // The text bar is never shown as a second row. Actions are matched by their toolbar name or,
    // after a theme refresh renamed them, by their action key.
    Tools->hide();
    const auto Take = [Tools](const char *Name, const char *Key) -> QAction * {
        for (auto *A : Tools->actions())
            if (A->objectName() == QLatin1String(Name) || A->objectName() == QLatin1String(Key))
            {
                Tools->removeAction(A);
                return A;
            }
        return nullptr;
    };
    auto *Replace = Take("textToolbar.replace", "text.replaceBar");
    auto *Wrap = Take("textToolbar.wordWrap", "text.wordWrap");
    auto *Whitespace = Take("textToolbar.whitespace", "text.whitespace");
    auto *Minimap = Take("textToolbar.minimap", "text.minimap");
    // After 찾기: 바꾸기 | 줄바꿈 · 공백 문자 · 미니맵.
    if (Replace)
        sourceActions_ << Replace;
    if (Wrap || Whitespace || Minimap)
    {
        auto *Gap = new QAction(toolbar_);
        Gap->setSeparator(true);
        sourceActions_ << Gap;
    }
    for (auto *A : {Wrap, Whitespace, Minimap})
        if (A)
            sourceActions_ << A;
    for (auto *A : sourceActions_)
    {
        toolbar_->addAction(A);
        A->setVisible(SourceVisible());
    }
}
void MarkdownDocumentView::SetShortcutBindings(const QJsonObject &Bindings)
{
    // Stored overrides or a resolved table: both resolve to every command's key.
    bindings_ = DocumentShortcutRegistry::Bindings(Bindings);
    QJsonObject Keys;
    for (const auto &Item : DocumentShortcutRegistry::Commands())
        if (Item.scope_ == "markdown" || Item.scope_ == "common")
            Keys[Item.id_] = bindings_.value(Item.id_);
    Keys["markdown.find"] = bindings_.value("text.find");
    preview_->Send({{"type", "shortcuts"}, {"bindings", Keys}});
}
void MarkdownDocumentView::mode(const QString &Value)
{
    mode_ = QStringList{"source", "preview", "split"}.contains(Value) ? Value : "preview";
    source_->setVisible(SourceVisible());
    for (auto *A : sourceActions_)
        A->setVisible(SourceVisible());
    previewPanel_->setVisible(mode_ != "source");
    preview_->setVisible(mode_ != "source");
    QTimer::singleShot(0, this, &MarkdownDocumentView::alignPreview);
    for (const auto &Id : {"source", "preview", "split"})
        actions_["markdown." + QString(Id)]->setChecked(mode_ == Id);
    syncControls();
    emit Changed();
}
void MarkdownDocumentView::syncControls()
{
    outlinePanel_->setVisible(actions_["markdown.toggleToc"]->isChecked());
}
void MarkdownDocumentView::saveToggle(const QString &Key, bool Value)
{
    AppSettings S;
    S.setValue("markdown/" + Key, Value);
}
bool MarkdownDocumentView::CanRun(const QString &Id) const
{
    return actions_.contains(Id) && actions_[Id]->isEnabled();
}
bool MarkdownDocumentView::RunPreviewCommand(const QString &Id)
{
    // The minimap toggle belongs to the source editor even while the preview has focus.
    if (Id == QLatin1String("text.minimap"))
        return false;
    if (SourceVisible() && !preview_->HasFocus())
        return false;
    if (Id == "edit.copy" || Id == "edit.selectAll")
        preview_->Send({{"type", Id == "edit.copy" ? "copy" : "selectAll"}});
    else if (Id == "text.find")
        Run("markdown.find");
    return Id.startsWith("edit.") || Id.startsWith("text.");
}
bool MarkdownDocumentView::Run(const QString &Id, const QJsonValue &Value)
{
    if (!CanRun(Id))
        return false;
    if (Id == "markdown.source" || Id == "markdown.preview" || Id == "markdown.split")
    {
        mode(Id.section('.', 1));
        Focus();
    }
    else if (Id == "markdown.toggleSource")
    {
        mode(SourceVisible() ? "preview" : "source");
        Focus();
    }
    else if (Id == "markdown.focusOtherPane")
    {
        if (mode_ == "split")
        {
            if (preview_->HasFocus())
                source_->Backend()->widget()->setFocus();
            else
                preview_->FocusInput();
        }
    }
    else if (Id == "markdown.toggleToc")
    {
        syncControls();
        saveToggle(editor_ ? "editorOutline" : "viewerOutline", actions_[Id]->isChecked());
    }
    else if (Id == "markdown.syncScroll")
    {
        syncControls();
        saveToggle("scrollSync", actions_[Id]->isChecked());
        lastLine_ = -1;lastPosition_ = -1;lastSourceAnchor_ = {};sampleSoon_.start(0);
    }
    else if (Id == "markdown.refresh")
    {
        if (!error_.isEmpty())
            preview_->Restart();
        Refresh(true);
    }
    else if (Id == "markdown.currentHeading")
    {
        actions_["markdown.toggleToc"]->setChecked(true);
        syncControls();
        if (outline_->currentItem())
            outline_->scrollToItem(outline_->currentItem());
    }
    else if (Id == "markdown.problems")
    {
        if (rendered_ != revision_)
            return false;
        QStringList Items;
        for (const auto V : problemDetails_)
        {
            const auto P = V.toObject();
            Items.append(tr("줄 %1 · %2 · %3")
                             .arg(P["line"].toInt())
                             .arg(P["kind"].toString(), P["message"].toString().simplified().left(180)));
        }
        if (!Items.isEmpty())
        {
            bool Ok = false;
            const auto Choice =
                QInputDialog::getItem(this, tr("렌더링 문제"), tr("소스 위치로 이동"), Items, 0, false, &Ok);
            if (Ok)
            {
                mode("split");
                source_->goToLine(problemDetails_[Items.indexOf(Choice)].toObject()["line"].toInt());
                source_->Backend()->widget()->setFocus();
            }
        }
    }
    else if (Id == "markdown.find")
    {
        // The row's one 찾기 button: the source's find bar unless the preview is the pane in use.
        if (mode_ == "source" || (SourceVisible() && !preview_->HasFocus() && source_->Backend()->widget()->hasFocus()))
            source_->showFindBar();
        else
        {
            bool Ok = false;
            const auto Text =
                QInputDialog::getText(this, tr("미리보기 찾기"), tr("찾을 내용"), QLineEdit::Normal, {}, &Ok);
            if (Ok)
                preview_->Send({{"type", "find"}, {"text", Text}});
        }
    }
    else if (Id == "markdown.zoomIn" || Id == "markdown.zoomOut")
        SetPreviewZoom(PreviewZoomPercent() + (Id.endsWith("In") ? 10 : -10));
    else if (Id == "markdown.zoomReset")
        SetPreviewZoom(100);
    else if (Id == "markdown.zoomSet")
    {
        bool Ok = false;
        int Percent = qRound(Value.toVariant().toDouble(&Ok));
        if (!Ok)
            Percent = QInputDialog::getInt(this, tr("미리보기 배율"), tr("배율 (%)"), PreviewZoomPercent(), MinZoomPercent,
                                           MaxZoomPercent, 10, &Ok);
        if (Ok)
            SetPreviewZoom(Percent);
    }
    emit Changed();
    return true;
}
int MarkdownDocumentView::PreviewZoomPercent() const
{
    return qRound(zoom_ * 100);
}
void MarkdownDocumentView::SetPreviewZoom(int Percent)
{
    const int Value = qBound(MinZoomPercent, Percent, MaxZoomPercent);
    if (Value == PreviewZoomPercent())
        return;
    zoom_ = Value / 100.0;
    preview_->Send({{"type", "zoom"}, {"value", zoom_}});
    // Offsets are CSS pixels; the same screen height is a different offset now.
    lastSourceAnchor_ = {};
    sampleSoon_.start(0);
    emit Changed();
}
void MarkdownDocumentView::ReloadPreferences()
{
    AppSettings S;
    options_ = {};
    for (const auto &Key : {"math", "mermaid", "inlineDollar", "codeHighlight", "autoPreview"})
        options_[Key] = S.value("markdown/" + QString(Key)).toBool();
    options_["remoteImages"] = S.value("markdown/remoteImages").toBool();
    QJsonObject Fonts;
    for (const auto& Key : {"enabled", "mode", "bodyFamily", "bodySizePt", "codeFamily", "codeSizePt", "lineHeight", "headings"})
        Fonts[Key] = QJsonValue::fromVariant(S.value("markdown/previewFonts/" + QString(Key)));
    options_["previewFonts"] = Fonts;
    // Position and scroll sync are one toggle now. Scroll sync only ever worked while
    // position sync was on, so the setting keeps what was effectively enabled.
    if (S.contains("markdown/positionSync"))
    {
        S.setValue("markdown/scrollSync", S.value("markdown/positionSync").toBool() && S.value("markdown/scrollSync").toBool());
        S.remove("markdown/positionSync");
        S.sync();
    }
    {
        const QSignalBlocker B(actions_["markdown.syncScroll"]);
        actions_["markdown.syncScroll"]->setChecked(S.value("markdown/scrollSync").toBool());
    }
    actions_["markdown.toggleToc"]->setChecked(
        S.value(editor_ ? "markdown/editorOutline" : "markdown/viewerOutline").toBool());
    syncControls();
    debounceMs_ = qBound(100, S.value("markdown/debounceMs").toInt(), 3000);
    debounce_.setInterval(debounceMs_);
    // report() rebuilds the outline after every render; it must not read the settings file there.
    // A new depth applies to the headings already rendered: the page itself does not change.
    if (const int Depth = qBound(1, S.value("markdown/outlineDepth").toInt(), 6); Depth != outlineDepth_)
    {
        outlineDepth_ = Depth;
        rebuildOutline();
    }
    if (!mode_.isEmpty())
        Refresh();
}
int MarkdownDocumentView::renderDelay() const
{
    // A long render keeps the helper's GUI thread busy, and that thread shares its input queue
    // with this one: wait longer before starting the next one.
    return qMax(debounceMs_, int(qMin<qint64>(lastRenderMs_ * 2, MaxRenderDelayMs)));
}
bool MarkdownDocumentView::inputPending() const
{
    if (HIWORD(GetQueueStatus(QS_KEY)) != 0)
        return true;
    return lastInput_.isValid() && lastInput_.elapsed() < debounceMs_;
}
void MarkdownDocumentView::renderWhenIdle()
{
    // Arrow keys and IME events do not restart the debounce as edits do. Starting a render
    // while they arrive makes the helper busy exactly when the editor needs the shared queue.
    if (inputPending() && (!deferredSince_.isValid() || deferredSince_.elapsed() < MaxInputDeferMs))
    {
        if (!deferredSince_.isValid())
            deferredSince_.start();
        const qint64 Quiet = lastInput_.isValid() ? debounceMs_ - lastInput_.elapsed() : 0;
        debounce_.start(int(qBound(qint64(SampleFrameMs), Quiet, qint64(debounceMs_))));
        return;
    }
    Refresh();
}
void MarkdownDocumentView::scheduleSample()
{
    // The caret and viewport signals of one key arrive in one event loop turn, and a pending
    // run (also the scroll guard's) already samples the latest state.
    if (sampleSoon_.isActive())
        return;
    const qint64 Since = lastSampleSent_.isValid() ? lastSampleSent_.elapsed() : SampleFrameMs;
    sampleSoon_.start(Since >= SampleFrameMs ? 0 : int(SampleFrameMs - Since));
}
bool MarkdownDocumentView::sourceMatchesSent() const
{
    if (!edits_.Changed())
        return true;
    const auto Start = edits_.Start(), Removed = edits_.Removed(), Inserted = edits_.InsertedEnd() - Start;
    if (Removed != Inserted || Removed > 1024 * 1024 || Start + Removed > sentText_.size())
        return false;
    auto *Backend = static_cast<ScintillaQtDirectBackend *>(source_->Backend());
    QByteArray Bytes(Inserted + 1, Qt::Uninitialized);
    Sci_TextRangeFull Range{};
    Range.chrg.cpMin = Start;
    Range.chrg.cpMax = Start + Inserted;
    Range.lpstrText = Bytes.data();
    Backend->Send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<qintptr>(&Range));
    return QByteArrayView(Bytes.constData(), Inserted) == QByteArrayView(sentText_).sliced(Start, Removed);
}
void MarkdownDocumentView::Refresh(bool Force)
{
    debounce_.stop();
    deferredSince_.invalidate();
    auto *Backend = static_cast<ScintillaQtDirectBackend *>(source_->Backend());
    const qint64 Length = Backend->Send(SCI_GETLENGTH);
    if (!Force && Length > AutoRenderBytes)
    {
        status_ = tr("대용량 · 수동 갱신 필요");
        render_ = RenderState::Manual;
        emit Changed();
        return;
    }
    if (Length > PreviewBytes)
    {
        error_ = tr("미리보기는 16 MiB 이하 문서를 지원합니다. 소스 편집은 계속할 수 있습니다.");
        message_->setText(error_);
        message_->show();
        emit Changed();
        return;
    }
    options_["dark"] = palette().color(QPalette::Window).lightness() < 128;
    // The renderer holds the text of helperRevision_ while the editor's document and its edit range are
    // continuous with it; then a request carries only the changed bytes.
    const auto Document = qintptr(Backend->Send(SCI_GETDOCPOINTER));
    const bool Synced = sentValid_ && error_.isEmpty() && helperRevision_ >= 0 && path_ == sentPath_ && Document == editsDocument_
        && edits_.Valid() && edits_.Length() == Length && sentText_.size() == edits_.BaseLength();
    // Theme and settings reloads, an undo back to the rendered text and outline clicks ask again
    // for what the preview already shows. Only the explicit refresh (Force) repeats a render.
    if (!Force && Synced && options_ == sentOptions_ && sourceMatchesSent())
    {
        edits_.Reset(Length);
        // The source is that revision again: its line map is the identity.
        revision_ = renderRequested_;
        shifts_.clear();
        shiftsOverflow_ = false;
        const bool Partial = source_->isContentTruncated();
        if (rendered_ == renderRequested_)
        {
            status_ = Partial ? tr("부분 미리보기") : tr("최신");
            render_ = Partial ? RenderState::Partial : RenderState::Current;
        }
        else
        {
            status_ = Partial ? tr("부분 미리보기 갱신 중") : tr("갱신 중");
            render_ = RenderState::Rendering;
        }
        emit Changed();
        return;
    }
    ++revision_;
    renderRequested_ = revision_;
    // The request carries every edit so far; later edits shift lines against it.
    shifts_.clear();
    shiftsOverflow_ = false;
    error_.clear();
    message_->hide();
    status_ = source_->isContentTruncated() ? tr("부분 미리보기 갱신 중") : tr("갱신 중");
    render_ = RenderState::Rendering;
    QJsonObject Message{{"type", "render"},
                        {"revision", revision_},
                        {"path", path_},
                        {"baseUrl", QUrl::fromLocalFile(QFileInfo(path_).absolutePath() + "/").toString()},
                        {"options", options_},
                        {"scroll", SourceVisible() && actions_["markdown.syncScroll"]->isChecked()
                            && source_->Backend()->widget()->hasFocus() && !preview_->HasFocus() ? sourceScrollAnchor() : QJsonObject{}}};
    if (Synced && !Force)
    {
        // UTF-8 offsets into the renderer's text; bytes that are not UTF-8 would change on the way.
        const auto Start = edits_.Start(), End = edits_.InsertedEnd();
        QByteArray Inserted(End - Start + 1, Qt::Uninitialized);
        Sci_TextRangeFull Range{};
        Range.chrg.cpMin = Start;
        Range.chrg.cpMax = End;
        Range.lpstrText = Inserted.data();
        Backend->Send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<qintptr>(&Range));
        Inserted.resize(End - Start);
        const auto Text = QString::fromUtf8(Inserted);
        if (Text.toUtf8() == Inserted)
        {
            Message["edit"] = QJsonObject{{"base", helperRevision_}, {"baseLength", edits_.BaseLength()}, {"start", Start},
                                          {"removed", edits_.Removed()}, {"inserted", Text}};
            sentText_.replace(Start, edits_.Removed(), Inserted);
            ++editRenders_;
        }
    }
    if (!Message.contains("edit"))
    {
        sentText_ = QByteArray(reinterpret_cast<const char *>(Backend->Send(SCI_GETCHARACTERPOINTER)), Length);
        Message["text"] = QString::fromUtf8(sentText_);
        fullRevision_ = revision_;
    }
    edits_.Reset(Length);
    editsDocument_ = Document;
    helperRevision_ = revision_;
    sentPath_ = path_;
    sentOptions_ = options_;
    sentValid_ = true;
    renderClock_.start();
    preview_->Send(Message);
    emit Changed();
}
void MarkdownDocumentView::report(const QJsonObject &Value)
{
    const auto Type = Value["type"].toString();
    if (Type == "escape")
    {
        emit EscapeRequested();
        return;
    }
    if (Type == "context")
    {
        // Not tied to a revision. The menu opens after this socket read returns, so its event
        // loop never runs inside MarkdownProcessView::receive.
        QTimer::singleShot(0, this, [this, Value] { showPreviewMenu(Value); });
        return;
    }
    if (Type == "command")
    {
        const auto Id = Value["id"].toString();
        if (Id.startsWith("markdown."))
        {
            if (CanRun(Id))
            {
                auto *A = actions_[Id];
                A->trigger();
            }
            return;
        }
        if (DocumentShortcutRegistry::ScopeOf(Id) != "common")
            return;
        for (auto *A : window()->findChildren<QAction *>())
            if (A->property("documentCommandId").toString() == Id && A->isEnabled())
            {
                A->trigger();
                break;
            }
        return;
    }
    if (Type == "focus")
    {
        pollFocus();
        return;
    }
    if (Type == "diagnostics")
    {
        diagnostics_ = Value;
        return;
    }
    if (Type == "resync")
    {
        // The renderer lacked the text an edit was based on (a restarted helper, a dropped message).
        // The next request sends the whole text; a refusal of an edit sent before it changes nothing.
        if (Value["revision"].toInteger(-1) > fullRevision_)
        {
            helperRevision_ = -1;
            ++resyncs_;
            QTimer::singleShot(0, this, [this] { Refresh(); });
        }
        return;
    }
    // Reports describe the requested revision. Edits made since then are mapped
    // through shifts_, so the preview stays synchronised while it waits for a render.
    if (Type != "error" && Value["revision"].toInteger(-1) != renderRequested_)
        return;
    if (Type == "error")
    {
        // A failed or restarted helper shows nothing: the next request must be sent.
        sentValid_ = false;
        error_ = Value["message"].toString();
        status_ = tr("갱신 실패");
        message_->setText(error_);
        message_->show();
    }
    else if (Type == "rendered")
    {
        rendered_ = renderRequested_;
        status_ = renderRequested_ != revision_ ? tr("갱신 대기") : source_->isContentTruncated() ? tr("부분 미리보기") : tr("최신");
        render_ = renderRequested_ != revision_ ? RenderState::Waiting
                  : source_->isContentTruncated() ? RenderState::Partial
                                                  : RenderState::Current;
        renderedAt_ = QDateTime::currentDateTime();
        if (renderClock_.isValid())
            lastRenderMs_ = renderClock_.elapsed();
        renderClock_.invalidate();
        // The new frame has its own layout: send the current anchor to it.
        lastSourceAnchor_ = {};
        lastPosition_ = -1;
        sampleSoon_.start(0);
        problems_ = Value["problems"].toInt();
        problemDetails_ = Value["details"].toArray();
        renderBlocks_ = Value["blocks"].toObject();
        headings_ = Value["headings"].toArray();
        rebuildOutline();
        // The foreign preview can leave Qt's logical source focus intact while
        // reparenting changes the native keyboard receiver. Repair that mismatch
        // only while the source still owns focus in the foreground editor.
        auto* Input=source_->Backend()->widget();
        const auto Root=GetAncestor(reinterpret_cast<HWND>(window()->effectiveWinId()),GA_ROOT);
        if(SourceVisible() && Input->hasFocus() && Input->isVisible() && Input->isEnabled()
            && Root && GetAncestor(GetForegroundWindow(),GA_ROOT)==Root
            && !QApplication::activeModalWidget() && !QApplication::activePopupWidget())
            QTimer::singleShot(0, this, [this] {
                auto* Source = source_->Backend()->widget();
                const auto Owner = GetAncestor(reinterpret_cast<HWND>(window()->effectiveWinId()), GA_ROOT);
                if (SourceVisible() && Source->hasFocus() && Source->isVisible() && Owner
                    && GetAncestor(GetForegroundWindow(), GA_ROOT) == Owner
                    && !QApplication::activeModalWidget() && !QApplication::activePopupWidget()) {
                    SetActiveWindow(Owner);QApplication::setActiveWindow(window());
                    Source->setFocus(Qt::OtherFocusReason);SetFocus(reinterpret_cast<HWND>(Source->effectiveWinId()));
                }
            });
    }
    else if (Type == "position" && actions_["markdown.syncScroll"]->isChecked() && SourceVisible() && !composing_)
    {
        // A click places the caret at the clicked character and keeps its screen height.
        suppress_.restart();
        placeSource(fromPreviewLine(Value["line"].toDouble(1)), Value["column"].isDouble() ? Value["column"].toInt() : -1,
                    Value["offset"].toDouble(), true);
        lastSourceAnchor_ = sourceScrollAnchor();
        lastPosition_ = static_cast<ScintillaQtDirectBackend *>(source_->Backend())->Send(SCI_GETCURRENTPOS);
    }
    else if (Type == "scroll")
    {
        previewScroll_ = Value;
        progress_ = Value["progress"].toInt();
        heading_ = Value["heading"].toString();
        for (QTreeWidgetItemIterator It(outline_); *It; ++It)
            if ((*It)->data(0, Qt::UserRole).toInt() == Value["headingLine"].toInt())
            {
                const QSignalBlocker Block(outline_);
                outline_->setCurrentItem(*It);
                break;
            }
        // Keys reach the preview only while it has focus. Wheel, pointer and touch scroll the
        // preview under the pointer even while the source keeps the focus, and the source follows.
        // A scrollbar drag or press has no page input events; the preview reports it as such.
        // Every user report is followed: a smooth scroll keeps reporting after the first one,
        // and the preview never calls its own echo of an editor request a user scroll.
        const auto Intent = Value["intent"].toString();
        const bool Positional = Intent == "wheel" || Intent == "pointer" || Intent == "touch" || Intent == "scrollbar";
        if (Value["user"].toBool() && (Positional || preview_->HasFocus()) && actions_["markdown.syncScroll"]->isChecked() && SourceVisible() && !composing_)
        {
            auto *B = static_cast<ScintillaQtDirectBackend *>(source_->Backend());
            suppress_.restart();
            if (Value["top"].toBool()) B->Send(SCI_SETFIRSTVISIBLELINE, 0);
            else if (Value["bottom"].toBool()) B->Send(SCI_SETFIRSTVISIBLELINE, quintptr(1) << 30);
            else placeSource(fromPreviewLine(Value["line"].toDouble(1)), Value["column"].isDouble() ? Value["column"].toInt() : -1,
                             Value["anchorOffset"].toDouble(), false);
            lastSourceAnchor_ = sourceScrollAnchor();
            lastPosition_ = B->Send(SCI_GETCURRENTPOS);
        }
    }
    else if (Type == "link")
        openLink(Value["url"].toString(), {});
    emit Changed();
}
void MarkdownDocumentView::rebuildOutline()
{
    outline_->clear();
    QList<QTreeWidgetItem *> Parents(7, nullptr);
    for (const auto V : headings_)
    {
        const auto H = V.toObject();
        const int Level = qBound(1, H["level"].toInt(), 6);
        if (Level > outlineDepth_)
            continue;
        QTreeWidgetItem *Parent = nullptr;
        for (int I = Level - 1; I > 0; --I)
            if (Parents[I])
            {
                Parent = Parents[I];
                break;
            }
        auto *Item = Parent ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(outline_);
        Item->setText(0, H["text"].toString());
        Item->setData(0, Qt::UserRole, H["line"].toInt());
        Parents[Level] = Item;
        for (int I = Level + 1; I < 7; ++I)
            Parents[I] = nullptr;
    }
    outline_->expandAll();
    if (auto *Filter = findChild<QLineEdit *>("markdownOutlineFilter"))
        QMetaObject::invokeMethod(Filter, "textChanged", Qt::DirectConnection, Q_ARG(QString, Filter->text()));
}
void MarkdownDocumentView::openLink(const QString &Link, const QString &Fragment)
{
    if (!Fragment.isEmpty())
    {
        // An in-document link scrolls the preview, as a click on it does.
        preview_->Send({{"type", "anchor"}, {"revision", renderRequested_}, {"id", Fragment}});
        return;
    }
    const QUrl Url(Link);
    if (Url.scheme() == "https" || Url.scheme() == "http")
        QDesktopServices::openUrl(Url);
    else if (Url.isLocalFile() && !QFileInfo(Url.toLocalFile()).isDir())
        emit OpenRequested(Url.toLocalFile());
}
void MarkdownDocumentView::sendPosition(double Line, bool Scroll, const QJsonObject &Anchor, bool Reveal)
{
    if (renderRequested_ < 0)
        return;
    QJsonObject Message{{"type", Scroll ? "scroll" : "position"},
                        {"revision", renderRequested_},
                        {"line", Line},
                        {"reveal", Reveal || !actions_["markdown.syncScroll"]->isChecked()}};
    if (Scroll)
    {
        for (const auto *Key : {"column", "offset", "caret", "top", "end"})
            if (Anchor.contains(Key))
                Message[Key] = Anchor[Key];
        Message["serial"] = ++scrollSerial_;
    }
    preview_->Send(Message);
}
double MarkdownDocumentView::toPreviewLine(double Line) const
{
    return qMax(1.0, ToRevisionLine(Line, shifts_));
}
double MarkdownDocumentView::fromPreviewLine(double Line) const
{
    return qMax(1.0, FromRevisionLine(Line, shifts_));
}
// Both panes share screen coordinates: an anchor row and its rendered line go to the same
// screen height. The preview measures CSS pixels, scaled by its zoom factor.
double MarkdownDocumentView::previewOffsetForSourceY(double Y) const
{
    const auto *Viewport = static_cast<ScintillaQtDirectBackend *>(source_->Backend())->scrollArea()->viewport();
    return (Viewport->mapToGlobal(QPoint(0, 0)).y() + Y - preview_->mapToGlobal(QPoint(0, 0)).y()) / qMax(.1, zoom_);
}
double MarkdownDocumentView::sourceYForPreviewOffset(double Offset) const
{
    const auto *Viewport = static_cast<ScintillaQtDirectBackend *>(source_->Backend())->scrollArea()->viewport();
    return preview_->mapToGlobal(QPoint(0, 0)).y() + Offset * qMax(.1, zoom_) - Viewport->mapToGlobal(QPoint(0, 0)).y();
}
QJsonObject MarkdownDocumentView::sourceScrollAnchor() const
{
    auto *B = static_cast<ScintillaQtDirectBackend *>(source_->Backend());
    const int Height = qMax(1, B->scrollArea()->viewport()->height());
    auto Position = B->Send(SCI_GETCURRENTPOS);
    // POINTY is the top of the display row holding the position.
    auto Y = B->Send(SCI_POINTYFROMPOSITION, 0, Position);
    const bool Caret = Y >= 0 && Y < Height;
    if (!Caret)
    {
        if (B->firstVisibleLine() == 0)
            return {{"line", 1}, {"column", 0}, {"top", true}, {"caret", false}, {"end", false}};
        if (B->isScrolledToEnd())
            return {{"line", B->lineCount() + 1}, {"column", 0}, {"top", false}, {"caret", false}, {"end", true}};
        // The nearest visible row keeps the preview continuous; its first character visible
        // at the left edge is the anchor, also when a long unwrapped line is scrolled sideways.
        Position = B->Send(SCI_POSITIONFROMPOINT, B->leftMarginWidth() + 1, qBound(0, int(Y), Height - 1));
        Y = B->Send(SCI_POINTYFROMPOSITION, 0, Position);
    }
    // A space has no rendered counterpart at a soft wrap; a character of the same row stands for it.
    Position = B->RowTextCharacter(Position);
    const int Line = int(B->Send(SCI_LINEFROMPOSITION, Position));
    const int Column = int(B->Send(SCI_COUNTCODEUNITS, B->Send(SCI_POSITIONFROMLINE, Line), Position));
    return {{"line", Line + 1}, {"column", Column}, {"sourceY", double(Y)}, {"offset", previewOffsetForSourceY(double(Y))},
            {"caret", Caret}, {"top", false}, {"end", false}};
}
void MarkdownDocumentView::placeSource(double Line, int Column, double Offset, bool MoveCaret)
{
    auto *B = static_cast<ScintillaQtDirectBackend *>(source_->Backend());
    const int DocLine = qBound(0, int(std::floor(Line)) - 1, qMax(1, source_->lineCount()) - 1);
    const auto Start = B->Send(SCI_POSITIONFROMLINE, DocLine), End = B->Send(SCI_GETLINEENDPOSITION, DocLine);
    const double Y = sourceYForPreviewOffset(Offset);
    const int Height = qMax(1, B->scrollArea()->viewport()->height());
    if (Column < 0)
    {
        // No character to match (an image or a diagram): the line's share of the block.
        if (MoveCaret)
            B->Send(SCI_SETEMPTYSELECTION, quintptr(Start));
        B->scrollFractionalLineToViewportRatio(Line, qBound(0.0, Y / Height, 1.0));
        return;
    }
    auto Position = B->Send(SCI_POSITIONRELATIVECODEUNITS, quintptr(Start), Column);
    if (Position < Start || Position > End)
        Position = End;
    if (MoveCaret)
        B->Send(SCI_SETEMPTYSELECTION, quintptr(Position));
    // Scintilla scrolls whole display rows: the row holding the character goes to the row
    // nearest the wanted height.
    const auto Place = [B, Position, Y] {
        const auto Row = qMax<qintptr>(1, B->Send(SCI_TEXTHEIGHT, B->Send(SCI_LINEFROMPOSITION, quintptr(Position))));
        const auto Now = B->Send(SCI_POINTYFROMPOSITION, 0, Position);
        const auto Rows = qintptr(std::lround((double(Now) - Y) / double(Row)));
        if (Rows != 0)
            B->Send(SCI_SETFIRSTVISIBLELINE, quintptr(qMax<qintptr>(0, B->Send(SCI_GETFIRSTVISIBLELINE) + Rows)));
    };
    const auto Line0 = B->Send(SCI_VISIBLEFROMDOCLINE, quintptr(DocLine));
    const auto Sub = qMax<qintptr>(0, (B->Send(SCI_POINTYFROMPOSITION, 0, Position) - B->Send(SCI_POINTYFROMPOSITION, 0, Start))
                                          / qMax<qintptr>(1, B->Send(SCI_TEXTHEIGHT, DocLine)));
    const auto RowsAbove = qintptr(std::lround(Y / double(qMax<qintptr>(1, B->Send(SCI_TEXTHEIGHT, DocLine)))));
    B->Send(SCI_SETFIRSTVISIBLELINE, quintptr(qMax<qintptr>(0, Line0 + Sub - RowsAbove)));
    // Rows above may wrap once they are shown; correct by the rows they added.
    Place();
    QTimer::singleShot(0, this, [this, Place] { suppress_.restart(); Place(); });
    // Without word wrap the character may sit beside the visible part of a long line.
    if (!source_->isWordWrapEnabled())
        B->Send(SCI_SCROLLRANGE, quintptr(Position), Position);
}
void MarkdownDocumentView::pollFocus()
{
    const bool PreviewFocus = preview_->HasFocus();
    const bool Active = PreviewFocus ? true : source_->Backend()->widget()->hasFocus() ? false : previewActive_;
    if (Active != previewActive_)
    {
        previewActive_ = Active;
        emit Changed();
    }
}
void MarkdownDocumentView::sampleSource()
{
    pollFocus();
    if (!SourceVisible() || preview_->isHidden() || composing_ || preview_->HasFocus()
        || !actions_["markdown.syncScroll"]->isChecked() || renderRequested_ < 0
        || !source_->Backend()->widget()->hasFocus())
        return;
    // The preview has just moved the editor: answering now would bounce. Send the
    // final position once the guard ends.
    if (suppress_.isValid() && suppress_.elapsed() < 300)
    {
        sampleSoon_.start(int(300 - suppress_.elapsed()) + 16);
        return;
    }
    ++anchorSyncs_;
    auto *B = static_cast<ScintillaQtDirectBackend *>(source_->Backend());
    const auto Position = B->Send(SCI_GETCURRENTPOS);
    if (Position != lastPosition_)
    {
        lastPosition_ = Position;
        lastLine_ = source_->currentLine();
        sendPosition(toPreviewLine(lastLine_), false);
        lastSampleSent_.restart();
    }
    const auto Anchor = sourceScrollAnchor();
    if (Anchor != lastSourceAnchor_)
    {
        lastSourceAnchor_ = Anchor;
        sendPosition(Anchor["top"].toBool() || Anchor["end"].toBool() ? Anchor["line"].toDouble() : toPreviewLine(Anchor["line"].toDouble()), true, Anchor);
        lastSampleSent_.restart();
    }
}
void MarkdownDocumentView::RequestDiagnostics(QJsonObject Request)
{
    diagnostics_ = {};
    Request["type"] = "diagnostics";
    preview_->Send(Request);
}
void MarkdownDocumentView::alignPreview()
{
    const int Top = mode_ == "split" ? source_->Backend()->widget()->mapTo(source_, QPoint{}).y() : 0;
    if (previewPanel_->layout()->contentsMargins().top() != Top)
        previewPanel_->layout()->setContentsMargins(0, Top, 0, 0);
}
bool MarkdownDocumentView::eventFilter(QObject *, QEvent *Event)
{
    if (Event->type() == QEvent::Move || Event->type() == QEvent::Resize || Event->type() == QEvent::Show)
    {
        QTimer::singleShot(0, this, &MarkdownDocumentView::alignPreview);
        // Geometry changes the screen height of the anchor row.
        sampleSoon_.start(0);
    }
    if (Event->type() == QEvent::FocusIn || Event->type() == QEvent::FocusOut)
        sampleSoon_.start(0);
    if (Event->type() == QEvent::InputMethod)
        composing_ = !static_cast<QInputMethodEvent *>(Event)->preeditString().isEmpty();
    if (Event->type() == QEvent::KeyPress || Event->type() == QEvent::InputMethod)
        lastInput_.restart();
    return false;
}
QList<DocumentStatusField> MarkdownDocumentView::StatusFields() const
{
    QList<DocumentStatusField> F;
    // Fields are {id, text, compact, detail, command, essential, trailing, picker}. The command runs
    // only where the host offers no picker.
    const auto Percent = [](int Value) { return QStringLiteral("%1 %").arg(Value); };
    if (!SourceVisible())
        F.append({"heading", QStringLiteral("%1 · %2").arg(heading_.isEmpty() ? tr("Markdown") : heading_, Percent(progress_)),
                  Percent(progress_), tr("현재 제목 · 미리보기 위치"), "markdown.currentHeading", true, false, true});
    F.append({"markdown.viewMode", mode_ == "split" ? (previewActive_ ? tr("나란히 · 미리보기") : tr("나란히 · 소스"))
                                   : mode_ == "source" ? tr("소스")
                                                       : tr("미리보기")});
    // In preview the zoom starts the right-aligned group; with the source shown the text fields
    // before these already start it.
    if (mode_ != "source")
        F.append({"mdzoom", tr("미리보기 %1").arg(Percent(PreviewZoomPercent())), Percent(PreviewZoomPercent()),
                  tr("미리보기 배율"), "markdown.zoomSet", true, !SourceVisible(), true});
    F.append({"render", renderText(), {}, renderDetail(), "markdown.refresh", !error_.isEmpty() || problems_ > 0, false, true});
    return F;
}
QString MarkdownDocumentView::renderText() const
{
    if (!error_.isEmpty())
        return tr("렌더링 실패");
    switch (render_)
    {
    case RenderState::Waiting:
        return tr("렌더링 대기");
    case RenderState::Rendering:
        return tr("렌더링 중");
    case RenderState::Manual:
        return tr("렌더링 수동 갱신");
    case RenderState::Current:
    case RenderState::Partial:
        break;
    }
    if (problems_ > 0)
        return tr("렌더링 문제 %1건").arg(problems_);
    return render_ == RenderState::Partial ? tr("렌더링 최신 · 부분") : tr("렌더링 최신");
}
QString MarkdownDocumentView::renderDetail() const
{
    if (!error_.isEmpty())
        return error_;
    if (render_ == RenderState::Manual)
        return tr("큰 문서는 자동으로 갱신하지 않습니다. 미리보기 갱신으로 렌더링합니다.");
    if ((render_ == RenderState::Current || render_ == RenderState::Partial) && problems_ > 0)
        return tr("수식 · 다이어그램 오류 %1건").arg(problems_);
    return renderedAt_.isValid() ? tr("미리보기 렌더링 — %1 · %2").arg(status_, QLocale().toString(renderedAt_.time(), QLocale::ShortFormat))
                                 : tr("미리보기 렌더링 — %1").arg(status_);
}
QList<QPair<QString, int>> MarkdownDocumentView::Headings() const
{
    QList<QPair<QString, int>> Items;
    for (QTreeWidgetItemIterator It(outline_); *It; ++It)
        Items.append({(*It)->text(0), int(fromPreviewLine((*It)->data(0, Qt::UserRole).toInt()))});
    return Items;
}
int MarkdownDocumentView::CurrentHeadingIndex() const
{
    int Index = 0;
    for (QTreeWidgetItemIterator It(outline_); *It; ++It, ++Index)
        if (*It == outline_->currentItem())
            return Index;
    return -1;
}
int MarkdownDocumentView::HeadingDepth(int Index) const
{
    int At = 0;
    for (QTreeWidgetItemIterator It(outline_); *It; ++It, ++At)
        if (At == Index)
        {
            int Depth = 0;
            for (auto *Parent = (*It)->parent(); Parent; Parent = Parent->parent())
                ++Depth;
            return Depth;
        }
    return 0;
}
void MarkdownDocumentView::GoToHeading(int Index)
{
    int At = 0;
    for (QTreeWidgetItemIterator It(outline_); *It; ++It, ++At)
        if (At == Index)
        {
            {
                const QSignalBlocker Block(outline_);
                outline_->setCurrentItem(*It);
            }
            goToHeadingLine((*It)->data(0, Qt::UserRole).toInt());
            return;
        }
}
void MarkdownDocumentView::goToHeadingLine(int Line)
{
    if (rendered_ != revision_)
    {
        Refresh();
        // An undo back to the rendered text needs no render: the preview is current again.
        if (rendered_ != revision_)
            return;
    }
    if (SourceVisible() && (!previewActive_ || actions_["markdown.syncScroll"]->isChecked()))
        source_->goToLine(int(fromPreviewLine(Line)));
    // The preview always shows the heading: with the source hidden or unfocused no anchor follows.
    if (!preview_->isHidden() &&
        (!SourceVisible() || previewActive_ || actions_["markdown.syncScroll"]->isChecked()))
        sendPosition(Line, false, {}, true);
}
void MarkdownDocumentView::ShowSourceLineInPreview()
{
    if (mode_ == "source")
        mode("split");
    // A preview shown just now has no layout yet; place it once the splitter has settled.
    QTimer::singleShot(0, this, [this] {
        if (renderRequested_ < 0 || preview_->isHidden())
            return;
        const auto Anchor = sourceScrollAnchor();
        const bool Caret = Anchor["caret"].toBool();
        // Marks the caret's block; without a visible caret it also scrolls to it.
        sendPosition(toPreviewLine(source_->currentLine()), false, {}, !Caret);
        if (Caret)
        {
            // The caret's character goes to the same screen height in the preview.
            lastSourceAnchor_ = Anchor;
            sendPosition(toPreviewLine(Anchor["line"].toDouble()), true, Anchor);
        }
    });
}
void MarkdownDocumentView::showInSource(double Line, int Column, double Offset)
{
    const bool Shown = SourceVisible();
    if (!Shown)
        mode("split");
    const auto Place = [this, Line, Column, Offset] {
        // As a click in the preview: the character keeps its screen height, and the editor's own
        // anchor is not sent back.
        suppress_.restart();
        placeSource(fromPreviewLine(Line), Column, Offset, true);
        lastSourceAnchor_ = sourceScrollAnchor();
        lastPosition_ = static_cast<ScintillaQtDirectBackend *>(source_->Backend())->Send(SCI_GETCURRENTPOS);
        focusSource();
    };
    if (Shown)
        Place();
    else
        QTimer::singleShot(0, this, Place);
}
void MarkdownDocumentView::showPreviewMenu(const QJsonObject &Value)
{
    if (preview_->isHidden())
        return;
    const double X = Value["x"].toDouble(), Y = Value["y"].toDouble();
    const auto Global = preview_->mapToGlobal(QPoint(qRound(X * zoom_), qRound(Y * zoom_)));
    const auto Link = Value["link"].toString(), Fragment = Value["fragment"].toString();
    double Line = Value["line"].toDouble(), Offset = Y;
    int Column = Value["column"].isDouble() ? Value["column"].toInt() : -1;
    if (Line < 1)
    {
        // Outside the document: the preview's own anchor at mid height stands in.
        Line = previewScroll_["line"].toDouble();
        Column = previewScroll_["column"].isDouble() ? previewScroll_["column"].toInt() : -1;
        Offset = previewScroll_["anchorOffset"].toDouble(Y);
    }
    const auto Key = [this](const char *Id) {
        return QKeySequence::fromString(bindings_.value(QString::fromLatin1(Id)).toString(), QKeySequence::PortableText);
    };
    const QUrl Url(Link);
    const bool HadFocus = preview_->HasFocus();
    QMenu Menu(this);
    Menu.setObjectName("markdownPreviewMenu");
    Menu.addAction(tr("복사"), Key("edit.copy"), this, [this] { preview_->Send({{"type", "copy"}}); })
        ->setEnabled(!Value["selection"].toString().isEmpty());
    Menu.addAction(tr("전체 선택"), Key("edit.selectAll"), this, [this] { preview_->Send({{"type", "selectAll"}}); });
    Menu.addSeparator();
    auto *Source = Menu.addAction(tr("소스에서 이 위치 보기"), this, [this, Line, Column, Offset] { showInSource(Line, Column, Offset); });
    Source->setEnabled(Line >= 1 && rendered_ >= 0);
    auto *Open = Menu.addAction(tr("링크 열기"), this, [this, Link, Fragment] { openLink(Link, Fragment); });
    Open->setEnabled(!Fragment.isEmpty() || Url.scheme() == "https" || Url.scheme() == "http" || Url.isLocalFile());
    Menu.addAction(tr("링크 주소 복사"), this, [Link] { QGuiApplication::clipboard()->setText(Link); })->setEnabled(!Link.isEmpty());
    Menu.addSeparator();
    for (const auto &[Id, Label] : {std::pair{"markdown.zoomIn", tr("미리보기 확대")}, std::pair{"markdown.zoomOut", tr("미리보기 축소")},
                                    std::pair{"markdown.zoomReset", tr("미리보기 원래 크기")}, std::pair{"markdown.refresh", tr("미리보기 갱신")}})
        Menu.addAction(Label, Key(Id), this, [this, Command = QString::fromLatin1(Id)] { Run(Command); })
            ->setEnabled(CanRun(QString::fromLatin1(Id)));
    Menu.addSeparator();
    for (const auto &[Id, Label] : {std::pair{"markdown.toggleToc", tr("개요")}, std::pair{"markdown.syncScroll", tr("스크롤 동기화")}})
    {
        auto *Own = actions_[QString::fromLatin1(Id)];
        // The toolbar action owns the state and its saved setting.
        auto *Item = Menu.addAction(Label, Key(Id), Own, [Own] { Own->trigger(); });
        Item->setCheckable(true);
        Item->setChecked(Own->isChecked());
    }
    const QPointer<MarkdownDocumentView> Self(this);
    auto *Chosen = Menu.exec(Global);
    // The menu took the keyboard from the preview window; give it back unless the choice moved on.
    if (Self && HadFocus && Chosen != Source && Chosen != Open && !preview_->isHidden())
        preview_->FocusInput();
}
void MarkdownDocumentView::showOutlineMenu(const QPoint &Position)
{
    // A render may rebuild the outline while the menu is open: keep values, not the item.
    const auto *Item = outline_->itemAt(Position);
    const int Line = Item ? Item->data(0, Qt::UserRole).toInt() : 0;
    const auto Text = Item ? Item->text(0) : QString();
    QMenu Menu(this);
    Menu.setObjectName("markdownOutlineMenu");
    Menu.addAction(tr("이 제목으로 이동"), this, [this, Line] { goToHeadingLine(Line); })->setEnabled(Item != nullptr);
    Menu.addSeparator();
    Menu.addAction(tr("모두 펼치기"), this, [this] { outline_->expandAll(); });
    Menu.addAction(tr("모두 접기"), this, [this] { outline_->collapseAll(); });
    Menu.addSeparator();
    Menu.addAction(tr("제목 텍스트 복사"), this, [Text] { QGuiApplication::clipboard()->setText(Text); })->setEnabled(Item != nullptr);
    Menu.exec(outline_->viewport()->mapToGlobal(Position));
}
QJsonObject MarkdownDocumentView::State() const
{
    return {{"mode", mode_},
            {"revision", revision_},
            {"renderedRevision", rendered_},
            {"editRenders", editRenders_},
            {"blocks", renderBlocks_},
            {"resyncs", resyncs_},
            {"status", status_},
            {"previewScroll", previewScroll_},
            {"sourceAnchor", sourceScrollAnchor()},
            {"problems", problems_},
            {"zoom", PreviewZoomPercent()},
            {"scrollSync", actions_["markdown.syncScroll"]->isChecked()},
            {"requestedRevision", renderRequested_},
            {"anchorSyncs", anchorSyncs_},
            {"scrollRequests", scrollSerial_},
            {"lineShifts", shifts_.size()},
            {"lineShiftsOverflow", shiftsOverflow_},
            {"diagnostics", diagnostics_},
            {"error", error_}};
}
} // namespace hc::document
