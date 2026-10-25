#include "stdafx.h"
#include "panelPathBar.hpp"

/// Unique libraries
#include "UniqueLibs/storageProviders.hpp"

/// External libraries
#include <fmstyle/Glyphs.h>
#include <fmstyle/StylePaint.h>
#include <fmstyle/ThemeManager.h>

/// Platform/framework headers
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace hc::ui {
namespace {
struct FolderResult { QStringList names_; QString error_; bool truncated_ = false; };

FolderResult ReadFolders(const QString& Path, std::stop_token Stop)
{
    using namespace hc::storage;
    FolderResult Result;
    const bool Remote = Path.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive)
        || (Path.startsWith("\\\\") && !Path.startsWith("\\\\?\\"));
    const auto Kind = Remote ? LocatorKind::Smb : LocatorKind::Local;
    auto Provider = MakeNativeReadProvider(Kind);
    OperationContext Context;
    Context.stopToken_ = Stop;
    Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    Context.incrementalEnumeration_ = true;
    Context.allowUnavailableChildMetadata_ = true;
    auto Cursor = Provider->Enumerate({Kind, Path.toStdString()}, Context).get();
    if (!Cursor.HasValue()) { Result.error_ = QString::fromStdString(Cursor.Failure().code_); return Result; }
    std::size_t Inspected = 0;
    while (!Stop.stop_requested()) {
        auto Page = Cursor.Value()->Next(128, Context).get();
        if (!Page.HasValue()) { Result.error_ = QString::fromStdString(Page.Failure().code_); break; }
        for (const auto& Entry : Page.Value().entries_)
            if (Entry.directory_ && Entry.name_ != "." && Entry.name_ != "..") Result.names_.append(QString::fromStdString(Entry.name_));
        Inspected += Page.Value().entries_.size();
        if (Page.Value().end_) break;
        if (Inspected >= 50000 || Result.names_.size() >= 2000) { Result.truncated_ = true; break; }
    }
    std::sort(Result.names_.begin(), Result.names_.end(), [](const QString& A, const QString& B) {
        const auto Order = A.compare(B, Qt::CaseInsensitive); return Order == 0 ? A < B : Order < 0;
    });
    return Result;
}
}

PanelPathBar::PanelPathBar(QWidget* Parent) : BreadcrumbBar(Parent)
{
    SetFullPathMode(true);
    setFocusPolicy(Qt::StrongFocus);
    clickTimer_.setSingleShot(true);
    connect(&clickTimer_, &QTimer::timeout, this, [this] { navigate(pendingPath_); });
    connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, [this] { layoutCrumbs(); update(); });
}

PanelPathBar::~PanelPathBar() { menuStop_.request_stop(); }

void PanelPathBar::setFullPath(const QString& Path)
{
    BreadcrumbBar::setFullPath(Path);
    if (fullPath_ == Path) return;
    clickTimer_.stop(); dismissMenu();
    fullPath_ = Path; crumbs_.clear();
    auto Normal = QDir::fromNativeSeparators(Path);
    int Start = 0;
    const int DriveStart = Normal.startsWith("//?/") ? 4 : 0;
    const bool ExtendedUnc = Normal.startsWith("//?/UNC/", Qt::CaseInsensitive);
    // Keep a UNC share together: its server name must never become a local path.
    if (ExtendedUnc || (Normal.startsWith("//") && !DriveStart)) {
        const int ServerEnd = Normal.indexOf('/', ExtendedUnc ? 8 : 2);
        const int ShareEnd = ServerEnd < 0 ? -1 : Normal.indexOf('/', ServerEnd + 1);
        Start = ShareEnd < 0 ? int(Normal.size()) : ShareEnd;
    } else if (Normal.size() >= DriveStart + 3 && Normal[DriveStart].isLetter()
        && Normal[DriveStart + 1] == ':' && Normal[DriveStart + 2] == '/') Start = DriveStart + 3;
    else if (Normal.startsWith('/')) Start = 1;
    if (Start > 0) {
        const auto Root = QDir::toNativeSeparators(Normal.left(Start));
        crumbs_.append({Root, Root, {}, {}});
        for (int End = Start; End < Normal.size();) {
            while (End < Normal.size() && Normal[End] == '/') ++End;
            const int Begin = End;
            while (End < Normal.size() && Normal[End] != '/') ++End;
            if (End > Begin) crumbs_.append({Normal.mid(Begin, End - Begin), QDir::toNativeSeparators(Normal.left(End)), {}, {}});
        }
    }
    revealed_ = false; hover_ = pressed_ = -1; keyboard_ = qMax(0, (int(crumbs_.size()) - 1) * 2);
    layoutCrumbs(); update();
}

