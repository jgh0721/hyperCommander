#include "stdafx.h"
#include "markdownDocumentView.hpp"
#include "markdownProcessView.hpp"
#include "documentShortcutRegistry.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "core/AppSettings.h"
#include "core/ToolbarIcons.h"
#include <Scintilla.h>
#include <QActionGroup>
#include <QToolBar>
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
    toolbar_ = new QToolBar(tr("마크다운"), this);
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto Make = [&](const QString &Id, const QString &Label, bool Check = false) {
        auto *A = toolbar_->addAction(Label);
        A->setObjectName(Id);
        A->setCheckable(Check);
        actions_[Id] = A;
        connect(A, &QAction::triggered, this, [this, Id] { Run(Id); });
        return A;
    };
    auto *Modes = new QActionGroup(this);
    for (const auto &Pair : {std::pair{"preview", tr("미리보기")}, std::pair{"source", tr("소스 편집")},
                             std::pair{"split", tr("나란히 보기")}})
    {
        auto *A = Make("markdown." + QString(Pair.first), Pair.second, true);
        A->setIcon(ToolbarIcons::iconForActionKey("markdown." + QString(Pair.first), __FILE__, __LINE__));
        Modes->addAction(A);
    }
    toolbar_->addSeparator();
    Make("markdown.toggleToc", tr("개요"), true);
    Make("markdown.syncScroll", tr("스크롤 동기화"), true)
        ->setToolTip(tr("소스 캐럿·본문 클릭과 화면 스크롤을 함께 연결합니다."));
    Make("markdown.refresh", tr("갱신"));
    toolbar_->removeAction(Make("markdown.focusOtherPane", tr("포커스 이동")));
    toolbar_->removeAction(Make("markdown.toggleSource", tr("소스 전환")));
    toolbar_->removeAction(Make("markdown.problems", tr("렌더링 문제")));
    toolbar_->removeAction(Make("markdown.currentHeading", tr("현재 개요 항목")));
    Make("markdown.zoomOut", tr("축소"));
    Make("markdown.zoomIn", tr("확대"));
    Make("markdown.find", tr("찾기"));
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
    connect(outline_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *Item) {
        if (rendered_ != revision_)
        {
            Refresh();
            return;
        }
        // Outline lines come from the rendered revision.
        const int Line = Item->data(0, Qt::UserRole).toInt();
        if (SourceVisible() && (!previewActive_ || actions_["markdown.syncScroll"]->isChecked()))
            source_->goToLine(int(fromPreviewLine(Line)));
        if (!preview_->isHidden() &&
            (!SourceVisible() || previewActive_ || actions_["markdown.syncScroll"]->isChecked()))
            sendPosition(Line, false);
    });
    debounce_.setSingleShot(true);
    connect(&debounce_, &QTimer::timeout, this, [this] { Refresh(); });
    connect(source_, &QTextView::ContentChanged, this, [this] {
        ++revision_;
        status_ = tr("갱신 대기");
        if (options_["autoPreview"].toBool(hc::defaults::edit::ViewValue("markdown/autoPreview").toBool()))
            debounce_.start();
        emit Changed();
    });
    connect(preview_, &MarkdownProcessView::Reported, this, &MarkdownDocumentView::report);
    source_->Backend()->widget()->installEventFilter(this);
    // The anchor follows the editor's own caret and viewport signals; several in one event
    // loop turn are sent once. Nothing is computed while the editor is idle.
    sampleSoon_.setSingleShot(true);
    connect(&sampleSoon_, &QTimer::timeout, this, &MarkdownDocumentView::sampleSource);
    connect(source_->Backend(), &ITextEditorBackend::cursorPositionChanged, this, [this] { sampleSoon_.start(0); });
    connect(source_->Backend(), &ITextEditorBackend::viewportScrolled, this, [this] { sampleSoon_.start(0); });
    connect(source_->Backend(), &ITextEditorBackend::linesShifted, this, [this](int Line, int Delta) {
        if (shifts_.size() < 256) shifts_.append({Line, Delta}); else shiftsOverflow_ = true;
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
    actions_["markdown.source"]->setText(Value ? tr("소스 보기") : tr("소스 편집"));
}
void MarkdownDocumentView::Focus()
{
    auto* Input = InputWidget();
    if (Input == preview_) preview_->FocusInput();
    else {
        Input->setFocus(Qt::OtherFocusReason);
        const auto Root = GetAncestor(reinterpret_cast<HWND>(window()->effectiveWinId()), GA_ROOT);
        if (Root && GetAncestor(GetForegroundWindow(), GA_ROOT) == Root)
            SetFocus(reinterpret_cast<HWND>(Input->effectiveWinId()));
    }
}
void MarkdownDocumentView::SetSourceTools(QToolBar *Tools)
{
    sourceTools_ = Tools;
    static_cast<QVBoxLayout *>(layout())->insertWidget(0, Tools);
    Tools->setVisible(SourceVisible());
}
void MarkdownDocumentView::SetShortcutBindings(const QJsonObject &Bindings)
{
    QJsonObject Keys;
    for (const auto &Item : DocumentShortcutRegistry::Commands())
        if (Item.scope_ == "markdown" || Item.scope_ == "common")
            Keys[Item.id_] = Bindings[Item.id_];
    Keys["markdown.find"] = Bindings["text.find"];
    preview_->Send({{"type", "shortcuts"}, {"bindings", Keys}});
}
void MarkdownDocumentView::mode(const QString &Value)
{
    mode_ = QStringList{"source", "preview", "split"}.contains(Value) ? Value : "preview";
    source_->setVisible(SourceVisible());
    if (sourceTools_)
        sourceTools_->setVisible(SourceVisible());
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
    if (SourceVisible() && !preview_->HasFocus())
        return false;
    if (Id == "edit.copy" || Id == "edit.selectAll")
        preview_->Send({{"type", Id == "edit.copy" ? "copy" : "selectAll"}});
    else if (Id == "text.find")
        Run("markdown.find");
    return Id.startsWith("edit.") || Id.startsWith("text.");
}
bool MarkdownDocumentView::Run(const QString &Id)
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
        if (SourceVisible() && !preview_->HasFocus() && source_->Backend()->widget()->hasFocus())
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
    {
        zoom_ = qBound(.5, zoom_ + (Id.endsWith("In") ? .1 : -.1), 3.);
        preview_->Send({{"type", "zoom"}, {"value", zoom_}});
        // Offsets are CSS pixels; the same screen height is a different offset now.
        lastSourceAnchor_ = {};sampleSoon_.start(0);
    }
    emit Changed();
    return true;
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
    debounce_.setInterval(qBound(100, S.value("markdown/debounceMs").toInt(), 3000));
    if (!mode_.isEmpty())
        Refresh();
}
void MarkdownDocumentView::Refresh(bool Force)
{
    debounce_.stop();
    const auto Text = source_->Content();
    if (!Force && Text.size() > 2 * 1024 * 1024)
    {
        status_ = tr("대용량 · 수동 갱신 필요");
        emit Changed();
        return;
    }
    if (Text.toUtf8().size() > 8 * 1024 * 1024)
    {
        error_ = tr("미리보기는 8 MiB 이하 문서를 지원합니다. 소스 편집은 계속할 수 있습니다.");
        message_->setText(error_);
        message_->show();
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
    options_["dark"] = palette().color(QPalette::Window).lightness() < 128;
    preview_->Send({{"type", "render"},
                    {"revision", revision_},
                    {"text", Text},
                    {"path", path_},
                    {"baseUrl", QUrl::fromLocalFile(QFileInfo(path_).absolutePath() + "/").toString()},
                    {"options", options_},
                    {"scroll", SourceVisible() && actions_["markdown.syncScroll"]->isChecked()
                        && source_->Backend()->widget()->hasFocus() && !preview_->HasFocus() ? sourceScrollAnchor() : QJsonObject{}}});
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
    // Reports describe the requested revision. Edits made since then are mapped
    // through shifts_, so the preview stays synchronised while it waits for a render.
    if (Type != "error" && Value["revision"].toInteger(-1) != renderRequested_)
        return;
    if (Type == "error")
    {
        error_ = Value["message"].toString();
        status_ = tr("갱신 실패");
        message_->setText(error_);
        message_->show();
    }
    else if (Type == "rendered")
    {
        rendered_ = renderRequested_;
        status_ = renderRequested_ != revision_ ? tr("갱신 대기") : source_->isContentTruncated() ? tr("부분 미리보기") : tr("최신");
        // The new frame has its own layout: send the current anchor to it.
        lastSourceAnchor_ = {};
        lastPosition_ = -1;
        sampleSoon_.start(0);
        problems_ = Value["problems"].toInt();
        problemDetails_ = Value["details"].toArray();
        outline_->clear();
        QList<QTreeWidgetItem *> Parents(7, nullptr);
        AppSettings S;
        const int Depth = qBound(1, S.value("markdown/outlineDepth").toInt(), 6);
        for (const auto V : Value["headings"].toArray())
        {
            const auto H = V.toObject();
            const int Level = qBound(1, H["level"].toInt(), 6);
            if (Level > Depth)
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
        outline_->expandAll();
        if(auto* Filter=findChild<QLineEdit*>("markdownOutlineFilter"))QMetaObject::invokeMethod(Filter,"textChanged",Qt::DirectConnection,Q_ARG(QString,Filter->text()));
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
    {
        const QUrl Url(Value["url"].toString());
        if (Url.scheme() == "https" || Url.scheme() == "http")
            QDesktopServices::openUrl(Url);
        else if (Url.isLocalFile())
            emit OpenRequested(Url.toLocalFile());
    }
    emit Changed();
}
void MarkdownDocumentView::sendPosition(double Line, bool Scroll, const QJsonObject &Anchor)
{
    if (renderRequested_ < 0)
        return;
    QJsonObject Message{{"type", Scroll ? "scroll" : "position"},
                        {"revision", renderRequested_},
                        {"line", Line},
                        {"reveal", !actions_["markdown.syncScroll"]->isChecked()}};
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
    }
    const auto Anchor = sourceScrollAnchor();
    if (Anchor != lastSourceAnchor_)
    {
        lastSourceAnchor_ = Anchor;
        sendPosition(Anchor["top"].toBool() || Anchor["end"].toBool() ? Anchor["line"].toDouble() : toPreviewLine(Anchor["line"].toDouble()), true, Anchor);
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
    return false;
}
QList<DocumentStatusField> MarkdownDocumentView::StatusFields() const
{
    QList<DocumentStatusField> F;
    if (!SourceVisible())
        F.append({"markdown.location",
                  QString("%1 · %2%").arg(heading_.isEmpty() ? tr("Markdown") : heading_).arg(progress_),
                  {},
                  {},
                  "markdown.currentHeading"});
    F.append({"markdown.viewMode", mode_ == "split" ? (previewActive_ ? tr("나란히 · 미리보기") : tr("나란히 · 소스"))
                                   : mode_ == "source" ? tr("소스")
                                                       : tr("미리보기")});
    if (mode_ != "source")
        F.append({"markdown.zoom", tr("미리보기 %1%").arg(qRound(zoom_ * 100)), {}, {}, "markdown.zoomIn", false});
    F.append({"markdown.render", status_, {}, error_, "markdown.refresh", !error_.isEmpty()});
    if (problems_)
        F.append({"markdown.problems",
                  tr("수식·다이어그램 오류 %1건").arg(problems_),
                  {},
                  tr("클릭하여 문제와 소스 위치를 확인하세요."),
                  "markdown.problems",
                  true});
    return F;
}
QJsonObject MarkdownDocumentView::State() const
{
    return {{"mode", mode_},
            {"revision", revision_},
            {"renderedRevision", rendered_},
            {"status", status_},
            {"previewScroll", previewScroll_},
            {"sourceAnchor", sourceScrollAnchor()},
            {"problems", problems_},
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
