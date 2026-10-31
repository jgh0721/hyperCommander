#include "stdafx.h"
#include "imageFullscreen.hpp"
#include "imageReadingView.hpp"
#include "core/AppSettings.h"
#include <QApplication>
#include <QAction>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenuBar>
#include <QMouseEvent>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTabBar>
#include <QToolBar>

namespace hc::document
{
ImageFullscreen::ImageFullscreen(QWidget *Workspace, std::function<ImageReadingView *()> Reader, std::function<void(int)> Navigate)
    : QObject(Workspace), workspace_(Workspace), reader_(std::move(Reader)), navigate_(std::move(Navigate))
{
    top_ = new QToolBar(Workspace);
    top_->setObjectName("imageFullscreenControls");
    top_->hide();
    auto *Exit = top_->addAction(tr("전체 화면 해제"));
    connect(Exit, &QAction::triggered, this, &ImageFullscreen::Leave);
    auto *Previous = top_->addAction(tr("이전"));
    connect(Previous, &QAction::triggered, this,
            [this]
            {
                if (navigate_) navigate_(-1);
                else if (auto *R = reader_ ? reader_() : nullptr)
                    R->Move(-1);
            });
    auto *Next = top_->addAction(tr("다음"));
    connect(Next, &QAction::triggered, this,
            [this]
            {
                if (navigate_) navigate_(1);
                else if (auto *R = reader_ ? reader_() : nullptr)
                    R->Move(1);
            });
    auto *Pin = top_->addAction(tr("도구 모음 고정"));
    Pin->setObjectName("fullscreenPinTop");
    Pin->setCheckable(true);
    connect(Pin, &QAction::toggled, this,
            [this](bool On)
            {
                pinTop_ = On;
                reveal(true, false);
                save();
            });
    auto *PinStatus = top_->addAction(tr("상태줄 고정"));
    PinStatus->setObjectName("fullscreenPinBottom");
    PinStatus->setCheckable(true);
    connect(PinStatus, &QAction::toggled, this,
            [this](bool On)
            {
                pinBottom_ = On;
                reveal(false, true);
                save();
            });
    auto *Cursor = top_->addAction(tr("커서 자동 숨김"));
    Cursor->setObjectName("fullscreenCursor");
    Cursor->setCheckable(true);
    Cursor->setChecked(true);
    connect(Cursor, &QAction::toggled, this,
            [this](bool On)
            {
                hideCursor_ = On;
                if (!On)
                    workspace_->unsetCursor();
                save();
            });
    auto *Remember = top_->addAction(tr("전체 화면 기억"));
    Remember->setObjectName("fullscreenRemember");
    Remember->setCheckable(true);
    Remember->setChecked(true);
    connect(Remember, &QAction::toggled, this,
            [this](bool On)
            {
                remember_ = On;
                save();
            });
    bottom_ = new QLabel(Workspace);
    bottom_->setObjectName("imageFullscreenStatus");
    bottom_->setAutoFillBackground(true);
    bottom_->hide();
    auto *ToggleKey = new QShortcut(QKeySequence(Qt::Key_F11), Workspace);
    ToggleKey->setObjectName("imageFullscreenFallback");
    ToggleKey->setAutoRepeat(false);
    connect(ToggleKey, &QShortcut::activated, this, &ImageFullscreen::Toggle);
    idle_.setSingleShot(true);
    idle_.setInterval(2000);
    connect(&idle_, &QTimer::timeout, this, &ImageFullscreen::timeout);
    edge_.setSingleShot(true);
    edge_.setInterval(150);
    connect(&edge_, &QTimer::timeout, this, [this] { reveal(edgeSide_ == 1, edgeSide_ == 2); });
    qApp->installEventFilter(this);
}
ImageFullscreen::~ImageFullscreen()
{
    save();
    workspace_->unsetCursor();
    qApp->removeEventFilter(this);
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
    remember_ = S.value("imageView/fullscreenRemember", true).toBool();
    pinTop_ = S.value("imageView/fullscreenPinTop", false).toBool();
    pinBottom_ = S.value("imageView/fullscreenPinBottom", false).toBool();
    hideCursor_ = S.value("imageView/fullscreenHideCursor", true).toBool();
    for (auto *Action : top_->actions())
    {
        const QSignalBlocker Block(Action);
        if (Action->objectName() == "fullscreenPinTop")
            Action->setChecked(pinTop_);
        else if (Action->objectName() == "fullscreenPinBottom")
            Action->setChecked(pinBottom_);
        else if (Action->objectName() == "fullscreenCursor")
            Action->setChecked(hideCursor_);
        else if (Action->objectName() == "fullscreenRemember")
            Action->setChecked(remember_);
    }
    const auto Geometry = S.value("imageView/normalGeometry").toList();
    const auto SavedScreen = S.value("imageView/fullscreenScreen").toString();
    bool ScreenPresent = SavedScreen.isEmpty();
    for (auto *Screen : QGuiApplication::screens())
        ScreenPresent |= Screen->name() == SavedScreen;
    if (Geometry.size() == 4 && !workspace_->parentWidget())
    {
        QRect Rect(Geometry[0].toInt(), Geometry[1].toInt(), Geometry[2].toInt(), Geometry[3].toInt());
        bool OnScreen = false;
        for (auto *Screen : QGuiApplication::screens())
            OnScreen |= Screen->availableGeometry().contains(Rect.center());
        if (!OnScreen || !ScreenPresent)
            Rect = QRect(workspace_->screen()->availableGeometry().topLeft(), QSize(1000, 700));
        const auto Area = workspace_->screen()->availableGeometry();
        Rect.setSize(Rect.size().boundedTo(Area.size()).expandedTo(QSize(320, 240)));
        if (!OnScreen)
            Rect.moveTopLeft(Area.topLeft());
        workspace_->setGeometry(Rect);
        if (S.value("imageView/normalMaximized", false).toBool())
            workspace_->showMaximized();
    }
    if (remember_ && ScreenPresent && S.value("imageView/fullscreen", false).toBool())
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
        Rect = QRect(workspace_->screen()->availableGeometry().topLeft(), QSize(1000, 700))
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
void ImageFullscreen::reveal(bool Top, bool Bottom)
{
    if (!active_)
        return;
    workspace_->unsetCursor();
    top_->setGeometry(0, 0, workspace_->width(), top_->sizeHint().height());
    bottom_->setGeometry(0, workspace_->height() - bottom_->sizeHint().height() - 8, workspace_->width(),
                         bottom_->sizeHint().height() + 8);
    UpdateStatus();
    if (Top || pinTop_)
    {
        top_->show();
        top_->raise();
    }
    if (Bottom || pinBottom_)
    {
        bottom_->show();
        bottom_->raise();
    }
    idle_.start();
}
void ImageFullscreen::UpdateStatus()
{
    if (!active_) return;
    if (auto *Reader = reader_ ? reader_() : nullptr; Reader && Reader->State()["pageCount"].toInt()>0)
    {
        const auto S = Reader->State();
        const bool Presented = S["mode"].toInt() != ImageReadingView::Webtoon && S["presentedPage"].toInt(-1) >= 0;
        bottom_->setText(QString("%1 · %2 / %3 · %4")
                             .arg(S["book"].toString())
                             .arg(S[Presented ? "presentedPage" : "page"].toInt() + 1)
                             .arg(S["pageCount"].toInt())
                             .arg(S[Presented ? "presentedName" : "pageName"].toString()));
    }
    else if (auto* Status=workspace_->findChild<QLabel*>("documentStatus")) bottom_->setText(Status->text());
}
void ImageFullscreen::timeout()
{
    if (!active_ || !workspace_->isActiveWindow())
        return;
    auto *Focus = QApplication::focusWidget();
    auto *Hover = QApplication::widgetAt(QCursor::pos());
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
    if (hideCursor_)
        workspace_->setCursor(Qt::BlankCursor);
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
        if (Key->key() == Qt::Key_Escape && !Key->isAutoRepeat() && !QApplication::activePopupWidget() &&
            !QApplication::activeModalWidget())
        {
            Leave();
            return true;
        }
        if (Key->key() == Qt::Key_F6 || Key->key() == Qt::Key_Tab)
        {
            reveal(true, true);
            if (Key->key() == Qt::Key_F6)
            {
                if (auto *First = top_->widgetForAction(top_->actions().first()))
                    First->setFocus();
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
    if (Event->type() == QEvent::Resize)
    {
        top_->setGeometry(0, 0, workspace_->width(), top_->sizeHint().height());
        bottom_->move(0, workspace_->height() - bottom_->height());
    }
    return false;
}
QJsonObject ImageFullscreen::State() const
{
    return {{"active", active_},
            {"toolbar", top_->isVisible()},
            {"status", bottom_->isVisible()},
            {"cursorHidden", workspace_->cursor().shape() == Qt::BlankCursor}};
}
} // namespace hc::document
