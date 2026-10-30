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
    Make("markdown.positionSync", tr("위치 동기화"), true)->setToolTip(tr("소스 캐럿·본문 클릭 위치를 연결합니다."));
    Make("markdown.syncScroll", tr("스크롤 동기화"), true)
        ->setToolTip(tr("소스·본문 화면 이동을 연결합니다. 위치 동기화가 켜져 있어야 합니다."));
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
    preview_ = new MarkdownProcessView(splitter_);
    splitter_->addWidget(preview_);
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
        const int Line = Item->data(0, Qt::UserRole).toInt();
        if (SourceVisible() && (!previewActive_ || actions_["markdown.positionSync"]->isChecked()))
            source_->goToLine(Line);
        if (!preview_->isHidden() &&
            (!SourceVisible() || previewActive_ || actions_["markdown.positionSync"]->isChecked()))
            sendPosition(Line, false);
    });
    debounce_.setSingleShot(true);
    connect(&debounce_, &QTimer::timeout, this, [this] { Refresh(); });
    connect(source_, &QTextView::ContentChanged, this, [this] {
        ++revision_;
        status_ = tr("갱신 대기");
        if (options_["autoPreview"].toBool(true))
            debounce_.start();
        emit Changed();
    });
    connect(preview_, &MarkdownProcessView::Reported, this, &MarkdownDocumentView::report);
    source_->Backend()->widget()->installEventFilter(this);
    sample_.setInterval(80);
    connect(&sample_, &QTimer::timeout, this, &MarkdownDocumentView::sampleSource);
    sample_.start();
    ReloadPreferences();
    AppSettings S;
    splitter_->restoreState(QByteArray::fromBase64(
        S.value(QString("markdown/layout/") + (editor_ ? "editor" : "viewer")).toString().toLatin1()));
    mode(S.value(QString("markdown/") + (editor_ ? "editorMode" : "viewerMode"), editor_ ? "split" : "preview")
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
    InputWidget()->setFocus();
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
    preview_->setVisible(mode_ != "source");
    for (const auto &Id : {"source", "preview", "split"})
        actions_["markdown." + QString(Id)]->setChecked(mode_ == Id);
    syncControls();
    emit Changed();
}
void MarkdownDocumentView::syncControls()
{
    actions_["markdown.syncScroll"]->setEnabled(actions_["markdown.positionSync"]->isChecked());
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
        mode(Id.section('.', 1));
    else if (Id == "markdown.toggleSource")
        mode(SourceVisible() ? "preview" : "source");
    else if (Id == "markdown.focusOtherPane")
    {
        if (mode_ == "split")
        {
            if (preview_->HasFocus())
                source_->Backend()->widget()->setFocus();
            else
                preview_->setFocus();
        }
    }
    else if (Id == "markdown.toggleToc")
    {
        syncControls();
        saveToggle(editor_ ? "editorOutline" : "viewerOutline", actions_[Id]->isChecked());
    }
    else if (Id == "markdown.positionSync" || Id == "markdown.syncScroll")
    {
        syncControls();
        saveToggle(Id == "markdown.positionSync" ? "positionSync" : "scrollSync", actions_[Id]->isChecked());
        lastLine_ = lastTop_ = -1;
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
    }
    emit Changed();
    return true;
}
void MarkdownDocumentView::ReloadPreferences()
{
    AppSettings S;
    options_ = {};
    for (const auto &Key : {"math", "mermaid", "inlineDollar", "codeHighlight", "autoPreview"})
        options_[Key] = S.value("markdown/" + QString(Key), true).toBool();
    options_["remoteImages"] = S.value("markdown/remoteImages", false).toBool();
    QJsonObject Fonts;
    for (const auto &Pair : {std::pair{"enabled", QVariant(false)}, std::pair{"mode", QVariant("document")},
                             std::pair{"bodyFamily", QVariant("맑은 고딕")}, std::pair{"bodySizePt", QVariant(11)},
                             std::pair{"codeFamily", QVariant("Consolas")}, std::pair{"codeSizePt", QVariant(10)},
                             std::pair{"lineHeight", QVariant(1.7)}, std::pair{"headings", QVariant(true)}})
        Fonts[Pair.first] =
            QJsonValue::fromVariant(S.value("markdown/previewFonts/" + QString(Pair.first), Pair.second));
    options_["previewFonts"] = Fonts;
    for (const auto &Pair :
         {std::pair{"markdown.positionSync", "positionSync"}, std::pair{"markdown.syncScroll", "scrollSync"}})
    {
        const QSignalBlocker B(actions_[Pair.first]);
        actions_[Pair.first]->setChecked(S.value("markdown/" + QString(Pair.second), true).toBool());
    }
    actions_["markdown.toggleToc"]->setChecked(
        S.value(editor_ ? "markdown/editorOutline" : "markdown/viewerOutline", editor_).toBool());
    syncControls();
    debounce_.setInterval(qBound(100, S.value("markdown/debounceMs", 250).toInt(), 3000));
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
    error_.clear();
    message_->hide();
    status_ = source_->isContentTruncated() ? tr("부분 미리보기 갱신 중") : tr("갱신 중");
    options_["dark"] = palette().color(QPalette::Window).lightness() < 128;
    preview_->Send({{"type", "render"},
                    {"revision", revision_},
                    {"text", Text},
                    {"path", path_},
                    {"baseUrl", QUrl::fromLocalFile(QFileInfo(path_).absolutePath() + "/").toString()},
                    {"options", options_}});
    emit Changed();
}
void MarkdownDocumentView::report(const QJsonObject &Value)
{
    const auto Type = Value["type"].toString();
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
    if (Type != "error" && Value["revision"].toInteger(-1) != revision_)
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
        rendered_ = revision_;
        status_ = source_->isContentTruncated() ? tr("부분 미리보기") : tr("최신");
        problems_ = Value["problems"].toInt();
        problemDetails_ = Value["details"].toArray();
        outline_->clear();
        QList<QTreeWidgetItem *> Parents(7, nullptr);
        AppSettings S;
        const int Depth = qBound(1, S.value("markdown/outlineDepth", 6).toInt(), 6);
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
        outline_->expandAll();
        if(auto* Filter=findChild<QLineEdit*>("markdownOutlineFilter"))QMetaObject::invokeMethod(Filter,"textChanged",Qt::DirectConnection,Q_ARG(QString,Filter->text()));
    }
    else if (Type == "position" && actions_["markdown.positionSync"]->isChecked() && SourceVisible() && !composing_)
    {
        suppress_.restart();
        source_->goToLine(Value["line"].toInt());
    }
    else if (Type == "scroll")
    {
        progress_ = Value["progress"].toInt();
        heading_ = Value["heading"].toString();
        for (QTreeWidgetItemIterator It(outline_); *It; ++It)
            if ((*It)->data(0, Qt::UserRole).toInt() == Value["headingLine"].toInt())
            {
                const QSignalBlocker Block(outline_);
                outline_->setCurrentItem(*It);
                break;
            }
        if (Value["user"].toBool() && actions_["markdown.positionSync"]->isChecked() &&
            actions_["markdown.syncScroll"]->isChecked() && SourceVisible() && !composing_ &&
            (!suppress_.isValid() || suppress_.elapsed() > 300))
        {
            auto *B = static_cast<ScintillaQtDirectBackend *>(source_->Backend());
            const int Line = qBound(0, Value["line"].toInt() - 1, source_->lineCount() - 1);
            suppress_.restart();
            const int Display = int(B->Send(SCI_VISIBLEFROMDOCLINE, Line));
            B->Send(SCI_SETFIRSTVISIBLELINE,
                    qMax(0, Display - int(Value["ratio"].toDouble() * B->Send(SCI_LINESONSCREEN))));
            lastTop_ = int(B->Send(SCI_GETFIRSTVISIBLELINE));
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
void MarkdownDocumentView::sendPosition(int Line, bool Scroll, double Ratio)
{
    if (rendered_ != revision_)
        return;
    preview_->Send({{"type", Scroll ? "scroll" : "position"},
                    {"revision", revision_},
                    {"line", Line},
                    {"ratio", Ratio},
                    {"nearest", actions_["markdown.syncScroll"]->isChecked()}});
}
void MarkdownDocumentView::sampleSource()
{
    const bool Active = preview_->HasFocus() ? true : source_->Backend()->widget()->hasFocus() ? false : previewActive_;
    if (Active != previewActive_)
    {
        previewActive_ = Active;
        emit Changed();
    }
    if (!SourceVisible() || preview_->isHidden() || composing_ || !actions_["markdown.positionSync"]->isChecked() ||
        rendered_ != revision_ || !source_->Backend()->widget()->hasFocus() ||
        (suppress_.isValid() && suppress_.elapsed() < 300))
        return;
    auto *B = static_cast<ScintillaQtDirectBackend *>(source_->Backend());
    const int Line = source_->currentLine(), Top = int(B->Send(SCI_GETFIRSTVISIBLELINE));
    if (Line != lastLine_)
    {
        lastLine_ = Line;
        sendPosition(Line, false);
    }
    if (Top != lastTop_)
    {
        lastTop_ = Top;
        if (actions_["markdown.syncScroll"]->isChecked())
            sendPosition(int(B->Send(SCI_DOCLINEFROMVISIBLE, Top)) + 1, true, 0);
    }
}
bool MarkdownDocumentView::eventFilter(QObject *, QEvent *Event)
{
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
            {"problems", problems_},
            {"positionSync", actions_["markdown.positionSync"]->isChecked()},
            {"scrollSync", actions_["markdown.syncScroll"]->isChecked()},
            {"error", error_}};
}
} // namespace hc::document
