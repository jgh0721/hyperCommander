#pragma once

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QShortcut>
#include <QToolButton>
#include <QWindow>
#include <QtMath>

#include <fmstyle/ThemeManager.h>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace hc::ui {

/// Board option B: the dialog drops the Windows title bar and its header band becomes the title bar. The band moves
/// the window, Alt+Space opens the system menu, a resizable dialog keeps its edges, and minimize · close sit at the
/// band's top right. Minimize takes the owner window down too, so a job dialog and the main window leave together
/// (Windows hides owned windows with their owner and brings them back with it). The window title is kept for
/// Alt+Tab, the taskbar and screen readers.
class DialogCaption final : public QObject, public QAbstractNativeEventFilter
{
public:
    static constexpr int kButtonWidth = 46;
    static constexpr int kButtonHeight = 32;
    static constexpr int kButtonsWidth = 2 * kButtonWidth;

    /// Call before the dialog is first shown, or from setVisible/showEvent once the frameless flag is set.
    /// A dialog whose native frame already exists keeps it.
    static DialogCaption* Attach( QDialog* Dialog, QWidget* Band )
    {
        if( !Dialog || !Band ) return nullptr;
        if( !Dialog->windowFlags().testFlag( Qt::FramelessWindowHint ) ) {
            if( Dialog->testAttribute( Qt::WA_WState_Created ) ) return nullptr;
            Dialog->setWindowFlag( Qt::FramelessWindowHint );
        }
        return new DialogCaption( Dialog, Band );
    }
    ~DialogCaption() override { if( qApp ) qApp->removeNativeEventFilter( this ); }

protected:
    bool eventFilter( QObject* Object, QEvent* Event ) override
    {
        if( Object == dialog_ ) {
            switch( Event->type() ) {
                case QEvent::Show: case QEvent::WinIdChange: applyStyle(); place(); break;
                case QEvent::Resize: case QEvent::LayoutRequest: place(); break;
                case QEvent::ActivationChange: refresh(); break;
                default: break;
            }
        } else if( Object == band_ && Event->type() == QEvent::MouseButtonPress && dialog_ ) {
            // Presses the header ignores reach the band: left drags the window, right opens the system menu.
            auto* Mouse = static_cast< QMouseEvent* >( Event );
            if( Mouse->button() == Qt::LeftButton && dialog_->windowHandle() ) { dialog_->windowHandle()->startSystemMove(); return true; }
            if( Mouse->button() == Qt::RightButton ) { systemMenu( Mouse->globalPosition().toPoint() ); return true; }
        }
        return QObject::eventFilter( Object, Event );
    }
    bool nativeEventFilter( const QByteArray&, void* Message, qintptr* Result ) override
    {
#ifdef Q_OS_WIN
        const auto* Msg = static_cast< MSG* >( Message );
        if( !dialog_ || Msg->message != WM_NCHITTEST || Msg->hwnd != reinterpret_cast< HWND >( dialog_->internalWinId() ) || !resizable() ) return false;
        RECT Bounds; GetWindowRect( Msg->hwnd, &Bounds );
        const int X = GET_X_LPARAM( Msg->lParam ), Y = GET_Y_LPARAM( Msg->lParam );
        const int Border = qRound( 6 * dialog_->devicePixelRatioF() );
        const bool Left = X < Bounds.left + Border, Right = X >= Bounds.right - Border;
        const bool Top = Y < Bounds.top + Border, Bottom = Y >= Bounds.bottom - Border;
        if( !( Left || Right || Top || Bottom ) ) return false;
        if( Result ) *Result = Top ? ( Left ? HTTOPLEFT : Right ? HTTOPRIGHT : HTTOP ) : Bottom ? ( Left ? HTBOTTOMLEFT : Right ? HTBOTTOMRIGHT : HTBOTTOM ) : Left ? HTLEFT : HTRIGHT;
        return true;
#else
        Q_UNUSED( Message ); Q_UNUSED( Result ); return false;
#endif
    }

private:
    DialogCaption( QDialog* Dialog, QWidget* Band ) : QObject( Dialog ), dialog_( Dialog ), band_( Band )
    {
        setObjectName( QStringLiteral( "hc.dialog.caption" ) );
        const auto Add = [ Dialog ]( const char* Name, const QString& Text ) {
            auto* Button = new QToolButton( Dialog ); Button->setObjectName( QString::fromLatin1( Name ) );
            Button->setAccessibleName( Text ); Button->setToolTip( Text ); Button->setFocusPolicy( Qt::TabFocus );
            Button->setAutoRaise( true ); Button->setFixedSize( kButtonWidth, kButtonHeight ); Button->setIconSize( QSize( 10, 10 ) );
            return Button;
        };
        minimize_ = Add( "hc.dialog.minimize", QCoreApplication::translate( "hc::ui::DialogCaption", "최소화" ) );
        close_ = Add( "hc.dialog.close", QCoreApplication::translate( "hc::ui::DialogCaption", "닫기" ) );
        QObject::connect( minimize_, &QToolButton::clicked, this, [ this ] { minimize(); } );
        QObject::connect( close_, &QToolButton::clicked, Dialog, &QWidget::close );
        Dialog->installEventFilter( this ); Band->installEventFilter( this );
        qApp->installNativeEventFilter( this );
        auto* MenuKey = new QShortcut( QKeySequence( Qt::ALT | Qt::Key_Space ), Dialog );
        QObject::connect( MenuKey, &QShortcut::activated, this, [ this ] { if( band_ ) systemMenu( band_->mapToGlobal( QPoint( 0, band_->height() ) ) ); } );
        QObject::connect( &fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, [ this ] { refresh(); } );
        refresh(); place();
        if( Dialog->testAttribute( Qt::WA_WState_Created ) ) applyStyle();
    }

