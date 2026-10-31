#include "stdafx.h"
#include "imageReadingNavigator.hpp"
#include "imageReadingView.hpp"
#include "core/AppSettings.h"
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace hc::document
{
namespace
{
enum class Glyph { Previous, Next, Up, Down, PreviousVolume, NextVolume, Pages, Minus, Plus, Pin, More };
QIcon NavigatorIcon(Glyph Value)
{
    QIcon Icon;
    // Render both raster densities explicitly; the overlay stays legible in light themes too.
    for (int Scale : {1, 2, 3})
    {
        QPixmap Pixmap(24 * Scale, 24 * Scale);
        Pixmap.setDevicePixelRatio(Scale);
        Pixmap.fill(Qt::transparent);
        QPainter P(&Pixmap);
        P.setRenderHint(QPainter::Antialiasing);
        P.setPen(QPen(QColor("#f1f4f8"), 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        auto Chevron = [&](qreal X, bool Right) {
            QPainterPath Path;
            Path.moveTo(X + (Right ? -2.5 : 2.5), 6);
            Path.lineTo(X + (Right ? 2.5 : -2.5), 12);
            Path.lineTo(X + (Right ? -2.5 : 2.5), 18);
            P.drawPath(Path);
        };
        if (Value == Glyph::Previous || Value == Glyph::Next) Chevron(12, Value == Glyph::Next);
        else if (Value == Glyph::PreviousVolume || Value == Glyph::NextVolume) {
            Chevron(8.5, Value == Glyph::NextVolume); Chevron(15.5, Value == Glyph::NextVolume);
        }
        else if (Value == Glyph::Up || Value == Glyph::Down) {
            P.translate(12, 12); P.rotate(Value == Glyph::Up ? 90 : -90); P.translate(-12, -12); Chevron(12, false);
        }
        else if (Value == Glyph::Minus || Value == Glyph::Plus) {
            P.drawLine(QPointF(5, 12), QPointF(19, 12));
            if (Value == Glyph::Plus) P.drawLine(QPointF(12, 5), QPointF(12, 19));
        }
        else if (Value == Glyph::Pages) {
            for (int Y : {4, 14}) for (int X : {4, 14}) P.drawRoundedRect(QRectF(X, Y, 6, 6), 1, 1);
        }
        else if (Value == Glyph::More) {
            P.setPen(Qt::NoPen); P.setBrush(QColor("#f1f4f8"));
            for (int X : {5, 12, 19}) P.drawEllipse(QPointF(X, 12), 1.5, 1.5);
        }
        else {
            QPainterPath Path;
            Path.moveTo(10, 3); Path.lineTo(20, 13); Path.lineTo(16, 13); Path.lineTo(13, 16);
            Path.lineTo(13, 20); Path.lineTo(4, 11); Path.lineTo(8, 11); Path.lineTo(11, 8); Path.closeSubpath();
            P.drawPath(Path); P.drawLine(QPointF(7, 17), QPointF(3, 21));
        }
        P.end();
        Icon.addPixmap(Pixmap);
    }
    return Icon;
}
}
ImageReadingNavigator::ImageReadingNavigator(ImageReadingView *Reader) : QWidget(Reader->viewport()), reader_(Reader)
{
    setObjectName("imageReadingNavigator");
    setAccessibleName(tr("읽기 네비게이터"));
    setAutoFillBackground(false);
    setStyleSheet(QStringLiteral(
        "#imageReadingNavigator { color: #f1f4f8; }"
        "#imageReadingNavigator QToolButton { background: #252a32; border: 1px solid #59616d; border-radius: 18px; padding: 0px; }"
        "#imageReadingNavigator QToolButton:hover, #imageReadingNavigator QToolButton:focus { background: #404b59; border-color: #86b7e7; }"
        "#imageReadingNavigator QToolButton:checked { background: #35516d; border-color: #86b7e7; }"
        "#imageReadingNavigator QToolButton:disabled { background: transparent; }"
        "#imageReadingNavigator QComboBox { color: #f1f4f8; background: #282d35; border: 1px solid #59616d; border-radius: 7px; padding: 4px 8px; }"
        "#imageReadingNavigator QComboBox:focus { border-color: #86b7e7; }"
        "#imageReadingNavigator QComboBox QAbstractItemView { color: #f1f4f8; background: #282d35; selection-background-color: #35516d; }"
        "#imageReadingNavigator QLabel { color: #f1f4f8; background: transparent; }"
        "#imageReadingNavigator QSlider { background: transparent; min-height: 24px; }"
        "#imageReadingNavigator QSlider::groove:horizontal { height: 6px; background: #545960; border-radius: 3px; }"
        "#imageReadingNavigator QSlider::sub-page:horizontal { background: #86b7e7; border-radius: 3px; }"
        "#imageReadingNavigator QSlider::handle:horizontal { width: 16px; margin: -5px 0; background: #f1f4f8; border-radius: 8px; }"));
    auto *Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(20, 9, 20, 10); Layout->setSpacing(4);
    Layout->setSizeConstraint(QLayout::SetNoConstraint);
    auto *Top = new QHBoxLayout;
    progress_ = new QSlider(Qt::Horizontal, this);
    progress_->setObjectName("imageNavigatorProgress");
    progress_->setAccessibleName(tr("읽기 위치")); progress_->setMinimumWidth(20);
    position_ = new QLabel(this); position_->setMinimumWidth(88); position_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    position_->setObjectName("imageNavigatorPosition");
    Top->addWidget(progress_, 1); Top->addSpacing(12); Top->addWidget(position_); Layout->addLayout(Top);
    auto *Buttons = new QHBoxLayout;
    Buttons->setSpacing(6);
    auto Button = [&](const char *Name, const QString &Tip, Glyph Icon) {
        auto *B = new QToolButton(this); B->setObjectName(Name); B->setToolTip(Tip); B->setAccessibleName(Tip);
        B->setIcon(NavigatorIcon(Icon)); B->setIconSize(QSize(24, 24)); B->setFixedSize(36, 36);
        B->setToolButtonStyle(Qt::ToolButtonIconOnly); B->setFocusPolicy(Qt::TabFocus); Buttons->addWidget(B); return B;
    };
    pages_ = Button("imageNavigatorPages", tr("페이지 목록"), Glyph::Pages);
    mode_ = new QComboBox(this); mode_->setObjectName("imageNavigatorMode"); mode_->setAccessibleName(tr("읽기 모드"));
    mode_->addItems({tr("코믹"), tr("웹툰")}); Buttons->addWidget(mode_);
    Buttons->addStretch();
    previousVolume_ = Button("imageNavigatorPreviousVolume", tr("이전 권"), Glyph::PreviousVolume);
    previous_ = Button("imageNavigatorPrevious", tr("이전 쪽"), Glyph::Previous);
    fit_ = new QComboBox(this); fit_->setObjectName("imageNavigatorFit"); fit_->setAccessibleName(tr("이미지 맞춤"));
    fit_->addItems({tr("원본 크기"), tr("창에 맞춤"), tr("너비 맞춤"), tr("높이 맞춤")}); Buttons->addWidget(fit_);
    minus_ = Button("imageNavigatorZoomOut", tr("축소"), Glyph::Minus);
    plus_ = Button("imageNavigatorZoomIn", tr("확대"), Glyph::Plus);
    next_ = Button("imageNavigatorNext", tr("다음 쪽"), Glyph::Next);
    nextVolume_ = Button("imageNavigatorNextVolume", tr("다음 권"), Glyph::NextVolume);
    Buttons->addStretch();
    pin_ = Button("imageNavigatorPin", tr("네비게이터 고정"), Glyph::Pin); pin_->setCheckable(true);
    pin_->setChecked(AppSettings().value("imageView/navigatorPinned", false).toBool());
    more_ = Button("imageNavigatorMore", tr("읽기 설정"), Glyph::More); more_->setPopupMode(QToolButton::InstantPopup);
    Layout->addLayout(Buttons);
    for (auto *B : {previousVolume_, previous_, next_, nextVolume_, pages_})
        connect(B, &QToolButton::pressed, this, [this] { lastError_.clear(); message_.stop(); });
    connect(previousVolume_, &QToolButton::clicked, Reader, [Reader] { Reader->OpenVolume(-1); });
    connect(nextVolume_, &QToolButton::clicked, Reader, [Reader] { Reader->OpenVolume(1); });
    connect(previous_, &QToolButton::clicked, Reader, [Reader] { Reader->Move(-1); });
    connect(next_, &QToolButton::clicked, Reader, [Reader] { Reader->Move(1); });
    connect(minus_, &QToolButton::clicked, Reader, [Reader] { Reader->Zoom(.8); });
    connect(plus_, &QToolButton::clicked, Reader, [Reader] { Reader->Zoom(1.25); });
    connect(mode_, &QComboBox::activated, this, [this](int Index) {
        lastError_.clear(); message_.stop(); emit ModeRequested(Index + 1);
    });
    connect(this, &ImageReadingNavigator::ModeRequested, Reader, [Reader](int Mode) { Reader->SetMode(ImageReadingView::Mode(Mode)); });
    connect(fit_, &QComboBox::activated, Reader, &ImageReadingView::SetFit);
    connect(pages_, &QToolButton::clicked, this, &ImageReadingNavigator::showPages);
    connect(pin_, &QToolButton::toggled, this, [this](bool On) {
        if (!reader_->State()["private"].toBool() && !qApp->property("privateReading").toBool())
            AppSettings().setValue("imageView/navigatorPinned", On);
        Reveal();
    });
    // Seeking on release avoids decoding every page passed while dragging.
    connect(progress_, &QSlider::sliderReleased, this, [this] {
        reader_->SeekProgress(progress_->value()); Sync();
    });
    connect(progress_, &QSlider::valueChanged, this, [this](int Value) {
        if (!progress_->isSliderDown()) reader_->SeekProgress(Value);
    });
    connect(Reader, &ImageReadingView::Changed, this, &ImageReadingNavigator::Sync);
    connect(Reader, &ImageReadingView::FramePresented, this, [this] { Sync(); });
    idle_.setSingleShot(true); idle_.setInterval(2200);
    message_.setSingleShot(true); message_.setInterval(5000);
    connect(&message_, &QTimer::timeout, this, &ImageReadingNavigator::Sync);
    connect(&idle_, &QTimer::timeout, this, &ImageReadingNavigator::hideIdle);
    qApp->installEventFilter(this);
    hide();
}
ImageReadingNavigator::~ImageReadingNavigator() { qApp->removeEventFilter(this); }
void ImageReadingNavigator::SetOptionsMenu(QMenu *Menu) { more_->setMenu(Menu); }
void ImageReadingNavigator::Sync()
{
    const auto S = reader_->State();
    const int Mode = S["mode"].toInt(), Count = S["pageCount"].toInt();
    const bool WasEligible = eligible_;
    eligible_ = reader_->isVisible() && Count > 0 && Mode != ImageReadingView::Single;
    if (!eligible_) { idle_.stop(); hide(); return; }
    const bool Webtoon = Mode == ImageReadingView::Webtoon;
    { const QSignalBlocker Block(mode_); mode_->setCurrentIndex(Mode - 1); }
    { const QSignalBlocker Block(fit_); fit_->setCurrentIndex(Webtoon ? 2 : S["fit"].toInt()); }
    fit_->setEnabled(!Webtoon);
    if (!progress_->isSliderDown()) {
        const QSignalBlocker Block(progress_);
        progress_->setRange(0, Webtoon ? 1000000 : qMax(0, Count - 1));
        progress_->setValue(Webtoon ? reader_->verticalScrollBar()->value() : S["page"].toInt());
        progress_->setPageStep(Webtoon ? reader_->verticalScrollBar()->pageStep() : 1);
    }
    const int Page = S["page"].toInt() + 1, Last = S["lastPage"].toInt() + 1;
    position_->setText(Webtoon ? QString("%1% · %2 / %3").arg(qRound(reader_->verticalScrollBar()->value() / 10000.)).arg(Page).arg(Count)
                        : Last > Page ? QString("%1–%2 / %3").arg(Page).arg(Last).arg(Count)
                                      : QString("%1 / %2").arg(Page).arg(Count));
    progress_->setToolTip(Webtoon ? tr("전체 세로 진행 위치 · 이미지 %1 / %2").arg(Page).arg(Count) : tr("쪽 이동"));
    const auto Error = S["error"].toString();
    const bool NewError = !Error.isEmpty() && Error != lastError_;
    if (NewError) { lastError_ = Error; message_.start(); }
    const auto DisplayError = message_.isActive() ? lastError_ : Error;
    position_->setToolTip(DisplayError);
    if (!DisplayError.isEmpty()) position_->setText(tr("이동 불가"));
    const bool Busy = S["openingVolume"].toBool();
    previousVolume_->setEnabled(!Busy); nextVolume_->setEnabled(!Busy);
    previous_->setEnabled(!Busy && (S["page"].toInt() > 0 || reader_->verticalScrollBar()->value() > 0 || S["part"].toInt() > 0));
    next_->setEnabled(!Busy); progress_->setEnabled(!Busy && (Webtoon ? reader_->verticalScrollBar()->maximum() > 0 : Count > 1));
    if (lastMode_ != Mode) {
        previous_->setIcon(NavigatorIcon(Webtoon ? Glyph::Up : Glyph::Previous));
        next_->setIcon(NavigatorIcon(Webtoon ? Glyph::Down : Glyph::Next));
        previous_->setToolTip(Webtoon ? tr("이전 화면") : tr("이전 쪽")); next_->setToolTip(Webtoon ? tr("다음 화면") : tr("다음 쪽"));
        previous_->setAccessibleName(previous_->toolTip()); next_->setAccessibleName(next_->toolTip());
    }
    position();
    if (!WasEligible || lastMode_ != Mode || NewError) Reveal();
    lastMode_ = Mode;
}
void ImageReadingNavigator::position()
{
    const int Available = reader_->viewport()->width();
    for (auto *Widget : QList<QWidget *>{pages_, mode_, fit_, minus_, plus_}) {
        // Keep essential navigation reachable even in a narrow docked viewer.
        const bool Zoom = Widget == minus_ || Widget == plus_;
        Widget->setVisible(Available >= (Zoom ? 600 : 500));
    }
    layout()->activate();
    const int Width = qMin(qMax(0, Available - 16), qMax(680, sizeHint().width()));
    const int Height = sizeHint().height();
    setGeometry((Available - Width) / 2, qMax(0, reader_->viewport()->height() - Height - 36), Width, Height);
}
void ImageReadingNavigator::Reveal()
{
    if (!eligible_) return;
    position(); show(); raise(); reader_->window()->unsetCursor(); idle_.start();
}
void ImageReadingNavigator::hideIdle()
{
    auto *Focus = QApplication::focusWidget();
    if (pin_->isChecked() || underMouse() || (Focus && (Focus == this || isAncestorOf(Focus))) ||
        QApplication::activePopupWidget() || QApplication::activeModalWidget() || QApplication::mouseButtons() != Qt::NoButton) {
        idle_.start(); return;
    }
    hide();
}
bool ImageReadingNavigator::eventFilter(QObject *Object, QEvent *Event)
{
    auto *Widget = qobject_cast<QWidget *>(Object);
    if (!Widget || (Widget != reader_ && !reader_->isAncestorOf(Widget))) return false;
    if (Object == reader_ && (Event->type() == QEvent::Show || Event->type() == QEvent::Resize))
        QTimer::singleShot(0, this, [this] { Sync(); });
    if (Object == reader_ && Event->type() == QEvent::Hide) { eligible_ = false; idle_.stop(); hide(); }
    if (!eligible_) return false;
    if (Event->type() == QEvent::ShortcutOverride) {
        auto *Key = static_cast<QKeyEvent *>(Event);
        if (Key->key() == Qt::Key_F6 || (Key->key() == Qt::Key_Escape && isAncestorOf(Widget))) {
            Event->accept(); return true;
        }
    }
    if (Event->type() == QEvent::MouseMove) {
        const auto Point = reader_->viewport()->mapFromGlobal(static_cast<QMouseEvent *>(Event)->globalPosition().toPoint());
        if (Point.y() >= reader_->viewport()->height() - height() - 60) Reveal();
    }
    if (Event->type() == QEvent::FocusIn && (Widget == this || isAncestorOf(Widget))) Reveal();
    if (Event->type() == QEvent::KeyPress) {
        auto *Key = static_cast<QKeyEvent *>(Event);
        if (Key->key() == Qt::Key_F6) { Reveal(); progress_->setFocus(Qt::ShortcutFocusReason); return true; }
        if (Key->key() == Qt::Key_Escape && isAncestorOf(Widget) && !QApplication::activePopupWidget()) {
            reader_->setFocus(); idle_.start(); return true;
        }
    }
    return false;
}
void ImageReadingNavigator::paintEvent(QPaintEvent *)
{
    QPainter P(this); P.setRenderHint(QPainter::Antialiasing);
    P.setPen(QPen(QColor(140, 152, 168, 150), 1)); P.setBrush(QColor(29, 33, 39, 238));
    P.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 30, 30);
}
void ImageReadingNavigator::showPages()
{
    QMenu Menu(this);
    auto *List = new QListWidget(&Menu); List->setAccessibleName(tr("페이지 목록"));
    const auto Names = reader_->PageNames();
    for (int I = 0; I < Names.size(); ++I) List->addItem(QString("%1  %2").arg(I + 1).arg(Names[I]));
    List->setCurrentRow(reader_->State()["page"].toInt()); List->scrollToItem(List->currentItem());
    List->setFixedSize(qMin(360, reader_->viewport()->width()), qMin(360, reader_->viewport()->height()));
    auto *Action = new QWidgetAction(&Menu); Action->setDefaultWidget(List); Menu.addAction(Action);
    connect(List, &QListWidget::itemActivated, &Menu, [this, List, &Menu] { reader_->GoTo(List->currentRow()); Menu.close(); });
    connect(List, &QListWidget::itemClicked, &Menu, [this, List, &Menu] { reader_->GoTo(List->currentRow()); Menu.close(); });
    QTimer::singleShot(0, List, [List] { List->setFocus(); });
    Menu.exec(pages_->mapToGlobal(QPoint(0, -List->height())));
    reader_->setFocus(); Reveal();
}
} // namespace hc::document