void PanelPathBar::SetBreadcrumbsEnabled(bool Enabled)
{
    if (enabled_ == Enabled) return;
    enabled_ = Enabled; clickTimer_.stop(); dismissMenu(); revealed_ = false; update();
}

void PanelPathBar::layoutCrumbs()
{
    const QFontMetrics Metrics(fm::style::pixelFont(font(), 13));
    const int Available = qMax(0, width() - 8), Arrow = 18, Overflow = 26;
    int Total = 0;
    for (auto& Crumb : crumbs_) {
        Crumb.text_ = {}; Crumb.arrow_ = {};
        Total += Metrics.horizontalAdvance(Crumb.label_) + 12 + Arrow;
    }
    firstVisible_ = 0;
    while (firstVisible_ < crumbs_.size() - 1 && Total + (firstVisible_ ? Overflow : 0) > Available) {
        Total -= Metrics.horizontalAdvance(crumbs_[firstVisible_].label_) + 12 + Arrow; ++firstVisible_;
    }
    overflow_ = firstVisible_ ? QRect(4, 2, qMin(Overflow, Available), qMax(0, height() - 4)) : QRect{};
    int X = 4 + overflow_.width();
    for (int Index = firstVisible_; Index < crumbs_.size(); ++Index) {
        auto& Crumb = crumbs_[Index];
        const int Remaining = qMax(0, width() - 4 - X);
        const int TextWidth = qMin(Metrics.horizontalAdvance(Crumb.label_) + 12, qMax(0, Remaining - Arrow));
        Crumb.text_ = QRect(X, 2, TextWidth, qMax(0, height() - 4)); X += TextWidth;
        Crumb.arrow_ = QRect(X, 2, qMin(Arrow, qMax(0, width() - 4 - X)), qMax(0, height() - 4)); X += Crumb.arrow_.width();
    }
    if (keyboard_ >= 0 && keyboard_ < firstVisible_ * 2) keyboard_ = -2;
}

int PanelPathBar::hitAt(const QPoint& Position) const
{
    if (overflow_.contains(Position)) return -2;
    for (int Index = firstVisible_; Index < crumbs_.size(); ++Index) {
        if (crumbs_[Index].text_.contains(Position)) return Index * 2;
        if (crumbs_[Index].arrow_.contains(Position)) return Index * 2 + 1;
    }
    return -1;
}

bool PanelPathBar::overPath(const QPoint& Position) const
{
    const auto TextRect = rect().adjusted(4, 2, -4, -2);
    const QFontMetrics Metrics(fm::style::pixelFont(font(), 13));
    return TextRect.contains(Position) && Position.x() < 4 + qMin(TextRect.width(), Metrics.horizontalAdvance(fullPath_));
}

void PanelPathBar::updateHover(const QPoint& Position)
{
    if (enabled_ && !crumbs_.isEmpty() && overPath(Position)) revealed_ = true;
    hover_ = revealed_ ? hitAt(Position) : -1;
    setCursor(hover_ != -1 ? Qt::PointingHandCursor : Qt::IBeamCursor);
    setToolTip(hover_ >= 0 ? crumbs_[hover_ / 2].path_ : fullPath_);
    update();
}

