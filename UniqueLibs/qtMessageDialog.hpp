#pragma once

#include "qtDialogFrame.hpp"

#include <QCoreApplication>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>

namespace hc::ui {

namespace detail {
/// Primary follows the dialog's default button; without one, the first accept/yes button.
inline void MarkPrimary( QDialogButtonBox* Box, QPushButton* Default )
{
    if( !Box ) return;
    for( auto* Button : Box->buttons() ) fm::style::setButtonRole( Button, fm::style::ButtonRole::Normal );
    if( Default && Box->buttons().contains( Default ) ) { fm::style::setButtonRole( Default, fm::style::ButtonRole::Primary ); return; }
    for( auto* Button : Box->buttons() ) {
        const auto Role = Box->buttonRole( Button );
        if( Role == QDialogButtonBox::AcceptRole || Role == QDialogButtonBox::YesRole ) { fm::style::setButtonRole( Button, fm::style::ButtonRole::Primary ); return; }
    }
}
}

/// QMessageBox in the product dialog frame (board Prop-*): a header band with a tone glyph and the title,
/// the message in the body and a footer band with the buttons, the default (or accept) button Primary.
/// It stays a QMessageBox, so callers keep its API and tests keep finding it with qobject_cast<QMessageBox*>.
/// The frame is applied on the first show, after the caller has set text, buttons and details; QMessageBox
/// then sizes itself from that layout. The static functions mirror QMessageBox's.
class MessageDialog final : public QMessageBox
{
public:
    using QMessageBox::QMessageBox;

    static StandardButton information( QWidget* Parent, const QString& Title, const QString& Text, StandardButtons Buttons = Ok, StandardButton Default = NoButton )
    { return ask( Information, Parent, Title, Text, Buttons, Default ); }
    static StandardButton warning( QWidget* Parent, const QString& Title, const QString& Text, StandardButtons Buttons = Ok, StandardButton Default = NoButton )
    { return ask( Warning, Parent, Title, Text, Buttons, Default ); }
    static StandardButton critical( QWidget* Parent, const QString& Title, const QString& Text, StandardButtons Buttons = Ok, StandardButton Default = NoButton )
    { return ask( Critical, Parent, Title, Text, Buttons, Default ); }
    static StandardButton question( QWidget* Parent, const QString& Title, const QString& Text, StandardButtons Buttons = StandardButtons( Yes | No ), StandardButton Default = NoButton )
    { return ask( Question, Parent, Title, Text, Buttons, Default ); }

    static void about( QWidget* Parent, const QString& Title, const QString& Text )
    { MessageDialog Box( NoIcon, Title, Text, Ok, Parent ); Box.exec(); }

    /// Korean labels for the standard buttons, for callers that relied on relabelling them (ShowMSGBox).
    void TranslateStandardButtons()
    {
        const auto Tr = []( const char* Text ) { return QCoreApplication::translate( "hc::ui::MessageDialog", Text ); };
        const std::pair< StandardButton, const char* > Names[] = {
            { Ok, "확인" }, { Save, "저장" }, { SaveAll, "모두 저장" }, { Open, "열기" }, { Yes, "예" }, { YesToAll, "모두 예" },
            { No, "아니오" }, { NoToAll, "모두 아니오" }, { Abort, "중지" }, { Retry, "재시도" }, { Ignore, "무시" },
            { Close, "닫기" }, { Cancel, "취소" }, { Apply, "적용" } };
        for( const auto& [ Which, Text ] : Names ) if( auto* Button = button( Which ) ) Button->setText( Tr( Text ) );
    }

    void setVisible( bool Visible ) override
    {
        // The header band becomes the title bar, so the window is created without the Windows frame.
        if( Visible && !framed_ && !testAttribute( Qt::WA_WState_Created ) ) setWindowFlag( Qt::FramelessWindowHint );
        QMessageBox::setVisible( Visible );
    }

protected:
    void showEvent( QShowEvent* Event ) override
    {
        if( !framed_ ) frame();
        QMessageBox::showEvent( Event );     // may add the automatic OK button, then sizes from the framed layout
        detail::MarkPrimary( findChild< QDialogButtonBox* >(), defaultButton() );
    }

private:
    static StandardButton ask( Icon Kind, QWidget* Parent, const QString& Title, const QString& Text, StandardButtons Buttons, StandardButton Default )
    {
        MessageDialog Box( Kind, Title, Text, Buttons, Parent );
        if( Default != NoButton ) Box.setDefaultButton( Default );
        if( Box.exec() == -1 ) return Cancel;
        return Box.standardButton( Box.clickedButton() );
    }
    void frame()
    {
        framed_ = true;
        if( objectName().isEmpty() ) setObjectName( QStringLiteral( "hc.messageDialog" ) );
        fm::ui::glyph::Glyph Glyph = fm::ui::glyph::Info; auto Tone = fm::ui::DialogHeader::Info;
        if( icon() == Warning ) { Glyph = fm::ui::glyph::Warning; Tone = fm::ui::DialogHeader::Warn; }
        else if( icon() == Critical ) { Glyph = fm::ui::glyph::Warning; Tone = fm::ui::DialogHeader::Danger; }
        else if( icon() == NoIcon ) { Glyph = fm::ui::glyph::App; Tone = fm::ui::DialogHeader::Mute; }
        // The header glyph replaces the large message icon.
        if( auto* IconLabel = findChild< QLabel* >( QStringLiteral( "qt_msgboxex_icon_label" ) ) ) IconLabel->hide();
        auto* Footer = ApplyDialogFrame( this, { windowTitle(), {}, Glyph } );
        if( auto* Header = findChild< fm::ui::DialogHeader* >( QStringLiteral( "hc.dialog.header" ) ) ) Header->setTone( Tone );
        MoveToFooter( Footer, findChild< QDialogButtonBox* >() );
    }

