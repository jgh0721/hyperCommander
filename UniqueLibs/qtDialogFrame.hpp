#pragma once

#include <QAbstractButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLayout>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/DialogFooter.h>
#include <fmwidgets/DialogHeader.h>

#include "qtDialogCaption.hpp"

namespace hc::ui {

struct DialogFrameOptions
{
    QString title_;                                     // header title; the window title when empty
    QString subtitle_;
    fm::ui::glyph::Glyph glyph_ = fm::ui::glyph::App;
    bool header_ = true;
    bool caption_ = true;                               // the header band is the title bar (board option B)
    bool outline_ = false;                              // distinct edges for overlapping file-job windows
};

namespace detail {
class DialogOutline final : public QWidget
{
public:
    explicit DialogOutline(QDialog* Dialog) : QWidget(Dialog), dialog_(Dialog)
    {
        setObjectName("hc.dialog.outline"); setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground); setFocusPolicy(Qt::NoFocus);
        Dialog->installEventFilter(this); setGeometry(Dialog->rect());
        QObject::connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, qOverload<>(&QWidget::update));
    }
protected:
    bool eventFilter(QObject* Object, QEvent* Event) override
    {
        if (Event->type() == QEvent::Resize || Event->type() == QEvent::Show) { setGeometry(dialog_->rect()); raise(); }
        if (Event->type() == QEvent::WindowActivate || Event->type() == QEvent::WindowDeactivate || Event->type() == QEvent::PaletteChange) update();
        return QWidget::eventFilter(Object, Event);
    }
    void paintEvent(QPaintEvent*) override
    {
        const auto& Colors = fm::style::themeColorsFor(this);
        const auto Color = Colors[dialog_->isActiveWindow() ? fm::style::Token::Focus : fm::style::Token::Fg2];
        QPainter Painter(this); Painter.setPen(QPen(Color, 2)); Painter.setBrush(Qt::NoBrush);
        Painter.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
    }
private:
    QDialog* dialog_;
};

/// Paper Fluent header band: a surface strip with a bottom line above the window-colored body.
class HeaderBand final : public QWidget
{
public:
    explicit HeaderBand( QWidget* Parent ) : QWidget( Parent )
    {
        QObject::connect( &fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, qOverload<>( &QWidget::update ) );
    }
protected:
    void paintEvent( QPaintEvent* ) override
    {
        const auto& Colors = fm::style::themeColorsFor( this );
        if( !Colors.isPaperFluent() ) return;
        QPainter Painter( this );
        Painter.fillRect( rect(), Colors[ fm::style::Token::Surface ] );
        Painter.fillRect( QRect( 0, height() - 1, width(), 1 ), Colors[ fm::style::Token::Line ] );
    }
};
}

/// Puts Header into a header band at the top of the dialog's layout. With Caption the band is the title bar:
/// it moves the window and carries minimize · close, so the dialog has no Windows title bar repeating the title.
/// Call before the dialog is first shown (MessageDialog · InputDialog set the frameless flag in setVisible).
inline QWidget* PlaceHeaderBand( QDialog* Dialog, QWidget* Header, bool Caption = true )
{
    auto* Band = new detail::HeaderBand( Dialog ); Band->setObjectName( QStringLiteral( "hc.dialog.headerBand" ) );
    auto* BandLayout = new QVBoxLayout( Band );
    // The right margin keeps the title clear of the caption buttons in the band's top-right corner.
    BandLayout->setContentsMargins( 20, 14, Caption ? 8 + DialogCaption::kButtonsWidth : 20, 12 );
    Header->setParent( Band ); BandLayout->addWidget( Header );
    if( auto* Layout = qobject_cast< QBoxLayout* >( Dialog->layout() ) ) Layout->insertWidget( 0, Band );
    if( Caption ) DialogCaption::Attach( Dialog, Band );
    return Band;
}

/// Gives a plain QDialog the Paper Fluent frame of DialogShell (board Prop-*): a header band, the existing
/// content in a padded body and a footer band. Widgets, object names and signal connections stay; only
/// their parent widget changes. Returns the footer row: a stretch first, so buttons added later sit right.
inline QHBoxLayout* ApplyDialogFrame( QDialog* Dialog, const DialogFrameOptions& Options = {} )
{
    // Changing flags recreates the window, which must not happen while a dialog is being shown.
    if( Dialog->windowFlags().testFlag( Qt::WindowContextHelpButtonHint ) && !Dialog->testAttribute( Qt::WA_WState_Created ) )
        Dialog->setWindowFlag( Qt::WindowContextHelpButtonHint, false );
    auto* Body = new QWidget( Dialog ); Body->setObjectName( QStringLiteral( "hc.dialog.body" ) );
    // setLayout takes the layout out of the dialog and moves its widgets into the body.
    if( auto* Content = Dialog->layout() ) { Body->setLayout( Content ); Content->setContentsMargins( 20, 12, 20, 12 ); }
    auto* Layout = new QVBoxLayout( Dialog ); Layout->setContentsMargins( 0, 0, 0, 0 ); Layout->setSpacing( 0 );
    Layout->addWidget( Body, 1 );
    if( Options.header_ ) {
        auto* Header = new fm::ui::DialogHeader( Dialog ); Header->setObjectName( QStringLiteral( "hc.dialog.header" ) );
        Header->setTitle( Options.title_.isEmpty() ? Dialog->windowTitle() : Options.title_ );
        Header->setSubtitle( Options.subtitle_ ); Header->setSubtitleWrap( true ); Header->setGlyph( Options.glyph_ );
        PlaceHeaderBand( Dialog, Header, Options.caption_ );
    }
    auto* Footer = new fm::ui::DialogFooter( Dialog ); Footer->setObjectName( QStringLiteral( "hc.dialog.footer" ) );
    auto* Row = new QHBoxLayout( Footer ); Row->setContentsMargins( 20, 12, 20, 12 ); Row->setSpacing( 8 ); Row->addStretch( 1 );
    Layout->addWidget( Footer );
    if (Options.outline_) { Layout->setContentsMargins(2, 2, 2, 2); new detail::DialogOutline(Dialog); }
    return Row;
}

/// Moves a button box or a single button into the footer. A button box's first accept/yes button becomes
/// the Primary (accent) button; pass Primary for a lone button that is the dialog's main action.
inline void MoveToFooter( QHBoxLayout* Footer, QWidget* Buttons, bool Primary = false )
{
    if( !Footer || !Buttons ) return;
    Buttons->setParent( Footer->parentWidget() ); Footer->addWidget( Buttons );
    if( auto* Box = qobject_cast< QDialogButtonBox* >( Buttons ) ) {
        for( auto* Button : Box->buttons() ) {
            const auto Role = Box->buttonRole( Button );
            if( Role == QDialogButtonBox::AcceptRole || Role == QDialogButtonBox::YesRole ) { fm::style::setButtonRole( Button, fm::style::ButtonRole::Primary ); break; }
        }
    } else if( Primary ) fm::style::setButtonRole( Buttons, fm::style::ButtonRole::Primary );
}

/// Wraps a dialog whose button box is already in its layout: header + body + footer with that box.
inline QHBoxLayout* ApplyDialogFrame( QDialog* Dialog, QDialogButtonBox* Buttons, const DialogFrameOptions& Options = {} )
{
    auto* Footer = ApplyDialogFrame( Dialog, Options ); MoveToFooter( Footer, Buttons ); return Footer;
}

}