void PanelPathBar::paintEvent(QPaintEvent* Event)
{
    BreadcrumbBar::paintEvent(Event);
    if (!revealed_ || crumbs_.isEmpty()) return;
    const auto& Colors = fm::style::themeColorsFor(this);
    using fm::style::Token;
    QPainter Painter(this);
    Painter.setClipRect(rect().adjusted(4, 2, -4, -2));
    Painter.fillRect(rect().adjusted(4, 2, -4, -2), IsPaneActive() ? Colors[Token::Accent] : Colors[Token::Field]);
    Painter.setFont(fm::style::pixelFont(font(), 13));
    const auto Draw = [&](QRect Area, int Hit, const QString& Text, bool Arrow) {
        const bool Hot = Hit == hover_ || (hasFocus() && Hit == keyboard_);
        if (Hot) {
            auto Fill = IsPaneActive() ? Colors[Token::OnAccent] : Colors[Token::Accent]; Fill.setAlpha(55);
            Painter.fillRect(Area, Fill);
            Painter.setPen(IsPaneActive() ? Colors[Token::OnAccent] : Colors[Token::Accent]);
            Painter.drawRect(Area.adjusted(0, 0, -1, -1));
        }
        const auto Foreground = IsPaneActive() ? Colors[Token::OnAccent] : Colors[Token::Fg];
        Painter.setPen(Foreground);
        if (Arrow) fm::style::paintGlyph(&Painter, fm::style::Glyph::ChevronRight, QRectF(Area.center().x() - 5, Area.center().y() - 5, 10, 10), Foreground);
        else Painter.drawText(Area.adjusted(6, 0, -6, 0), Qt::AlignVCenter, Painter.fontMetrics().elidedText(Text, Qt::ElideMiddle, qMax(0, Area.width() - 12)));
    };
    if (!overflow_.isEmpty()) Draw(overflow_, -2, QStringLiteral("…"), false);
    for (int Index = firstVisible_; Index < crumbs_.size(); ++Index) {
        Draw(crumbs_[Index].text_, Index * 2, crumbs_[Index].label_, false);
        Draw(crumbs_[Index].arrow_, Index * 2 + 1, {}, true);
    }
}

void PanelPathBar::enterEvent(QEnterEvent* Event)
{
    BreadcrumbBar::enterEvent(Event); updateHover(Event->position().toPoint());
}

void PanelPathBar::mouseMoveEvent(QMouseEvent* Event) { updateHover(Event->position().toPoint()); }

void PanelPathBar::mousePressEvent(QMouseEvent* Event)
{
    if (Event->button() != Qt::LeftButton) { BreadcrumbBar::mousePressEvent(Event); return; }
    emit activated();
    updateHover(Event->position().toPoint());
    pressed_ = revealed_ ? hitAt(Event->position().toPoint()) : -1;
    if (pressed_ == -1) { EditPath(); return; }
    setFocus(Qt::MouseFocusReason); keyboard_ = pressed_; update();
}

void PanelPathBar::mouseReleaseEvent(QMouseEvent* Event)
{
    if (Event->button() != Qt::LeftButton) return;
    const int Hit = revealed_ ? hitAt(Event->position().toPoint()) : -1;
    if (Hit != -1 && Hit == pressed_) {
        clickTimer_.stop();
        if (Hit == -2 || Hit % 2) openMenu(Hit);
        else {
            // Defer navigation until a second click can turn this into editing.
            pendingPath_ = crumbs_[Hit / 2].path_; clickTimer_.start(QApplication::doubleClickInterval());
        }
    }
    pressed_ = -1;
}

void PanelPathBar::mouseDoubleClickEvent(QMouseEvent* Event)
{
    if (Event->button() == Qt::LeftButton) EditPath();
}