    bool framed_ = false;
};

/// QInputDialog in the product dialog frame: header band with the title, the prompt and the field in the body,
/// OK · Cancel in the footer band. It stays a QInputDialog; the static functions mirror QInputDialog's.
class InputDialog final : public QInputDialog
{
public:
    using QInputDialog::QInputDialog;

    static QString getText( QWidget* Parent, const QString& Title, const QString& Label, QLineEdit::EchoMode Mode = QLineEdit::Normal,
                            const QString& Text = {}, bool* Ok = nullptr, Qt::WindowFlags Flags = {}, Qt::InputMethodHints Hints = Qt::ImhNone )
    {
        InputDialog Dialog( Parent, Flags );
        Dialog.setWindowTitle( Title ); Dialog.setLabelText( Label ); Dialog.setTextValue( Text );
        Dialog.setTextEchoMode( Mode ); Dialog.setInputMethodHints( Hints );
        const bool Accepted = Dialog.exec() == QDialog::Accepted; if( Ok ) *Ok = Accepted;
        return Accepted ? Dialog.textValue() : QString();
    }
    static QString getItem( QWidget* Parent, const QString& Title, const QString& Label, const QStringList& Items, int Current = 0, bool Editable = true,
                            bool* Ok = nullptr, Qt::WindowFlags Flags = {}, Qt::InputMethodHints Hints = Qt::ImhNone )
    {
        const QString Initial = Items.value( Current );
        InputDialog Dialog( Parent, Flags );
        Dialog.setWindowTitle( Title ); Dialog.setLabelText( Label ); Dialog.setComboBoxItems( Items ); Dialog.setTextValue( Initial );
        Dialog.setComboBoxEditable( Editable ); Dialog.setInputMethodHints( Hints );
        const bool Accepted = Dialog.exec() == QDialog::Accepted; if( Ok ) *Ok = Accepted;
        return Accepted ? Dialog.textValue() : Initial;
    }

    static QString getMultiLineText( QWidget* Parent, const QString& Title, const QString& Label, const QString& Text = {}, bool* Ok = nullptr,
                                     Qt::WindowFlags Flags = {}, Qt::InputMethodHints Hints = Qt::ImhNone )
    {
        InputDialog Dialog( Parent, Flags );
        Dialog.setOptions( QInputDialog::UsePlainTextEditForTextInput );
        Dialog.setWindowTitle( Title ); Dialog.setLabelText( Label ); Dialog.setTextValue( Text ); Dialog.setInputMethodHints( Hints );
        const bool Accepted = Dialog.exec() == QDialog::Accepted; if( Ok ) *Ok = Accepted;
        return Accepted ? Dialog.textValue() : QString();
    }
    static int getInt( QWidget* Parent, const QString& Title, const QString& Label, int Value = 0, int Minimum = -2147483647, int Maximum = 2147483647,
                       int Step = 1, bool* Ok = nullptr, Qt::WindowFlags Flags = {} )
    {
        InputDialog Dialog( Parent, Flags );
        Dialog.setWindowTitle( Title ); Dialog.setLabelText( Label );
        Dialog.setIntRange( Minimum, Maximum ); Dialog.setIntValue( Value ); Dialog.setIntStep( Step );
        const bool Accepted = Dialog.exec() == QDialog::Accepted; if( Ok ) *Ok = Accepted;
        return Accepted ? Dialog.intValue() : Value;
    }
    static double getDouble( QWidget* Parent, const QString& Title, const QString& Label, double Value = 0, double Minimum = -2147483647,
                             double Maximum = 2147483647, int Decimals = 1, bool* Ok = nullptr, Qt::WindowFlags Flags = {}, double Step = 1 )
    {
        InputDialog Dialog( Parent, Flags );
        Dialog.setWindowTitle( Title ); Dialog.setLabelText( Label ); Dialog.setDoubleDecimals( Decimals );
        Dialog.setDoubleRange( Minimum, Maximum ); Dialog.setDoubleValue( Value ); Dialog.setDoubleStep( Step );
        const bool Accepted = Dialog.exec() == QDialog::Accepted; if( Ok ) *Ok = Accepted;
        return Accepted ? Dialog.doubleValue() : Value;
    }

    void setVisible( bool Visible ) override
    {
        if( Visible && !framed_ && !testAttribute( Qt::WA_WState_Created ) ) setWindowFlag( Qt::FramelessWindowHint );
        QInputDialog::setVisible( Visible );
    }

protected:
    void showEvent( QShowEvent* Event ) override
    {
        // QInputDialog builds its layout in setVisible, before this event.
        if( !framed_ ) {
            framed_ = true;
            if( objectName().isEmpty() ) setObjectName( QStringLiteral( "hc.inputDialog" ) );
            auto* Buttons = findChild< QDialogButtonBox* >();
            ApplyDialogFrame( this, Buttons, { windowTitle(), {}, fm::ui::glyph::Rename } );
            detail::MarkPrimary( Buttons, nullptr );
        }
        QInputDialog::showEvent( Event );
    }

private:
    bool framed_ = false;
};

}
