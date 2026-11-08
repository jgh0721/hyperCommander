#include "stdafx.h"
#include "newDocumentChooser.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StylePaint.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/DialogWidgets.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QButtonGroup>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        using fm::style::Token;
        constexpr int     Margin = 10;      // 그림자 자리
        constexpr int     Padding = 10;
        constexpr qreal   Radius = 8;
        constexpr qint64  PixelLimit = 128LL * 1024 * 1024;
        const QStringList KindIds{ "text", "markdown", "epub", "image" };

        QLabel* Caption( const QString& Text, QWidget* Parent, bool Strong = false )
        {
            auto* Label = new QLabel( Text, Parent );
            Label->setTextFormat( Qt::PlainText );
            if( Strong ) { auto Font = Label->font(); Font.setWeight( QFont::DemiBold ); Label->setFont( Font ); }
            return Label;
        }
        QLabel* Note( QWidget* Parent )
        {
            auto* Label = new QLabel( Parent );
            Label->setTextFormat( Qt::PlainText );
            Label->setWordWrap( true );
            Label->setMinimumHeight( Label->fontMetrics().lineSpacing() * 2 + 2 );
            Label->setAlignment( Qt::AlignLeft | Qt::AlignTop );
            return Label;
        }
        void Tint( QLabel* Label, Token Which )
        {
            auto Palette = Label->palette();
            Palette.setColor( QPalette::WindowText, fm::style::themeColorsFor( Label )[ Which ] );
            Label->setPalette( Palette );
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

    NewDocumentChooser* NewDocumentChooser::Open( QWidget* Parent, const Options& Value )
    {
        auto* Chooser = new NewDocumentChooser( Parent, Value );
        Chooser->place();
        Chooser->show();
        Chooser->activateWindow();
        if( Value.imagePage_ ) Chooser->showPage( 1 );
        else if( auto* Card = Chooser->kinds_.value( int( Value.initial_ ) ) ) Card->setFocus( Qt::PopupFocusReason );
        return Chooser;
    }

    NewDocumentChooser::NewDocumentChooser( QWidget* Parent, const Options& Value )
        : QFrame( Parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint )
        , options_( Value )
    {
        setObjectName( "newDocumentChooser" );
        setAttribute( Qt::WA_DeleteOnClose );
        setAttribute( Qt::WA_TranslucentBackground );
        setFrameShape( QFrame::NoFrame );
        setAccessibleName( tr( "새 문서" ) );
        setWindowTitle( tr( "새 문서" ) );
        auto* Layout = new QVBoxLayout( this );
        Layout->setContentsMargins( Margin + Padding, Margin + Padding, Margin + Padding, Margin + Padding );
        pages_ = new QStackedWidget( this );
        pages_->addWidget( buildKinds() );
        pages_->addWidget( buildImage() );
        Layout->addWidget( pages_ );
        setFixedWidth( PanelWidth + 2 * Margin );
        refreshKindNote(); refreshImageNote();
    }

    QWidget* NewDocumentChooser::buildKinds()
    {
        auto* Page = new QWidget( this ); Page->setObjectName( "newDocumentKinds" );
        auto* Layout = new QVBoxLayout( Page ); Layout->setContentsMargins( 0, 0, 0, 0 ); Layout->setSpacing( 6 );
        auto* Head = new QHBoxLayout; Head->setContentsMargins( 2, 0, 2, 2 );
        Head->addWidget( Caption( tr( "새 문서" ), Page, true ) ); Head->addStretch();
        if( !options_.repeatKey_.isEmpty() ) { auto* Key = Caption( options_.repeatKey_.toString( QKeySequence::NativeText ), Page ); Tint( Key, Token::Fg3 ); Head->addWidget( Key ); }
        Layout->addLayout( Head );
        struct Row { QString name_, detail_; fm::ui::ChoiceCard::FileKind kind_; };
        const QList< Row > Rows{
            { tr( "텍스트 문서" ), QStringLiteral( ".txt" ), fm::ui::ChoiceCard::DocKind },
            { tr( "Markdown 문서" ), QStringLiteral( ".md" ), fm::ui::ChoiceCard::CodeKind },
            { tr( "EPUB 책…" ), QStringLiteral( ".epub" ), fm::ui::ChoiceCard::ArchiveKind },
            { tr( "이미지…" ), QStringLiteral( ".png" ), fm::ui::ChoiceCard::ImageKind } };
        auto* Group = new QButtonGroup( Page ); Group->setExclusive( true );
        for( int I = 0; I < Rows.size(); ++I ) {
            auto* Card = new fm::ui::ChoiceCard( Rows[ I ].name_, Page );
            Card->setObjectName( "newDocument." + KindIds[ I ] );
            Card->setDetail( Rows[ I ].detail_ ); Card->setKeyHint( QString::number( I + 1 ) ); Card->setFileKind( Rows[ I ].kind_ );
            Card->setAccessibleName( Rows[ I ].name_ );
            Group->addButton( Card, I );
            Card->installEventFilter( this );
            connect( Card, &QAbstractButton::toggled, this, [ this ]( bool On ) { if( On ) refreshKindNote(); } );
            // A click creates at once; arrows only move the selection.
            connect( Card, &QAbstractButton::clicked, this, [ this, I ] { choose( I ); } );
            kinds_.append( Card ); Layout->addWidget( Card );
        }
        if( auto* Initial = kinds_.value( int( options_.initial_ ) ) ) { const QSignalBlocker Block( Initial ); Initial->setChecked( true ); }
        kindNote_ = Note( Page ); kindNote_->setObjectName( "newDocumentNote" ); Tint( kindNote_, Token::Fg2 );
        Layout->addWidget( kindNote_ );
        auto* Foot = Caption( tr( "↑↓ 선택 · Enter 만들기 · 1 – 4 바로 만들기 · Esc 닫기" ), Page ); Tint( Foot, Token::Fg3 );
        if( auto FootFont = Foot->font(); FootFont.pointSizeF() > 0 ) { FootFont.setPointSizeF( FootFont.pointSizeF() * 0.9 ); Foot->setFont( FootFont ); }
        Layout->addWidget( Foot );
        return Page;
    }

    QWidget* NewDocumentChooser::buildImage()
    {
        auto* Page = new QWidget( this ); Page->setObjectName( "newDocumentImage" );
        auto* Layout = new QVBoxLayout( Page ); Layout->setContentsMargins( 0, 0, 0, 0 ); Layout->setSpacing( 6 );
        auto* Head = new QHBoxLayout; Head->setContentsMargins( 0, 0, 2, 2 ); Head->setSpacing( 4 );
        auto* Back = new QToolButton( Page ); Back->setObjectName( "newImageBack" ); Back->setText( QStringLiteral( "←" ) ); Back->setAutoRaise( true );
        Back->setToolTip( tr( "종류 선택으로 돌아가기 (Esc)" ) ); Back->setAccessibleName( tr( "종류 선택으로 돌아가기" ) );
        connect( Back, &QToolButton::clicked, this, [ this ] { showPage( 0 ); } );
        Head->addWidget( Back ); Head->addWidget( Caption( tr( "새 이미지" ), Page, true ) ); Head->addStretch();
        Layout->addLayout( Head );
        Layout->addWidget( Caption( tr( "크기" ), Page ) );
        struct Preset { QString id_, name_, detail_; QSize size_; };
        QList< Preset > Rows;
        if( options_.clipboardSize_.isValid() )
            Rows.append( { "clipboard", tr( "클립보드 이미지" ), QStringLiteral( "%1 × %2" ).arg( options_.clipboardSize_.width() ).arg( options_.clipboardSize_.height() ), options_.clipboardSize_ } );
        Rows.append( { "fhd", QStringLiteral( "1920 × 1080" ), QStringLiteral( "FHD" ), { 1920, 1080 } } );
        Rows.append( { "hd", QStringLiteral( "1280 × 720" ), QStringLiteral( "HD" ), { 1280, 720 } } );
        Rows.append( { "square", QStringLiteral( "1080 × 1080" ), tr( "정사각형" ), { 1080, 1080 } } );
        Rows.append( { "a4", QStringLiteral( "2480 × 3508" ), tr( "A4 · 300 dpi" ), { 2480, 3508 } } );
        Rows.append( { "custom", tr( "사용자 지정" ), {}, {} } );
        auto* Group = new QButtonGroup( Page ); Group->setExclusive( true );
        for( int I = 0; I < Rows.size(); ++I ) {
            auto* Card = new fm::ui::ChoiceCard( Rows[ I ].name_, Page );
            Card->setObjectName( "newImage." + Rows[ I ].id_ ); Card->setDetail( Rows[ I ].detail_ ); Card->setFileKind( fm::ui::ChoiceCard::ImageKind );
            Group->addButton( Card, I ); Card->installEventFilter( this );
            connect( Card, &QAbstractButton::toggled, this, [ this ]( bool On ) {
                if( !On ) return;
                const bool Custom = presetIds_.value( checkedPreset() ) == "custom";
                width_->setEnabled( Custom ); height_->setEnabled( Custom );
                refreshImageNote();
            } );
            presets_.append( Card ); presetSizes_.append( Rows[ I ].size_ ); presetIds_.append( Rows[ I ].id_ );
            Layout->addWidget( Card );
        }
        auto* Size = new QHBoxLayout; Size->setContentsMargins( 28, 0, 0, 0 ); Size->setSpacing( 6 );
        width_ = new QSpinBox( Page ); width_->setObjectName( "newImageWidth" ); width_->setRange( 1, 99999 ); width_->setValue( options_.lastImage_.width_ ); width_->setAccessibleName( tr( "가로(px)" ) );
        height_ = new QSpinBox( Page ); height_->setObjectName( "newImageHeight" ); height_->setRange( 1, 99999 ); height_->setValue( options_.lastImage_.height_ ); height_->setAccessibleName( tr( "세로(px)" ) );
        for( auto* Box : { width_, height_ } ) { Box->installEventFilter( this ); connect( Box, &QSpinBox::valueChanged, this, &NewDocumentChooser::refreshImageNote ); }
        Size->addWidget( width_ ); Size->addWidget( Caption( QStringLiteral( "×" ), Page ) ); Size->addWidget( height_ ); Size->addWidget( Caption( QStringLiteral( "px" ), Page ) ); Size->addStretch();
        Layout->addLayout( Size );
        Layout->addWidget( Caption( tr( "배경" ), Page ) );
        auto* Backgrounds = new QHBoxLayout; Backgrounds->setContentsMargins( 2, 0, 0, 0 ); Backgrounds->setSpacing( 12 );
        auto* BackgroundGroup = new QButtonGroup( Page );
        const QList< QPair< QString, QString > > Choices{ { "white", tr( "흰색" ) }, { "clear", tr( "투명" ) }, { "black", tr( "검정" ) } };
        for( const auto& Choice : Choices ) {
            auto* Button = new QRadioButton( Choice.second, Page ); Button->setObjectName( "newImageBackground." + Choice.first ); Button->setProperty( "background", Choice.first );
            Button->setChecked( Choice.first == options_.lastImage_.background_ ); Button->installEventFilter( this );
            connect( Button, &QRadioButton::toggled, this, [ this ]( bool On ) { if( On ) refreshImageNote(); } );
            BackgroundGroup->addButton( Button ); backgrounds_.append( Button ); Backgrounds->addWidget( Button );
        }
        if( std::none_of( backgrounds_.begin(), backgrounds_.end(), []( auto* Button ) { return Button->isChecked(); } ) ) backgrounds_.first()->setChecked( true );
        Backgrounds->addStretch();
        Layout->addLayout( Backgrounds );
        imageNote_ = Note( Page ); imageNote_->setObjectName( "newImageNote" );
        Layout->addWidget( imageNote_ );
        auto* Foot = new QHBoxLayout; Foot->setContentsMargins( 2, 0, 0, 0 );
        auto* Hint = Caption( tr( "↑↓ 크기 · Enter 만들기 · Esc 뒤로" ), Page ); Tint( Hint, Token::Fg3 );
        create_ = new QPushButton( tr( "만들기" ), Page ); create_->setObjectName( "newImageCreate" ); create_->setDefault( true );
        connect( create_, &QPushButton::clicked, this, &NewDocumentChooser::createImage );
        Foot->addWidget( Hint ); Foot->addStretch(); Foot->addWidget( create_ );
        Layout->addLayout( Foot );
        // The last size and background come back; the clipboard row is never remembered.
        int Initial = presetIds_.indexOf( options_.lastImage_.preset_ );
        if( Initial < 0 || presetIds_.value( Initial ) == "clipboard" ) Initial = presetIds_.indexOf( "fhd" );
        presets_.value( Initial )->setChecked( true );
        const bool Custom = presetIds_.value( Initial ) == "custom";
        width_->setEnabled( Custom ); height_->setEnabled( Custom );
        return Page;
    }

    void NewDocumentChooser::place()
    {
        adjustSize();
        QPoint TopLeft = options_.useAnchor_ ? options_.anchorBelow_ - QPoint( Margin, Margin - 4 )
                                             : options_.topCenter_ - QPoint( width() / 2, Margin );
        if( const auto* Screen = QGuiApplication::screenAt( options_.useAnchor_ ? options_.anchorBelow_ : options_.topCenter_ ) ) {
            const auto Area = Screen->availableGeometry();
            TopLeft.setX( qBound( Area.left(), TopLeft.x(), qMax( Area.left(), Area.right() - width() ) ) );
            TopLeft.setY( qBound( Area.top(), TopLeft.y(), qMax( Area.top(), Area.bottom() - height() ) ) );
        }
        move( TopLeft );
    }

    void NewDocumentChooser::showPage( int Page )
    {
        pages_->setCurrentIndex( Page );
        for( int I = 0; I < pages_->count(); ++I )
            pages_->widget( I )->setSizePolicy( QSizePolicy::Preferred, I == Page ? QSizePolicy::Preferred : QSizePolicy::Ignored );
        pages_->adjustSize(); adjustSize(); place();
        if( Page == 1 ) {
            if( auto* Card = presets_.value( checkedPreset() ) ) Card->setFocus( Qt::PopupFocusReason );
            setAccessibleName( tr( "새 이미지" ) );
        } else {
            if( auto* Card = kinds_.value( checkedKind() ) ) Card->setFocus( Qt::PopupFocusReason );
            setAccessibleName( tr( "새 문서" ) );
        }
        update();
    }

    int NewDocumentChooser::checkedKind() const
    {
        for( int I = 0; I < kinds_.size(); ++I ) if( kinds_[ I ]->isChecked() ) return I;
        return 0;
    }

    int NewDocumentChooser::checkedPreset() const
    {
        for( int I = 0; I < presets_.size(); ++I ) if( presets_[ I ]->isChecked() ) return I;
        return 0;
    }

    NewImageSpec NewDocumentChooser::imageSpec() const
    {
        NewImageSpec Spec;
        const int Index = checkedPreset();
        Spec.preset_ = presetIds_.value( Index );
        Spec.clipboard_ = Spec.preset_ == "clipboard";
        const auto Size = Spec.preset_ == "custom" ? QSize( width_->value(), height_->value() ) : presetSizes_.value( Index );
        Spec.width_ = Size.width(); Spec.height_ = Size.height();
        Spec.dotsPerInch_ = Spec.preset_ == "a4" ? 300 : 96;
        for( auto* Button : backgrounds_ ) if( Button->isChecked() ) Spec.background_ = Button->property( "background" ).toString();
        return Spec;
    }

    void NewDocumentChooser::refreshKindNote()
    {
        if( !kindNote_ ) return;
        switch( NewDocumentKind( checkedKind() ) ) {
            case NewDocumentKind::Text:
                kindNote_->setText( tr( "‘새 문서 %1’ 탭으로 엽니다. 저장할 때 이름과 위치를 정합니다." ).arg( options_.nextDocument_ ) ); break;
            case NewDocumentKind::Markdown:
                kindNote_->setText( options_.viewer_
                    ? tr( "‘새 문서 %1.md’ 탭을 소스 보기로 엽니다." ).arg( options_.nextDocument_ )
                    : tr( "‘새 문서 %1.md’ 탭으로 엽니다. 소스와 미리보기를 나란히 봅니다." ).arg( options_.nextDocument_ ) ); break;
            case NewDocumentKind::Epub:
                kindNote_->setText( options_.viewer_
                    ? tr( "HyperEditor 새 창에서 제목 · 저자 · 저장 위치를 정해 만들고 편집합니다. HyperViewer는 EPUB을 편집하지 않습니다." )
                    : tr( "제목 · 저자 · 저장 위치를 정해 파일을 만든 뒤 EPUB 편집 모드로 엽니다." ) ); break;
            case NewDocumentKind::Image:
                kindNote_->setText( tr( "크기와 배경을 고른 뒤 ‘새 이미지 %1’을 이미지 편집 모드로 엽니다." ).arg( options_.nextImage_ ) ); break;
        }
    }

    void NewDocumentChooser::refreshImageNote()
    {
        if( !imageNote_ || !create_ ) return;
        const auto Spec = imageSpec();
        const auto Error = NewImageError( Spec.width_, Spec.height_ );
        create_->setEnabled( Error.isEmpty() );
        Tint( imageNote_, Error.isEmpty() ? Token::Fg2 : Token::Danger );
        if( !Error.isEmpty() ) { imageNote_->setText( Error ); return; }
        if( Spec.clipboard_ ) {
            imageNote_->setText( tr( "클립보드 이미지(%1 × %2)로 ‘새 이미지 %3’을 엽니다. 배경은 이미지의 투명한 부분에만 쓰입니다." )
                .arg( Spec.width_ ).arg( Spec.height_ ).arg( options_.nextImage_ ) );
            return;
        }
        const auto Background = Spec.background_ == "clear" ? tr( "투명" ) : Spec.background_ == "black" ? tr( "검정" ) : tr( "흰색" );
        imageNote_->setText( tr( "‘새 이미지 %1’ · %2 × %3 · %4 배경으로 편집 모드에서 엽니다. 형식은 저장할 때 고릅니다." )
            .arg( options_.nextImage_ ).arg( Spec.width_ ).arg( Spec.height_ ).arg( Background ) );
    }

    void NewDocumentChooser::choose( int Index )
    {
        if( chosen_ || Index < 0 || Index >= kinds_.size() ) return;
        if( NewDocumentKind( Index ) == NewDocumentKind::Image ) {
            { const QSignalBlocker Block( kinds_[ Index ] ); kinds_[ Index ]->setChecked( true ); }
            refreshKindNote(); showPage( 1 ); return;
        }
        chosen_ = true;
        const auto Kind = NewDocumentKind( Index );
        // Closing first returns focus; the new tab then takes it.
        close();
        emit Chosen( Kind );
    }

    void NewDocumentChooser::createImage()
    {
        if( chosen_ ) return;
        const auto Spec = imageSpec();
        if( !NewImageError( Spec.width_, Spec.height_ ).isEmpty() ) return;
        chosen_ = true;
        close();
        emit ImageChosen( Spec );
    }

    void NewDocumentChooser::ActivateCurrent()
    {
        if( pages_->currentIndex() == 1 ) createImage(); else choose( checkedKind() );
    }

    bool NewDocumentChooser::handleKey( QKeyEvent* Key )
    {
        const bool Image = pages_->currentIndex() == 1;
        const QKeySequence Pressed( Key->keyCombination() );
        if( !options_.repeatKey_.isEmpty() && Pressed.matches( options_.repeatKey_ ) == QKeySequence::ExactMatch ) {
            if( !Key->isAutoRepeat() ) { if( Image ) createImage(); else choose( checkedKind() ); }
            return true;
        }
        if( Key->key() == Qt::Key_Escape ) {
            if( Image && !options_.imagePage_ ) showPage( 0 ); else close();
            return true;
        }
        if( Key->modifiers() & ~( Qt::KeypadModifier | Qt::ShiftModifier ) ) return false;
        if( Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter ) {
            if( !Key->isAutoRepeat() ) { if( Image ) createImage(); else choose( checkedKind() ); }
            return true;
        }
        if( !Image && Key->key() >= Qt::Key_1 && Key->key() <= Qt::Key_4 ) {
            if( !Key->isAutoRepeat() ) choose( Key->key() - Qt::Key_1 );
            return true;
        }
        // Arrows move the selection only. Qt's own arrow handling on radio buttons would click (and so create).
        const bool Down = Key->key() == Qt::Key_Down || Key->key() == Qt::Key_Right, Up = Key->key() == Qt::Key_Up || Key->key() == Qt::Key_Left;
        if( Down || Up ) {
            auto& Cards = Image ? presets_ : kinds_;
            if( Image && ( width_->hasFocus() || height_->hasFocus()
                || std::any_of( backgrounds_.begin(), backgrounds_.end(), []( auto* Button ) { return Button->hasFocus(); } ) ) ) return false;
            const int Current = Image ? checkedPreset() : checkedKind();
            const int Next = ( Current + ( Down ? 1 : -1 ) + int( Cards.size() ) ) % int( Cards.size() );
            Cards[ Next ]->setChecked( true ); Cards[ Next ]->setFocus( Qt::TabFocusReason );
            return true;
        }
        return false;
    }

    void NewDocumentChooser::keyPressEvent( QKeyEvent* Event )
    {
        if( handleKey( Event ) ) { Event->accept(); return; }
        QFrame::keyPressEvent( Event );
    }

    bool NewDocumentChooser::eventFilter( QObject* Object, QEvent* Event )
    {
        if( Event->type() == QEvent::KeyPress && handleKey( static_cast< QKeyEvent* >( Event ) ) ) return true;
        return QFrame::eventFilter( Object, Event );
    }

    void NewDocumentChooser::closeEvent( QCloseEvent* Event )
    {
        QFrame::closeEvent( Event );
        if( focusReturn_ && !chosen_ ) focusReturn_->setFocus( Qt::PopupFocusReason );
    }

    void NewDocumentChooser::paintEvent( QPaintEvent* )
    {
        const auto& Colors = fm::style::themeColorsFor( this );
        QPainter Painter( this );
        Painter.setRenderHint( QPainter::Antialiasing );
        const QRectF Card = QRectF( rect() ).adjusted( Margin + 0.5, Margin + 0.5, -Margin - 0.5, -Margin - 0.5 );
        QPainterPath Shape; Shape.addRoundedRect( Card, Radius, Radius );
        // A soft shadow below, like the status popups and fm::ui::Menu.
        QColor Shadow = Colors[ Token::Shadow ]; const qreal Base = Shadow.alphaF();
        Painter.setPen( Qt::NoPen );
        for( int I = 6; I >= 1; --I ) {
            Shadow.setAlphaF( Base * 0.12 * ( 7 - I ) / 6.0 );
            Painter.setBrush( Shadow );
            Painter.drawRoundedRect( Card.adjusted( -I, -I + 3, I, I + 3 ), Radius + I, Radius + I );
        }
        Painter.setBrush( Colors[ Token::Surface ] );
        Painter.setPen( QPen( Colors[ Token::Line ], 1 ) );
        Painter.drawPath( Shape );
    }
}