void PanelPathBar::EditPath()
{
    emit activated();
    clickTimer_.stop(); dismissMenu(); revealed_ = false; pressed_ = -1;
    BreadcrumbBar::EditPath();
    if (auto* Editor = findChild<QLineEdit*>()) {
        Editor->setObjectName("hc.panelPath.editor"); Editor->setAccessibleName(tr("전체 경로 입력"));
        Editor->installEventFilter(this); Editor->raise();
    }
    update();
}

bool PanelPathBar::eventFilter(QObject* Object, QEvent* Event)
{
    if (auto* Editor = qobject_cast<QLineEdit*>(Object)) {
        if (Event->type() == QEvent::InputMethod) composing_ = !static_cast<QInputMethodEvent*>(Event)->preeditString().isEmpty();
        if (Event->type() == QEvent::Hide) {
            composing_ = false; revealed_ = false; update();
            if (Object == QApplication::focusWidget()) setFocus(Qt::OtherFocusReason);
        }
        if (!composing_ && (Event->type() == QEvent::ShortcutOverride || Event->type() == QEvent::KeyPress)) {
            auto* Key = static_cast<QKeyEvent*>(Event);
            const bool Plain = Key->modifiers() == Qt::NoModifier || Key->modifiers() == Qt::KeypadModifier;
            const bool Commit = Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter;
            if (Plain && (Commit || Key->key() == Qt::Key_Escape)) {
                Key->accept();
                if (Event->type() == QEvent::ShortcutOverride) return true;
                // Consume the editor's Enter: propagating it after hiding the
                // editor would reach the bar and start another editing session.
                const auto Path = Editor->text(); Editor->hide();
                if (Commit) navigate(Path);
                return true;
            }
        }
    }
    return BreadcrumbBar::eventFilter(Object, Event);
}

void PanelPathBar::leaveEvent(QEvent* Event)
{
    BreadcrumbBar::leaveEvent(Event);
    hover_ = pressed_ = -1;
    if (!menu_) revealed_ = false;
    unsetCursor(); update();
}

void PanelPathBar::resizeEvent(QResizeEvent* Event)
{
    BreadcrumbBar::resizeEvent(Event); layoutCrumbs(); update();
}

void PanelPathBar::focusOutEvent(QFocusEvent* Event)
{
    clickTimer_.stop();
    BreadcrumbBar::focusOutEvent(Event);
    if (!underMouse() && !menu_) revealed_ = false;
    update();
}

void PanelPathBar::keyPressEvent(QKeyEvent* Event)
{
    clickTimer_.stop();
    if (Event->key() == Qt::Key_F2 || (Event->key() == Qt::Key_L && Event->modifiers() == Qt::ControlModifier)) { EditPath(); return; }
    if (!enabled_ || crumbs_.isEmpty()) { BreadcrumbBar::keyPressEvent(Event); return; }
    if (Event->key() == Qt::Key_Escape) { dismissMenu(); revealed_ = false; update(); return; }
    if (Event->key() == Qt::Key_Left || Event->key() == Qt::Key_Right) {
        revealed_ = true;
        if (keyboard_ == -2) keyboard_ = Event->key() == Qt::Key_Right ? firstVisible_ * 2 : -2;
        else if (keyboard_ == firstVisible_ * 2 && Event->key() == Qt::Key_Left && firstVisible_) keyboard_ = -2;
        else keyboard_ = std::clamp(keyboard_ + (Event->key() == Qt::Key_Left ? -1 : 1), firstVisible_ * 2, int(crumbs_.size()) * 2 - 1);
        update(); return;
    }
    if (Event->key() == Qt::Key_Down || Event->key() == Qt::Key_Space) {
        revealed_ = true; openMenu(keyboard_ < 0 ? -2 : keyboard_ | 1); update(); return;
    }
    if (Event->key() == Qt::Key_Return || Event->key() == Qt::Key_Enter) {
        if (revealed_ && keyboard_ >= 0 && keyboard_ % 2 == 0) navigate(crumbs_[keyboard_ / 2].path_);
        else if (revealed_) openMenu(keyboard_);
        else EditPath();
        return;
    }
    BreadcrumbBar::keyPressEvent(Event);
}

