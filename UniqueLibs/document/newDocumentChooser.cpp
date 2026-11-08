#include "stdafx.h"
#include "newDocumentChooser.hpp"

/// 모듈
#include "UniqueLibs/qtDialogFrame.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/DialogWidgets.h>
#include <fmwidgets/Glyph.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        using fm::style::Token;
        constexpr qint64  PixelLimit = 128LL * 1024 * 1024;
        const QStringList KindIds{ "text", "markdown", "epub", "image" };

        // The dialog message line: secondary text, or the danger colour for a problem (as in 새 EPUB 만들기).
        void Tint( QLabel* Label, Token Which )
        {
            auto Palette = Label->palette();
            Palette.setColor( QPalette::WindowText, fm::style::themeColorsFor( Label )[ Which ] );
            Label->setPalette( Palette );
        }
        QLabel* Message( QWidget* Parent, const char* Name )
        {
            auto* Label = new QLabel( Parent ); Label->setObjectName( Name );
            Label->setTextFormat( Qt::PlainText ); Label->setWordWrap( true );
            Label->setMinimumHeight( Label->fontMetrics().lineSpacing() * 2 + 2 );
            Label->setAlignment( Qt::AlignLeft | Qt::AlignTop );
            return Label;
        }
        bool Repeats( const QKeySequence& Key, QKeyEvent* Event )
        {
            return !Key.isEmpty() && QKeySequence( Event->keyCombination() ).matches( Key ) == QKeySequence::ExactMatch;
        }
    }

    QString NewDocumentKindId( NewDocumentKind Kind ) { return KindIds.value( int( Kind ), KindIds.first() ); }

    NewDocumentKind NewDocumentKindFrom( const QString& Id, NewDocumentKind Fallback )
    {
        const auto Index = KindIds.indexOf( Id );
        return Index < 0 ? Fallback : NewDocumentKind( Index );
    }

    QString NewImageError( int Width, int Height )
    {
        if( Width < 1 || Height < 1 || Width > 16384 || Height > 16384 )
            return QObject::tr( "가로 · 세로는 1 – 16384 px로 입력하세요." );
        if( qint64( Width ) * Height > PixelLimit )
            return QObject::tr( "전체 픽셀 수가 128 MP(512 MiB)를 넘어 만들 수 없습니다." );
        return {};
    }

    // ── 새 문서 ─────────────────────────────────────────────────────────

    NewDocumentChooser::NewDocumentChooser( QWidget* Parent, const Options& Value )
        : QDialog( Parent ), options_( Value )
    {
        setObjectName( "newDocumentChooser" );
        setWindowTitle( tr( "새 문서" ) );
        setAttribute( Qt::WA_DeleteOnClose );
        auto* Layout = new QVBoxLayout( this ); Layout->setSpacing( 6 );
        struct Row { QString name_, detail_; fm::ui::ChoiceCard::FileKind kind_; };
        const QList< Row > Rows{
            { tr( "텍스트 문서" ), QStringLiteral( ".txt" ), fm::ui::ChoiceCard::DocKind },
            { tr( "Markdown 문서" ), QStringLiteral( ".md" ), fm::ui::ChoiceCard::CodeKind },
            { tr( "EPUB 책…" ), QStringLiteral( ".epub" ), fm::ui::ChoiceCard::ArchiveKind },
            { tr( "이미지…" ), QStringLiteral( ".png" ), fm::ui::ChoiceCard::ImageKind } };
        auto* Group = new QButtonGroup( this ); Group->setExclusive( true );
        for( int I = 0; I < Rows.size(); ++I ) {
            auto* Card = new fm::ui::ChoiceCard( Rows[ I ].name_, this );
            Card->setObjectName( "newDocument." + KindIds[ I ] );
            Card->setDetail( Rows[ I ].detail_ ); Card->setKeyHint( QString::number( I + 1 ) ); Card->setFileKind( Rows[ I ].kind_ );
            Card->setAccessibleName( Rows[ I ].name_ );
            Group->addButton( Card, I ); Card->installEventFilter( this );
            connect( Card, &QAbstractButton::toggled, this, [ this ]( bool On ) { if( On ) refreshNote(); } );
            kinds_.append( Card ); Layout->addWidget( Card );
        }
        if( auto* Initial = kinds_.value( int( options_.initial_ ) ) ) Initial->setChecked( true );
        note_ = Message( this, "newDocumentNote" ); Tint( note_, Token::Fg2 );
        Layout->addSpacing( 4 ); Layout->addWidget( note_ );
        auto* Buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
        create_ = Buttons->button( QDialogButtonBox::Ok ); create_->setObjectName( "newDocument.create" );
        Buttons->button( QDialogButtonBox::Cancel )->setText( tr( "취소(&C)" ) );
        Layout->addWidget( Buttons );
        connect( Buttons, &QDialogButtonBox::accepted, this, &NewDocumentChooser::accept );
        connect( Buttons, &QDialogButtonBox::rejected, this, &NewDocumentChooser::reject );
        hc::ui::ApplyDialogFrame( this, Buttons, { {}, tr( "만들 문서의 종류를 고릅니다. 1 – 4 키나 두 번 누르기로 바로 만들 수 있습니다." ), fm::ui::glyph::NewFile } );
        refreshNote();
        resize( 480, sizeHint().height() );
        if( auto* Card = kinds_.value( checkedKind() ) ) Card->setFocus( Qt::OtherFocusReason );
    }

    int NewDocumentChooser::checkedKind() const
    {
        for( int I = 0; I < kinds_.size(); ++I ) if( kinds_[ I ]->isChecked() ) return I;
        return 0;
    }

    void NewDocumentChooser::refreshNote()
    {
        if( !note_ || !create_ ) return;
        const auto Kind = NewDocumentKind( checkedKind() );
        // EPUB and image continue in their own dialog: the button says so.
        create_->setText( Kind == NewDocumentKind::Epub || Kind == NewDocumentKind::Image ? tr( "다음(&O)…" ) : tr( "만들기(&O)" ) );
        switch( Kind ) {
            case NewDocumentKind::Text:
                note_->setText( tr( "‘새 문서 %1’ 탭으로 엽니다. 저장할 때 이름과 위치를 정합니다." ).arg( options_.nextDocument_ ) ); break;
            case NewDocumentKind::Markdown:
                note_->setText( options_.viewer_
                    ? tr( "‘새 문서 %1.md’ 탭을 소스 보기로 엽니다." ).arg( options_.nextDocument_ )
                    : tr( "‘새 문서 %1.md’ 탭으로 엽니다. 소스와 미리보기를 나란히 봅니다." ).arg( options_.nextDocument_ ) ); break;
            case NewDocumentKind::Epub:
                note_->setText( options_.viewer_
                    ? tr( "HyperEditor 새 창에서 제목 · 저자 · 저장 위치를 정해 만들고 편집합니다. HyperViewer는 EPUB을 편집하지 않습니다." )
                    : tr( "제목 · 저자 · 저장 위치를 정해 파일을 만든 뒤 EPUB 편집 모드로 엽니다." ) ); break;
            case NewDocumentKind::Image:
                note_->setText( tr( "크기와 배경을 고른 뒤 ‘새 이미지 %1’을 이미지 편집 모드로 엽니다." ).arg( options_.nextImage_ ) ); break;
        }
    }

    void NewDocumentChooser::choose( int Index )
    {
        if( chosen_ || Index < 0 || Index >= kinds_.size() ) return;
        chosen_ = true;
        const auto Kind = NewDocumentKind( Index );
        // Closing first returns focus; the new tab or the next dialog then takes it.
        QDialog::accept();
        emit Chosen( Kind );
    }

    void NewDocumentChooser::accept() { choose( checkedKind() ); }
    void NewDocumentChooser::ActivateCurrent() { choose( checkedKind() ); }

    bool NewDocumentChooser::eventFilter( QObject* Object, QEvent* Event )
    {
        if( Event->type() == QEvent::MouseButtonDblClick ) {
            const auto Index = kinds_.indexOf( qobject_cast< fm::ui::ChoiceCard* >( Object ) );
            if( Index >= 0 ) { kinds_[ Index ]->setChecked( true ); choose( Index ); return true; }
        }
        if( Event->type() == QEvent::KeyPress ) {
            auto* Key = static_cast< QKeyEvent* >( Event );
            // Arrows move the selection only: Qt's own arrow handling on radio buttons also clicks.
            const bool Down = Key->key() == Qt::Key_Down || Key->key() == Qt::Key_Right, Up = Key->key() == Qt::Key_Up || Key->key() == Qt::Key_Left;
            if( ( Down || Up ) && !( Key->modifiers() & ~Qt::KeypadModifier ) ) {
                const int Next = ( checkedKind() + ( Down ? 1 : -1 ) + int( kinds_.size() ) ) % int( kinds_.size() );
                kinds_[ Next ]->setChecked( true ); kinds_[ Next ]->setFocus( Qt::TabFocusReason );
                return true;
            }
        }
        return QDialog::eventFilter( Object, Event );
    }

    void NewDocumentChooser::keyPressEvent( QKeyEvent* Event )
    {
        if( Repeats( options_.repeatKey_, Event ) ) { if( !Event->isAutoRepeat() ) ActivateCurrent(); Event->accept(); return; }
        if( !( Event->modifiers() & ~Qt::KeypadModifier ) && Event->key() >= Qt::Key_1 && Event->key() <= Qt::Key_4 ) {
            if( !Event->isAutoRepeat() ) { kinds_[ Event->key() - Qt::Key_1 ]->setChecked( true ); choose( Event->key() - Qt::Key_1 ); }
            Event->accept(); return;
        }
        QDialog::keyPressEvent( Event );
    }

    // ── 새 이미지 ───────────────────────────────────────────────────────

    NewImageDialog::NewImageDialog( QWidget* Parent, const Options& Value )
        : QDialog( Parent ), options_( Value )
    {
        setObjectName( "newImageDialog" );
        setWindowTitle( tr( "새 이미지" ) );
        setAttribute( Qt::WA_DeleteOnClose );
        auto* Form = new QFormLayout( this );
        Form->setFieldGrowthPolicy( QFormLayout::AllNonFixedFieldsGrow );
        size_ = new QComboBox( this ); size_->setObjectName( "newImage.size" );
        if( options_.clipboardSize_.isValid() )
            size_->addItem( tr( "클립보드 이미지 · %1 × %2" ).arg( options_.clipboardSize_.width() ).arg( options_.clipboardSize_.height() ),
                QVariantList{ "clipboard", options_.clipboardSize_.width(), options_.clipboardSize_.height() } );
        size_->addItem( tr( "1920 × 1080 · FHD" ), QVariantList{ "fhd", 1920, 1080 } );
        size_->addItem( tr( "1280 × 720 · HD" ), QVariantList{ "hd", 1280, 720 } );
        size_->addItem( tr( "1080 × 1080 · 정사각형" ), QVariantList{ "square", 1080, 1080 } );
        size_->addItem( tr( "2480 × 3508 · A4 300 dpi" ), QVariantList{ "a4", 2480, 3508 } );
        size_->addItem( tr( "사용자 지정" ), QVariantList{ "custom", 0, 0 } );
        Form->addRow( tr( "크기(&S)" ), size_ );
        auto* SizeRow = new QHBoxLayout; SizeRow->setSpacing( 6 );
        width_ = new QSpinBox( this ); width_->setObjectName( "newImageWidth" ); width_->setRange( 1, 99999 ); width_->setAccessibleName( tr( "가로(px)" ) );
        height_ = new QSpinBox( this ); height_->setObjectName( "newImageHeight" ); height_->setRange( 1, 99999 ); height_->setAccessibleName( tr( "세로(px)" ) );
        width_->setValue( options_.lastImage_.width_ ); height_->setValue( options_.lastImage_.height_ );
        SizeRow->addWidget( width_, 1 ); SizeRow->addWidget( new QLabel( QStringLiteral( "×" ), this ) ); SizeRow->addWidget( height_, 1 ); SizeRow->addWidget( new QLabel( QStringLiteral( "px" ), this ) );
        auto* SizeLabel = new QLabel( tr( "가로 × 세로(&W)" ), this ); SizeLabel->setBuddy( width_ );
        Form->addRow( SizeLabel, SizeRow );
        auto* BackgroundRow = new QHBoxLayout; BackgroundRow->setSpacing( 16 );
        auto* Group = new QButtonGroup( this );
        const QList< QPair< QString, QString > > Choices{ { "white", tr( "흰색" ) }, { "clear", tr( "투명" ) }, { "black", tr( "검정" ) } };
        for( const auto& Choice : Choices ) {
            auto* Button = new QRadioButton( Choice.second, this ); Button->setObjectName( "newImageBackground." + Choice.first ); Button->setProperty( "background", Choice.first );
            Button->setChecked( Choice.first == options_.lastImage_.background_ );
            connect( Button, &QRadioButton::toggled, this, [ this ]( bool On ) { if( On ) refresh(); } );
            Group->addButton( Button ); backgrounds_.append( Button ); BackgroundRow->addWidget( Button );
        }
        if( std::none_of( backgrounds_.begin(), backgrounds_.end(), []( auto* Button ) { return Button->isChecked(); } ) ) backgrounds_.first()->setChecked( true );
        BackgroundRow->addStretch();
        auto* BackgroundLabel = new QLabel( tr( "배경(&B)" ), this ); BackgroundLabel->setBuddy( backgrounds_.first() );
        Form->addRow( BackgroundLabel, BackgroundRow );
        message_ = Message( this, "newImage.message" );
        Form->addRow( message_ );
        auto* Buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
        create_ = Buttons->button( QDialogButtonBox::Ok ); create_->setObjectName( "newImage.create" ); create_->setText( tr( "만들기(&O)" ) );
        Buttons->button( QDialogButtonBox::Cancel )->setText( tr( "취소(&C)" ) );
        Form->addRow( Buttons );
        connect( Buttons, &QDialogButtonBox::accepted, this, &NewImageDialog::accept );
        connect( Buttons, &QDialogButtonBox::rejected, this, &NewImageDialog::reject );
        hc::ui::ApplyDialogFrame( this, Buttons, { {}, tr( "크기와 배경을 고르면 빈 캔버스를 이미지 편집 모드로 엽니다. 형식은 저장할 때 고릅니다." ), fm::ui::glyph::NewFile } );
        // The last size and background come back; the clipboard item is never remembered.
        int Initial = 0;
        for( int I = 0; I < size_->count(); ++I ) if( size_->itemData( I ).toList().value( 0 ).toString() == options_.lastImage_.preset_ ) Initial = I;
        if( size_->itemData( Initial ).toList().value( 0 ).toString() == "clipboard" ) Initial = size_->findText( tr( "1920 × 1080 · FHD" ) );
        size_->setCurrentIndex( qMax( 0, Initial ) );
        connect( size_, &QComboBox::currentIndexChanged, this, [ this ] {
            const auto Data = size_->currentData().toList();
            if( Data.value( 0 ).toString() != "custom" ) { const QSignalBlocker A( width_ ), B( height_ ); width_->setValue( Data.value( 1 ).toInt() ); height_->setValue( Data.value( 2 ).toInt() ); }
            refresh();
        } );
        for( auto* Box : { width_, height_ } ) connect( Box, &QSpinBox::valueChanged, this, &NewImageDialog::refresh );
        if( const auto Data = size_->currentData().toList(); Data.value( 0 ).toString() != "custom" ) { width_->setValue( Data.value( 1 ).toInt() ); height_->setValue( Data.value( 2 ).toInt() ); }
        refresh();
        resize( 520, sizeHint().height() );
        size_->setFocus( Qt::OtherFocusReason );
    }

    NewImageSpec NewImageDialog::Spec() const
    {
        NewImageSpec Value;
        const auto Data = size_->currentData().toList();
        Value.preset_ = Data.value( 0 ).toString();
        Value.clipboard_ = Value.preset_ == "clipboard";
        Value.width_ = width_->value(); Value.height_ = height_->value();
        Value.dotsPerInch_ = Value.preset_ == "a4" ? 300 : 96;
        for( auto* Button : backgrounds_ ) if( Button->isChecked() ) Value.background_ = Button->property( "background" ).toString();
        return Value;
    }

    void NewImageDialog::refresh()
    {
        if( !message_ || !create_ ) return;
        const bool Custom = size_->currentData().toList().value( 0 ).toString() == "custom";
        width_->setEnabled( Custom ); height_->setEnabled( Custom );
        const auto Value = Spec();
        const auto Error = NewImageError( Value.width_, Value.height_ );
        create_->setEnabled( Error.isEmpty() );
        Tint( message_, Error.isEmpty() ? Token::Fg2 : Token::Danger );
        if( !Error.isEmpty() ) { message_->setText( Error ); return; }
        if( Value.clipboard_ ) {
            message_->setText( tr( "클립보드 이미지(%1 × %2)로 ‘새 이미지 %3’을 엽니다. 배경은 이미지의 투명한 부분에만 쓰입니다." )
                .arg( Value.width_ ).arg( Value.height_ ).arg( options_.nextImage_ ) );
            return;
        }
        const auto Background = Value.background_ == "clear" ? tr( "투명" ) : Value.background_ == "black" ? tr( "검정" ) : tr( "흰색" );
        message_->setText( tr( "‘새 이미지 %1’ · %2 × %3 · %4 배경으로 편집 모드에서 엽니다." )
            .arg( options_.nextImage_ ).arg( Value.width_ ).arg( Value.height_ ).arg( Background ) );
    }

    void NewImageDialog::accept()
    {
        if( chosen_ || !create_->isEnabled() ) return;
        const auto Value = Spec();
        if( !NewImageError( Value.width_, Value.height_ ).isEmpty() ) return;
        chosen_ = true;
        QDialog::accept();
        emit ImageChosen( Value );
    }

    void NewImageDialog::keyPressEvent( QKeyEvent* Event )
    {
        if( Repeats( options_.repeatKey_, Event ) ) { if( !Event->isAutoRepeat() ) accept(); Event->accept(); return; }
        QDialog::keyPressEvent( Event );
    }
}