    bool resizable() const { return dialog_ && dialog_->minimumSize() != dialog_->maximumSize(); }

    void place()
    {
        if( !dialog_ ) return;
        const int Top = band_ ? band_->mapTo( dialog_, QPoint() ).y() : 0;
        minimize_->move( dialog_->width() - kButtonsWidth, Top ); close_->move( dialog_->width() - kButtonWidth, Top );
        // Buttons added while the dialog is already showing (message · input windows) are not shown implicitly.
        minimize_->raise(); close_->raise(); minimize_->show(); close_->show();
    }

    static QIcon captionIcon( bool Close, const QColor& Color, const QColor& Hover )
    {
        QIcon Icon;
        for( const qreal Dpr : { 1.0, 1.25, 1.5, 2.0 } )
            for( const auto Mode : { QIcon::Normal, QIcon::Active } ) {
                QPixmap Pixmap( qCeil( 10 * Dpr ), qCeil( 10 * Dpr ) ); Pixmap.setDevicePixelRatio( Dpr ); Pixmap.fill( Qt::transparent );
                QPainter Painter( &Pixmap ); Painter.setRenderHint( QPainter::Antialiasing, Close ); Painter.setPen( QPen( Mode == QIcon::Active ? Hover : Color, 1.0 ) );
                if( Close ) { Painter.drawLine( QPointF( 0.5, 0.5 ), QPointF( 9.5, 9.5 ) ); Painter.drawLine( QPointF( 9.5, 0.5 ), QPointF( 0.5, 9.5 ) ); }
                else Painter.drawLine( QPointF( 0, 5.5 ), QPointF( 10, 5.5 ) );
                Painter.end(); Icon.addPixmap( Pixmap, Mode );
            }
        return Icon;
    }

    void refresh()
    {
        if( !dialog_ ) return;
        // Same caption buttons as the product title bar: transparent, a light overlay on hover, close turns danger.
        const auto& Colors = fm::style::themeColorsFor( dialog_ );
        auto Fg = Colors[ fm::style::Token::Fg2 ]; if( !dialog_->isActiveWindow() ) Fg.setAlphaF( 0.7f );
        const auto Danger = Colors[ fm::style::Token::DangerFill ];
        const auto Css = QString( "QToolButton {background:transparent;border:0;} QToolButton:hover {background:rgba(%1,%2,%3,36);} QToolButton:pressed {background:rgba(%1,%2,%3,60);}" )
            .arg( Fg.red() ).arg( Fg.green() ).arg( Fg.blue() );
        minimize_->setStyleSheet( Css );
        close_->setStyleSheet( Css + QString( " QToolButton:hover {background:%1;} QToolButton:pressed {background:%2;}" ).arg( Danger.name(), Danger.darker( 115 ).name() ) );
        minimize_->setIcon( captionIcon( false, Fg, Colors[ fm::style::Token::Fg ] ) );
        close_->setIcon( captionIcon( true, Fg, Colors[ fm::style::Token::OnDanger ] ) );
    }

    void applyStyle()
    {
#ifdef Q_OS_WIN
        // Thick frame keeps Snap and the DWM shadow; the system menu and minimize box back Alt+Space and minimize.
        if( !dialog_ || !dialog_->testAttribute( Qt::WA_WState_Created ) ) return;
        const auto Handle = reinterpret_cast< HWND >( dialog_->winId() );
        const auto Style = GetWindowLongPtrW( Handle, GWL_STYLE );
        const auto Wanted = Style | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX;
        if( Wanted == Style ) return;
        SetWindowLongPtrW( Handle, GWL_STYLE, Wanted );
        SetWindowPos( Handle, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
#endif
    }

    void minimize()
    {
        if( !dialog_ ) return;
        // The owner goes down first; Windows hides its owned dialogs and restores them with it.
        auto* Owner = dialog_->parentWidget() ? dialog_->parentWidget()->window() : nullptr;
        if( Owner && Owner->isVisible() ) Owner->showMinimized(); else dialog_->showMinimized();
    }

    void systemMenu( const QPoint& Global )
    {
#ifdef Q_OS_WIN
        if( !dialog_ ) return;
        const auto Handle = reinterpret_cast< HWND >( dialog_->winId() );
        const auto Menu = GetSystemMenu( Handle, FALSE );
        EnableMenuItem( Menu, SC_MAXIMIZE, MF_BYCOMMAND | MF_GRAYED ); EnableMenuItem( Menu, SC_RESTORE, MF_BYCOMMAND | MF_GRAYED );
        EnableMenuItem( Menu, SC_SIZE, MF_BYCOMMAND | ( resizable() ? MF_ENABLED : MF_GRAYED ) );
        const auto Scale = dialog_->devicePixelRatioF(); const auto Local = dialog_->mapFromGlobal( Global );
        POINT Native{ qRound( Local.x() * Scale ), qRound( Local.y() * Scale ) }; ClientToScreen( Handle, &Native );
        const auto Command = TrackPopupMenu( Menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, Native.x, Native.y, 0, Handle, nullptr );
        if( Command == SC_MINIMIZE ) minimize(); else if( Command ) PostMessageW( Handle, WM_SYSCOMMAND, Command, 0 );
#else
        Q_UNUSED( Global );
#endif
    }

    QPointer< QDialog > dialog_;
    QPointer< QWidget > band_;
    QToolButton*        minimize_ = nullptr;
    QToolButton*        close_ = nullptr;
};

}
