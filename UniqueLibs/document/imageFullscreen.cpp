#include "stdafx.h"
#include "imageFullscreen.hpp"
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include <QDir>
#include "imageReadingView.hpp"
#include "imageReadingNavigator.hpp"
#include "documentStatusBar.hpp"
#include "core/AppSettings.h"
#include <QApplication>
#include <QAction>
#include <QActionEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace hc::document
{
namespace
{
// Capsule metrics follow the bottom navigator: 36 px round buttons on the same dark glass.
// The widget keeps a transparent margin around the glass for its drop shadow.
constexpr int Shadow = 8, Margin = 10, GlassTop = 12, ButtonSize = 36, NameWidth = 220;
QIcon LightIcon(const QIcon &Source)
{
    if (Source.isNull())
        return {};
    // Shared actions carry theme icons; the capsule is always dark, so their ink is redrawn light.
    QIcon Icon;
    for (const qreal Scale : {1.0, 2.0, 3.0})
    {
        QPixmap Pixmap = Source.pixmap(QSize(18, 18), Scale);
        if (Pixmap.isNull())
            continue;
        if (Pixmap.hasAlphaChannel())
        {
            QPainter Painter(&Pixmap);
            Painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            Painter.fillRect(QRectF(QPointF(), Pixmap.deviceIndependentSize()), QColor(0xf1, 0xf4, 0xf8));
        }
        Icon.addPixmap(Pixmap);
    }
    return Icon;
}
class OverlayCapsule final : public QWidget
{
  public:
    explicit OverlayCapsule(QWidget *Parent) : QWidget(Parent)
    {
    }
    std::function<void()> relayout_;

  protected:
    bool event(QEvent *Event) override
    {
        const bool Result = QWidget::event(Event);
        // Button text follows the shared actions; keep the capsule centred when its width changes.
        if (Event->type() == QEvent::LayoutRequest && relayout_)
            relayout_();
        return Result;
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter Painter(this);
        const QRectF Glass = QRectF(rect()).adjusted(Margin, Margin, -Margin, -Margin);
        ImageReadingNavigator::PaintOverlay(Painter, Glass, Glass.height() / 2, Shadow);
    }
};
// A capsule button. A shared action keeps driving text, check and menu; the icon is redrawn light and a
// trailing hint (menu caret or key) is painted in the right padding reserved for it.
class OverlayButton final : public QToolButton
{
  public:
    OverlayButton(QWidget *Parent, bool Labelled) : QToolButton(Parent), labelled_(Labelled)
    {
        setFocusPolicy(Qt::TabFocus);
        setIconSize(QSize(18, 18));
        setToolButtonStyle(Labelled ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
        setProperty("overlayText", Labelled);
        if (Labelled)
            setFixedHeight(ButtonSize);
        else
            setFixedSize(ButtonSize, ButtonSize);
    }
    void Bind(QAction *Action)
    {
        setDefaultAction(Action);
        restyle();
    }
    void SetHint(const QString &Hint)
    {
        hint_ = Hint;
        restyle();
    }

  protected:
    void actionEvent(QActionEvent *Event) override
    {
        QToolButton::actionEvent(Event);
        // QToolButton re-applies the action (icon included) on every change.
        if (Event->type() == QEvent::ActionChanged && Event->action() == defaultAction())
            restyle();
    }
    void paintEvent(QPaintEvent *Event) override
    {
        QToolButton::paintEvent(Event);
        const auto Hint = hint();
        if (Hint.isEmpty())
            return;
        QPainter Painter(this);
        Painter.setPen(isEnabled() ? QColor(0xa4, 0xac, 0xb6) : QColor(0x6c, 0x73, 0x7c));
        Painter.drawText(rect().adjusted(0, 0, -12, 0), Qt::AlignRight | Qt::AlignVCenter, Hint);
    }

  private:
    QString hint() const
    {
        if (!hint_.isEmpty())
            return hint_;
        return labelled_ && defaultAction() && defaultAction()->menu() ? QStringLiteral("▾") : QString();
    }
    void restyle()
    {
        if (auto *Action = defaultAction())
        {
            if (Action->menu())
                setPopupMode(QToolButton::InstantPopup);
            if (Action->icon().cacheKey() != iconKey_)
            {
                iconKey_ = Action->icon().cacheKey();
                light_ = LightIcon(Action->icon());
            }
            setIcon(light_);
        }
        const auto Hint = hint();
        const int Padding = Hint.isEmpty() ? 0 : fontMetrics().horizontalAdvance(Hint) + 18;
        if (Padding != padding_)
        {
            padding_ = Padding;
            setStyleSheet(Padding ? QStringLiteral("padding-right: %1px;").arg(Padding) : QString());
        }
    }
    bool labelled_;
    QString hint_;
    QIcon light_;
    qint64 iconKey_ = 0;
    int padding_ = 0;
};
QFrame *Separator(QWidget *Parent)
{
    auto *Line = new QFrame(Parent);
    Line->setObjectName("imageFullscreenSeparator");
    Line->setFixedSize(1, 22);
    return Line;
}
} // namespace
ImageFullscreen::ImageFullscreen(QWidget *Workspace, std::function<ImageReadingView *()> Reader, std::function<void(int)> Navigate)
    : QObject(Workspace), workspace_(Workspace), reader_(std::move(Reader)), navigate_(std::move(Navigate))
{
    // One capsule at the top centre: book · image name | reading tools | ⋯ 표시 설정 | 전체 화면 해제.
    // Paging belongs to the bottom navigator; previous/next only appear when there is none.
    auto *Capsule = new OverlayCapsule(Workspace);
    top_ = Capsule;
    top_->setObjectName("imageFullscreenControls");
    top_->setAccessibleName(tr("전체 화면 도구"));
    top_->setStyleSheet(ImageReadingNavigator::OverlayStyleSheet(top_->objectName()) +
                        QStringLiteral("#imageFullscreenControls QToolButton[overlayText=\"true\"] { padding: 0px 12px 0px 9px; }"
                                       "#imageFullscreenControls QToolButton::menu-indicator { image: none; width: 0px; }"
                                       "#imageFullscreenControls QToolButton#fullscreenExit { background: #303741; }"
                                       "#imageFullscreenControls QLabel#imageFullscreenItem { color: #a4acb6; }"
                                       "#imageFullscreenControls QFrame#imageFullscreenSeparator { background: rgba(255, 255, 255, 40); border: none; }"));
    top_->hide();
    auto *Row = new QHBoxLayout(top_);
    Row->setContentsMargins(Margin + 10, Margin + 6, Margin + 6, Margin + 6);
    Row->setSpacing(6);
    Row->setSizeConstraint(QLayout::SetNoConstraint);
    name_ = new QWidget(top_);
    name_->setObjectName("imageFullscreenName");
    auto *Names = new QVBoxLayout(name_);
    Names->setContentsMargins(0, 0, 4, 0);
    Names->setSpacing(0);
    book_ = new QLabel(name_);
    book_->setObjectName("imageFullscreenBook");
    QFont Bold = book_->font();
    Bold.setWeight(QFont::DemiBold);
    book_->setFont(Bold);
    item_ = new QLabel(name_);
    item_->setObjectName("imageFullscreenItem");
    QFont Small = item_->font();
    if (Small.pointSizeF() > 0)
        Small.setPointSizeF(Small.pointSizeF() * .9);
    item_->setFont(Small);
    Names->addWidget(book_);
    Names->addWidget(item_);
    Row->addWidget(name_);
    leading_ = Separator(top_);
    Row->addWidget(leading_);
    const auto Step = [this](int Direction)
    {
        if (navigate_)
            navigate_(Direction);
        else if (auto *R = reader())
            R->Move(Direction);
    };
    auto *Previous = new OverlayButton(top_, false);
    Previous->setObjectName("fullscreenPrevious");
    Previous->setIcon(ImageReadingNavigator::OverlayIcon("previous"));
    Previous->setToolTip(tr("이전"));
    connect(Previous, &QToolButton::clicked, this, [Step] { Step(-1); });
    previous_ = Previous;
    auto *Next = new OverlayButton(top_, false);
    Next->setObjectName("fullscreenNext");
    Next->setIcon(ImageReadingNavigator::OverlayIcon("next"));
    Next->setToolTip(tr("다음"));
    connect(Next, &QToolButton::clicked, this, [Step] { Step(1); });
    next_ = Next;
    Row->addWidget(previous_);
    Row->addWidget(next_);
    tools_ = new QHBoxLayout;
    tools_->setSpacing(6);
    Row->addLayout(tools_);
    trailing_ = Separator(top_);
    Row->addWidget(trailing_);
    display_ = new QMenu(tr("표시 설정"), Workspace);
    display_->setObjectName("fullscreenDisplayMenu");
    display_->addSection(tr("표시 설정"));
    auto *More = new OverlayButton(top_, false);
    More->setObjectName("fullscreenMore");
    More->setIcon(ImageReadingNavigator::OverlayIcon("more"));
    More->setToolTip(tr("표시 설정"));
    More->setAccessibleName(More->toolTip());
    More->setMenu(display_);
    More->setPopupMode(QToolButton::InstantPopup);
    more_ = More;
    Row->addWidget(more_);
    const auto FullscreenKey = QKeySequence(hc::defaults::edit::Shortcut("image.fullscreen"), QKeySequence::PortableText).toString(QKeySequence::NativeText);
    auto *Exit = new OverlayButton(top_, true);
    Exit->setObjectName("fullscreenExit");
    Exit->setIcon(ImageReadingNavigator::OverlayIcon("exitFullscreen"));
    Exit->setText(tr("전체 화면 해제"));
    Exit->setToolTip(tr("전체 화면 해제 (%1 · Esc)").arg(FullscreenKey));
    Exit->setAccessibleName(Exit->text());
    Exit->SetHint(FullscreenKey);
    connect(Exit, &QToolButton::clicked, this, &ImageFullscreen::Leave);
    exit_ = Exit;
    Row->addWidget(exit_);

    // The display toggles keep their object names and settings keys.
    const auto DisplayToggle = [this](const QString &Text, const char *Name, bool Checked)
    {
        auto *Action = new QAction(Text, top_);
        Action->setObjectName(Name);
        Action->setCheckable(true);
        Action->setChecked(Checked);
        display_->addAction(Action);
        return Action;
    };
    pinTopAction_ = DisplayToggle(tr("도구 모음 고정"), "fullscreenPinTop", false);
    connect(pinTopAction_, &QAction::toggled, this,
            [this](bool On)
            {
                pinTop_ = On;
                reveal(true, false);
                save();
            });
    pinBottomAction_ = DisplayToggle(tr("탐색 막대 고정"), "fullscreenPinBottom", false);
    connect(pinBottomAction_, &QAction::toggled, this,
            [this](bool On)
            {
                pinBottom_ = On;
                // The navigator owns its pin (imageView/navigatorPinned); this entry drives it while it is shown.
                if (auto *Navigator = navigated() ? navigator() : nullptr)
                    Navigator->SetPinned(On);
                reveal(false, true);
                save();
            });
    cursorAction_ = DisplayToggle(tr("커서 자동 숨김"), "fullscreenCursor", true);
    connect(cursorAction_, &QAction::toggled, this,
            [this](bool On)
            {
                hideCursor_ = On;
                if (!On)
                    workspace_->unsetCursor();
                save();
            });
    rememberAction_ = DisplayToggle(tr("전체 화면 기억"), "fullscreenRemember", true);
    connect(rememberAction_, &QAction::toggled, this,
            [this](bool On)
            {
                remember_ = On;
                save();
            });
    // Without a reading navigator (single images) the status line still shows the document status.
    bottom_ = new QLabel(Workspace);
    bottom_->setObjectName("imageFullscreenStatus");
    bottom_->setAutoFillBackground(true);
    bottom_->hide();
    Capsule->relayout_ = [this] { place(); };
    auto *ToggleKey = new QShortcut(QKeySequence(hc::defaults::edit::Shortcut("image.fullscreen")), Workspace);
    ToggleKey->setObjectName("imageFullscreenFallback");
    ToggleKey->setAutoRepeat(false);
    connect(ToggleKey, &QShortcut::activated, this, &ImageFullscreen::Toggle);
    idle_.setSingleShot(true);
    idle_.setInterval(2000);
    connect(&idle_, &QTimer::timeout, this, &ImageFullscreen::timeout);
    edge_.setSingleShot(true);
    edge_.setInterval(150);
    // Either screen edge brings back both overlays: the capsule and the navigator (or status line).
    connect(&edge_, &QTimer::timeout, this, [this] { reveal(true, true); });
    qApp->installEventFilter(this);
}
ImageFullscreen::~ImageFullscreen()
{
    save();
    workspace_->unsetCursor();
    qApp->removeEventFilter(this);
    // The overlays are workspace children that call back into this object.
    delete top_;
    delete bottom_;
    delete display_;
}
void ImageFullscreen::Toggle()
{
    if (active_)
        Leave();
    else
        Enter();
}
void ImageFullscreen::Restore()
{
    if (restored_ || !workspace_->isVisible())
        return;
    restored_ = true;
    tracked_ = true;
    AppSettings S;
    remember_ = S.value("imageView/fullscreenRemember").toBool();
    pinTop_ = S.value("imageView/fullscreenPinTop").toBool();
    pinBottom_ = S.value("imageView/fullscreenPinBottom").toBool();
    hideCursor_ = S.value("imageView/fullscreenHideCursor").toBool();
    for (const auto &[Action, Value] : {std::pair{pinTopAction_, pinTop_}, std::pair{pinBottomAction_, pinBottom_},
                                        std::pair{cursorAction_, hideCursor_}, std::pair{rememberAction_, remember_}})
    {
        const QSignalBlocker Block(Action);
        Action->setChecked(Value);
    }
    const auto Geometry = S.value("imageView/normalGeometry").toList();
    const auto SavedScreen = S.value("imageView/fullscreenScreen").toString();
    bool ScreenPresent = SavedScreen.isEmpty();
    for (auto *Screen : QGuiApplication::screens())
        ScreenPresent |= Screen->name() == SavedScreen;
    if (Geometry.size() == 4 && !workspace_->parentWidget())
    {
        // Window size belongs to the viewer/editor window settings, regardless of document type.
        QRect Rect(QPoint(Geometry[0].toInt(), Geometry[1].toInt()), workspace_->size());
        bool OnScreen = false;
        for (auto *Screen : QGuiApplication::screens())
            OnScreen |= Screen->availableGeometry().contains(Rect.center());
        const auto Area = workspace_->screen()->availableGeometry();
        Rect.setSize(Rect.size().boundedTo(Area.size()).expandedTo(QSize(320, 240)));
        if (!OnScreen || !ScreenPresent)
            Rect.moveTopLeft(Area.topLeft());
        workspace_->setGeometry(Rect);
        if (S.value("imageView/normalMaximized").toBool())
            workspace_->showMaximized();
    }
    if (remember_ && ScreenPresent && S.value("imageView/fullscreen").toBool())
        QTimer::singleShot(0, this,
                           [this]
                           {
                               if (workspace_->isVisible())
                                   Enter();
                           });
}
void ImageFullscreen::Enter()
{
    if (active_)
        return;
    active_ = true;
    tracked_ = true;
    parent_ = workspace_->parentWidget();
    flags_ = workspace_->windowFlags();
    normal_ = workspace_->isMaximized() ? workspace_->normalGeometry() : workspace_->geometry();
    maximized_ = workspace_->isMaximized();
    auto *Screen = workspace_->screen();
    if (parent_)
        workspace_->setParent(nullptr, Qt::Window);
    for (auto *Child : workspace_->findChildren<QWidget *>())
    {
        if (Child == top_ || Child == bottom_ || top_->isAncestorOf(Child))
            continue;
        if ((qobject_cast<QMenuBar *>(Child) || qobject_cast<QToolBar *>(Child) || qobject_cast<QTabBar *>(Child) ||
             Child->objectName() == "documentWindowChrome" || Child->objectName() == "documentStatusRow" ||
             Child->objectName() == "documentStatus") &&
            !Child->isHidden())
        {
            hidden_ << Child;
            Child->hide();
        }
    }
    emit ActiveChanged(true);
    workspace_->showFullScreen();
    workspace_->setScreen(Screen);
    workspace_->setMouseTracking(true);
    for (auto *Child : workspace_->findChildren<QWidget *>())
        Child->setMouseTracking(true);
    // Shared actions may have been rebuilt since the last session.
    shared_.clear();
    reveal(true, true);
    save();
}
void ImageFullscreen::Leave()
{
    if (!active_)
        return;
    active_ = false;
    idle_.stop();
    edge_.stop();
    unbind();
    workspace_->unsetCursor();
    top_->hide();
    bottom_->hide();
    workspace_->showNormal();
    if (parent_)
    {
        workspace_->setParent(parent_, flags_);
        if (parent_->layout())
            parent_->layout()->addWidget(workspace_);
    }
    QRect Rect = normal_;
    bool Found = false;
    for (auto *Screen : QGuiApplication::screens())
        Found |= Screen->availableGeometry().intersects(Rect);
    if (!Found)
        Rect = QRect(workspace_->screen()->availableGeometry().topLeft(), hc::defaults::edit::WindowSize)
                   .intersected(workspace_->screen()->availableGeometry());
    workspace_->setGeometry(Rect);
    for (const auto &Child : hidden_)
        if (Child)
            Child->show();
    hidden_.clear();
    emit ActiveChanged(false);
    if (maximized_ && !parent_)
        workspace_->showMaximized();
    else
        workspace_->show();
    workspace_->setFocus();
    save();
}
void ImageFullscreen::save()
{
    if (!tracked_ || QCoreApplication::instance()->property("privateReading").toBool())
        return;
    if (!active_ && !workspace_->parentWidget()) {
        maximized_ = workspace_->isMaximized();
        normal_ = maximized_ ? workspace_->normalGeometry() : workspace_->geometry();
    }
    AppSettings S;
    S.setValue("imageView/fullscreen", active_);
    S.setValue("imageView/fullscreenRemember", remember_);
    S.setValue("imageView/fullscreenPinTop", pinTop_);
    S.setValue("imageView/fullscreenPinBottom", pinBottom_);
    S.setValue("imageView/fullscreenHideCursor", hideCursor_);
    if (!normal_.isEmpty())
        S.setValue("imageView/normalGeometry",
                   QVariantList{normal_.x(), normal_.y(), normal_.width(), normal_.height()});
    S.setValue("imageView/normalMaximized", maximized_);
    S.setValue("imageView/fullscreenScreen", workspace_->screen()->name());
}
ImageReadingView *ImageFullscreen::reader() const
{
    return reader_ ? reader_() : nullptr;
}
ImageReadingNavigator *ImageFullscreen::navigator() const
{
    auto *Reader = reader();
    return Reader ? Reader->findChild<ImageReadingNavigator *>() : nullptr;
}
bool ImageFullscreen::navigated() const
{
    // The navigator serves comic and webtoon books; single images and single-page layouts have none.
    auto *Reader = reader();
    if (!Reader || Reader->isHidden())
        return false;
    const auto S = Reader->State();
    return S["pageCount"].toInt() > 0 && S["mode"].toInt() != ImageReadingView::Single;
}
void ImageFullscreen::bind()
{
    auto *Reader = reader();
    if (Reader == bound_.data())
        return;
    unbind();
    bound_ = Reader;
    if (!Reader)
        return;
    bindings_ << connect(Reader, &ImageReadingView::Changed, this, &ImageFullscreen::schedule);
    if (auto *Navigator = Reader->findChild<ImageReadingNavigator *>())
        bindings_ << connect(Navigator, &ImageReadingNavigator::PinnedChanged, this,
                             [this](bool On)
                             {
                                 // The navigator's own pin button and 탐색 막대 고정 are one setting.
                                 const QSignalBlocker Block(pinBottomAction_);
                                 pinBottomAction_->setChecked(On);
                                 if (pinBottom_ == On)
                                     return;
                                 pinBottom_ = On;
                                 save();
                             });
}
void ImageFullscreen::unbind()
{
    for (const auto &Connection : std::exchange(bindings_, {}))
        disconnect(Connection);
    bound_ = nullptr;
    shared_.clear();
}
void ImageFullscreen::UpdateStatus()
{
    schedule();
}
void ImageFullscreen::schedule()
{
    // Reader changes arrive in bursts (scrolling, frames); one refresh per event loop turn is enough.
    if (!active_ || queued_)
        return;
    queued_ = true;
    QTimer::singleShot(0, this,
                       [this]
                       {
                           queued_ = false;
                           refresh();
                       });
}
void ImageFullscreen::refresh()
{
    if (!active_)
        return;
    bind();
    auto *Reader = bound_.data();
    const auto S = Reader ? Reader->State() : QJsonObject{};
    const bool Book = Reader && !Reader->isHidden() && S["pageCount"].toInt() > 0;
    const int Mode = Book ? S["mode"].toInt() : -1;
    const bool Navigated = Book && Mode != ImageReadingView::Single;
    name_->setVisible(Book);
    if (Book)
    {
        const bool Presented = Mode != ImageReadingView::Webtoon && S["presentedPage"].toInt(-1) >= 0;
        const auto Path = S["book"].toString();
        const auto Item = S[Presented ? "presentedName" : "pageName"].toString();
        const auto Title = QFileInfo(Path).fileName();
        book_->setText(book_->fontMetrics().elidedText(Title.isEmpty() ? Path : Title, Qt::ElideMiddle, NameWidth));
        item_->setText(item_->fontMetrics().elidedText(Item, Qt::ElideMiddle, NameWidth));
        // Names only in the capsule; the full path stays reachable as the tooltip.
        const auto Full = QDir::toNativeSeparators(Item.isEmpty() ? Path : Path + '/' + Item);
        for (auto *Widget : {name_, static_cast<QWidget *>(book_), static_cast<QWidget *>(item_)})
            Widget->setToolTip(Full);
    }
    syncTools(Mode);
    previous_->setVisible(!Navigated);
    next_->setVisible(!Navigated);
    // Separators only between groups that are present: name | tools or previous/next | ⋯ · exit.
    const bool Tools = !toolButtons_.isEmpty() || !Navigated;
    leading_->setVisible(Book && Tools);
    trailing_->setVisible(Book || Tools);
    if (Navigated)
    {
        // Page and position are on the navigator; the status line would only repeat them.
        bottom_->hide();
        if (auto *Navigator = navigator(); Navigator && Navigator->Pinned() != pinBottomAction_->isChecked())
        {
            const QSignalBlocker Block(pinBottomAction_);
            pinBottomAction_->setChecked(Navigator->Pinned());
            pinBottom_ = Navigator->Pinned();
            save();
        }
    }
    else
    {
        // Status text from the document status bar items; the reader's position covers a single-page book.
        QStringList Parts;
        // The window's own bar lives in its status row (hidden while full screen; its fields stay current).
        auto *Row = workspace_->findChild<QWidget *>("documentStatusRow");
        if (auto *Bar = Row ? dynamic_cast<DocumentStatusBar *>(Row->findChild<QWidget *>("documentStatusBar")) : nullptr)
            for (const auto &Field : Bar->Fields())
                if (!Field.text_.isEmpty())
                    Parts << Field.text_;
        if (Parts.isEmpty() && Book)
            Parts << QString("%1 / %2").arg(S["page"].toInt() + 1).arg(S["pageCount"].toInt()) << S["pageName"].toString();
        if (Parts.isEmpty())
            if (auto *Status = workspace_->findChild<QLabel *>("documentStatus"))
                Parts << Status->text();
        bottom_->setText(Parts.join(QStringLiteral(" · ")));
        if (bottom_->text().isEmpty())
            bottom_->hide();
    }
    place();
}
void ImageFullscreen::syncTools(int Mode)
{
    // Reading tools are the document pane's shared actions, so text, checks, menus and visibility
    // follow the window toolbar. Without a pane (or an action) the tool is left out.
    static const QStringList ComicTools{"readingPageList", "readingSpreadButton", "readingDirectionButton",
                                        "readingCorrectButton", "readingBookmarkButton"};
    static const QStringList WebtoonTools{"readingPageList", "readingGapButton", "readingCorrectButton",
                                          "readingBookmarkButton"};
    QList<QAction *> Wanted;
    if (auto *Pane = Mode >= 0 && bound_ ? bound_->parentWidget() : nullptr)
        for (const auto &Name : Mode == ImageReadingView::Webtoon ? WebtoonTools : ComicTools)
        {
            auto &Found = shared_[Name];
            if (!Found)
                Found = Pane->findChild<QAction *>(Name);
            if (Found && Found->isVisible())
                Wanted << Found.data();
        }
    QList<QAction *> Current;
    for (const auto &Action : toolActions_)
        Current << Action.data();
    if (Wanted == Current)
        return;
    // Rebuilt from a deferred refresh, so a button is never deleted inside its own click.
    for (const auto &Old : std::exchange(toolButtons_, {}))
        if (Old)
        {
            Old->hide();
            Old->deleteLater();
        }
    toolActions_.clear();
    for (auto *Action : Wanted)
    {
        const auto Name = Action->objectName();
        auto *Tool = new OverlayButton(top_, Name != "readingPageList" && Name != "readingBookmarkButton");
        Tool->Bind(Action);
        tools_->addWidget(Tool);
        // Visibility is not part of the button state; a hidden action leaves the capsule on the next refresh.
        connect(Action, &QAction::changed, Tool, [this] { schedule(); });
        toolButtons_ << Tool;
        toolActions_ << Action;
    }
}
void ImageFullscreen::place()
{
    const auto Hint = top_->sizeHint();
    const int Width = qMin(Hint.width(), qMax(0, workspace_->width() - 16));
    // Centred; the glass sits GlassTop px below the screen edge inside the shadow margin.
    top_->setGeometry((workspace_->width() - Width) / 2, GlassTop - Margin, Width, Hint.height());
    bottom_->setGeometry(0, workspace_->height() - bottom_->sizeHint().height() - 8, workspace_->width(),
                         bottom_->sizeHint().height() + 8);
}
void ImageFullscreen::reveal(bool Top, bool Bottom)
{
    if (!active_)
        return;
    workspace_->unsetCursor();
    refresh();
    if (Top || pinTop_)
    {
        top_->show();
        top_->raise();
    }
    if (Bottom || pinBottom_)
    {
        if (navigated())
        {
            if (auto *Navigator = navigator())
                Navigator->Reveal();
        }
        else if (!bottom_->text().isEmpty())
        {
            bottom_->show();
            bottom_->raise();
        }
    }
    idle_.start();
}
void ImageFullscreen::timeout()
{
    if (!active_ || !workspace_->isActiveWindow())
        return;
    auto *Focus = QApplication::focusWidget();
    auto *Hover = QApplication::widgetAt(QCursor::pos());
    auto *Navigator = navigator();
    if (Navigator && Navigator->isVisible() &&
        ((Focus && (Focus == Navigator || Navigator->isAncestorOf(Focus))) ||
         (Hover && (Hover == Navigator || Navigator->isAncestorOf(Hover)))))
    {
        workspace_->unsetCursor();
        idle_.start();
        return;
    }
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget() ||
        QApplication::mouseButtons() != Qt::NoButton || (Focus && (Focus == top_ || top_->isAncestorOf(Focus))) ||
        (Hover && (Hover == top_ || top_->isAncestorOf(Hover) || Hover == bottom_)))
    {
        idle_.start();
        return;
    }
    if (!pinTop_)
        top_->hide();
    if (!pinBottom_)
        bottom_->hide();
    // The capsule and the navigator leave together; the navigator keeps itself while pinned or in use.
    if (Navigator && Navigator->isVisible())
        Navigator->HideIdle();
    if (hideCursor_)
        workspace_->setCursor(Qt::BlankCursor);
}
QToolButton *ImageFullscreen::firstButton() const
{
    QList<QToolButton *> Order{previous_, next_};
    for (const auto &Tool : toolButtons_)
        if (Tool)
            Order << Tool.data();
    Order << more_ << exit_;
    for (auto *Candidate : Order)
        if (!Candidate->isHidden())
            return Candidate;
    return exit_;
}
bool ImageFullscreen::eventFilter(QObject *Object, QEvent *Event)
{
    auto *Widget = qobject_cast<QWidget *>(Object);
    if (!active_ || !Widget || (Widget != workspace_ && !workspace_->isAncestorOf(Widget)))
        return false;
    if (Event->type() == QEvent::ShortcutOverride)
    {
        auto *Key = static_cast<QKeyEvent *>(Event);
        if (Key->key() == Qt::Key_Escape && !QApplication::activePopupWidget() && !QApplication::activeModalWidget())
        {
            Event->accept();
            return true;
        }
    }
    if (Event->type() == QEvent::KeyPress)
    {
        auto *Key = static_cast<QKeyEvent *>(Event);
        auto *Reader = reader();
        auto *Navigator = Reader ? Reader->findChild<ImageReadingNavigator *>() : nullptr;
        // Escape first returns keyboard focus from the navigator to the page.
        if (Key->key() == Qt::Key_Escape && Navigator && Navigator->isAncestorOf(Widget))
            return false;
        if (Key->key() == Qt::Key_Escape && !Key->isAutoRepeat() && !QApplication::activePopupWidget() &&
            !QApplication::activeModalWidget())
        {
            Leave();
            return true;
        }
        if (Key->key() == hc::defaults::edit::AuxiliaryShortcut("image.focusNavigator").key() || Key->key() == Qt::Key_Tab)
        {
            if (Key->key() == hc::defaults::edit::AuxiliaryShortcut("image.focusNavigator").key())
                if (Reader && Reader->isVisible() && Navigator && Reader->State()["mode"].toInt() != ImageReadingView::Single)
                {
                    Navigator->Reveal();
                    if (auto *Progress = Navigator->findChild<QWidget *>("imageNavigatorProgress"))
                        Progress->setFocus(Qt::ShortcutFocusReason);
                    return true;
                }
            reveal(true, true);
            if (Key->key() == hc::defaults::edit::AuxiliaryShortcut("image.focusNavigator").key())
            {
                firstButton()->setFocus();
                return true;
            }
        }
    }
    if (Event->type() == QEvent::MouseMove)
    {
        auto *Mouse = static_cast<QMouseEvent *>(Event);
        workspace_->unsetCursor();
        const auto P = workspace_->mapFromGlobal(Mouse->globalPosition().toPoint());
        const int Side = P.y() < 12 ? 1 : P.y() > workspace_->height() - 12 ? 2 : 0;
        if (Side != edgeSide_)
        {
            edge_.stop();
            edgeSide_ = Side;
            if (Side)
                edge_.start();
            else
            {
                idle_.setInterval(500);
                idle_.start();
            }
        }
        else if (!Side)
        {
            idle_.setInterval(2000);
            idle_.start();
        }
    }
    if (Event->type() == QEvent::MouseButtonPress || Event->type() == QEvent::Wheel)
    {
        workspace_->unsetCursor();
        idle_.start(2000);
    }
    if (Event->type() == QEvent::WindowDeactivate || Event->type() == QEvent::Hide)
    {
        workspace_->unsetCursor();
        idle_.stop();
        edge_.stop();
    }
    if (Event->type() == QEvent::WindowActivate)
        reveal(true, true);
    if (Event->type() == QEvent::Resize && Object == workspace_)
        place();
    return false;
}
QJsonObject ImageFullscreen::State() const
{
    QJsonArray Tools;
    for (const auto &Action : toolActions_)
        if (Action)
            Tools.append(Action->objectName());
    auto *Navigator = navigator();
    return {{"active", active_},
            {"toolbar", top_->isVisible()},
            {"status", bottom_->isVisible()},
            {"navigator", Navigator && Navigator->isVisible()},
            {"tools", Tools},
            {"cursorHidden", workspace_->cursor().shape() == Qt::BlankCursor}};
}
} // namespace hc::document