void PanelPathBar::navigate(const QString& Path)
{
    const auto Target = Path;
    clickTimer_.stop(); dismissMenu(); revealed_ = false; update();
    emit pathEntered(Target);
}

void PanelPathBar::dismissMenu()
{
    menuStop_.request_stop();
    if (menu_) { auto* Menu = menu_.data(); menu_.clear(); Menu->close(); Menu->deleteLater(); }
}

void PanelPathBar::openMenu(int Hit)
{
    dismissMenu();
    auto* Menu = new QMenu(this); menu_ = Menu;
    Menu->setObjectName("hc.panelPath.folders");
    Menu->setAccessibleName(tr("하위 폴더"));
    Menu->setAttribute(Qt::WA_DeleteOnClose);
    connect(Menu, &QMenu::aboutToHide, this, [this, Menu] {
        if (menu_ != Menu) return;
        menuStop_.request_stop(); menu_.clear();
        revealed_ = underMouse(); update();
    });
    const auto AddPath = [this, Menu](const QString& Label, const QString& Path) {
        auto* Action = Menu->addAction(QString(Label).replace('&', "&&")); Action->setData(Path);
        connect(Action, &QAction::triggered, this, [this, Path] { navigate(Path); });
    };
    if (Hit == -2) {
        for (int Index = 0; Index < firstVisible_; ++Index) AddPath(crumbs_[Index].label_, crumbs_[Index].path_);
        Menu->popup(mapToGlobal(overflow_.bottomLeft())); return;
    }
    const auto Index = Hit / 2;
    const auto Path = crumbs_[Index].path_;
    const auto Selected = Index + 1 < crumbs_.size() ? crumbs_[Index + 1].label_ : QString{};
    Menu->addAction(tr("폴더 목록을 불러오는 중…"))->setEnabled(false);
    Menu->popup(mapToGlobal(crumbs_[Index].arrow_.bottomLeft()));
    menuStop_ = std::stop_source{};
    auto* Watcher = new QFutureWatcher<FolderResult>(this);
    const QPointer<QMenu> Guard(Menu);
    connect(Watcher, &QFutureWatcher<FolderResult>::finished, this, [this, Watcher, Guard, Path, Selected] {
        const auto Result = Watcher->result(); Watcher->deleteLater();
        if (!Guard || menu_ != Guard || !Guard->isVisible()) return;
        Guard->clear();
        for (const auto& Name : Result.names_) {
            const auto Child = QDir::toNativeSeparators(QDir(Path).filePath(Name));
            auto* Action = Guard->addAction(QString(Name).replace('&', "&&")); Action->setData(Child);
            Action->setCheckable(true); Action->setChecked(Name.compare(Selected, Qt::CaseInsensitive) == 0);
            connect(Action, &QAction::triggered, this, [this, Child] { navigate(Child); });
        }
        if (!Result.error_.isEmpty()) Guard->addAction(tr("폴더 목록을 읽을 수 없습니다 (%1)").arg(Result.error_))->setEnabled(false);
        else if (Result.truncated_) Guard->addAction(tr("목록 일부만 표시합니다. 폴더로 이동해 전체 목록을 확인하세요."))->setEnabled(false);
        else if (Result.names_.isEmpty()) Guard->addAction(tr("하위 폴더가 없습니다"))->setEnabled(false);
    });
    // The task owns only values. Closing a menu or panel never waits for SMB I/O.
    Watcher->setFuture(QtConcurrent::run([Path, Stop = menuStop_.get_token()] { return ReadFolders(Path, Stop); }));
}

} // namespace hc::ui
