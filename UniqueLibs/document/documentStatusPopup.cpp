#include "stdafx.h"
#include "documentStatusPopup.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/FlowLayout.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractButton>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPushButton>
#include <QScreen>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        Q_LOGGING_CATEGORY( popupLog, "hc.document.statusPopup" )

        using T = fm::style::Token;

        // 시안 .popover: padding 10 / 12, 1 px 테두리, 모서리 8.
        constexpr int       PanelRadius       = 8;
        constexpr int       PaddingX          = 12;
        constexpr int       PaddingY          = 10;
        // 카드가 창 가장자리에서 떨어지는 거리(시안: 1200 px 창에서 1196 - 340).
        constexpr int       WindowInset       = 4;
        constexpr int       MinimumPanelWidth = 220;
        constexpr int       ListMaximumHeight = 170;
        constexpr int       FieldHeight       = 26;
        constexpr int       NoteRole          = Qt::UserRole + 1;
        // fm::ui::Menu 와 같은 그림자 자리 — 팝업과 메뉴가 같은 떠 있는 판으로 보인다.
        constexpr QMargins  ShadowMargins( 8, 4, 8, 12 );

        QString TabName() { return QStringLiteral( "statusPopupTab" ); }
        QString ChipName() { return QStringLiteral( "statusPopupChip" ); }
        QString NoteName() { return QStringLiteral( "statusPopupNote" ); }

        const fm::style::ThemeColors& ColorsOf( const QWidget* Widget ) { return fm::style::themeColorsFor( Widget ); }

        QColor Faded( QColor Color, qreal Alpha )
        {
            Color.setAlphaF( Color.alphaF() * Alpha );
            return Color;
        }

        // 마우스 올림 · 누름 겹침 — FmStyle 의 납작한 단추와 같은 값.
        QColor HoverFill( const QWidget* Widget, bool Down )
        {
            return Faded( ColorsOf( Widget )[ T::Fg ], Down ? 0.12 : 0.07 );
        }

        // 시안의 px 크기. QFont 의 픽셀 크기는 정수뿐이라 포인트로 준다(Qt 6 의 논리 DPI 는 96).
        QFont SizedFont( QFont Font, qreal Px )
        {
            Font.setPointSizeF( Px * 72.0 / 96.0 );
            return Font;
        }

        // 시안 .popover 의 line-height 1.45.
        int LineHeight( const QFont& Font )
        {
            const qreal Px = Font.pointSizeF() > 0 ? Font.pointSizeF() * 96.0 / 72.0 : Font.pixelSize();
            return qMax( QFontMetrics( Font ).height(), qRound( Px * 1.45 ) );
        }

        bool KeyboardFocus( const QWidget* Widget )
        {
            QStyleOption Option;
            Option.initFrom( Widget );
            return ( Option.state & QStyle::State_HasFocus ) && ( Option.state & QStyle::State_KeyboardFocusChange );
        }

        // 2 px 포커스 링(시안 :focus-visible). 위젯 밖에는 그릴 수 없어 안쪽에 그린다.
        void PaintFocus( QPainter& Painter, const QWidget* Widget, qreal Radius )
        {
            Painter.setPen( QPen( ColorsOf( Widget )[ T::Focus ], 2 ) );
            Painter.setBrush( Qt::NoBrush );
            Painter.drawRoundedRect( QRectF( Widget->rect() ).adjusted( 1, 1, -1, -1 ), Radius, Radius );
        }

        // 팔레트로 그리는 위젯에 토큰을 건다. 처음 넣을 때와 테마가 바뀔 때 부른다.
        void ApplyRoles( QWidget* Widget )
        {
            auto Palette = Widget->palette();
            if( qobject_cast< QLineEdit* >( Widget ) )
                // 바탕 · 테두리는 FieldEdit 가 그린다. 스타일이 테 없는 입력 칸에 칠하는 Base 는 비운다.
                Palette.setColor( QPalette::Base, Qt::transparent );
            else if( Widget->objectName() == NoteName() )
                Palette.setColor( QPalette::WindowText, ColorsOf( Widget )[ T::Fg3 ] );
            else
                return;
            Widget->setPalette( Palette );
        }

        bool IsRoving( const QObject* Object )
        {
            return Object->objectName() == TabName() || Object->objectName() == ChipName();
        }

        // 탭 · 칩 묶음은 Tab 키가 하나만 들른다(고른 것, 없으면 첫째). 묶음 안은 방향키로 움직인다.
        void Rove( QWidget* Group, QAbstractButton* Current )
        {
            const auto Buttons = Group->findChildren< QAbstractButton* >( Qt::FindDirectChildrenOnly );
            if( !Current )
                for( auto* Button : Buttons )
                    if( Button->isChecked() ) { Current = Button; break; }
            if( !Current && !Buttons.isEmpty() ) Current = Buttons.first();
            for( auto* Button : Buttons )
                Button->setFocusPolicy( Button == Current ? Qt::StrongFocus : Qt::ClickFocus );
        }

        void StepWithin( QAbstractButton* Button, int Step )
        {
            auto* Group = Button->parentWidget();
            const auto Buttons = Group->findChildren< QAbstractButton* >( Qt::FindDirectChildrenOnly );
            const int Count = int( Buttons.size() ), Index = int( Buttons.indexOf( Button ) );
            if( Count < 2 || Index < 0 ) return;
            auto* Target = Buttons[ ( Index + Step + Count ) % Count ];
            Rove( Group, Target );
            Target->setFocus( Qt::TabFocusReason );
            // 탭은 포커스를 따라 쪽을 바꾼다. 칩은 Enter · Space 로만 고른다(배율처럼 바로 적용되는 값이 있다).
            if( Target->objectName() == TabName() ) Target->click();
        }

        // 글자나 오른쪽 설명에 검색어가 있는 행만 남긴다. 고른 행이 숨으면 처음 보이는 행으로 옮긴다.
        void FilterList( QListWidget* List, const QString& Needle )
        {
            const auto Text = Needle.trimmed();
            int First = -1;
            for( int Row = 0; Row < List->count(); ++Row )
            {
                const auto* Item  = List->item( Row );
                const bool  Match = Text.isEmpty() || Item->text().contains( Text, Qt::CaseInsensitive )
                                    || Item->data( NoteRole ).toString().contains( Text, Qt::CaseInsensitive );
                List->setRowHidden( Row, !Match );
                if( Match && First < 0 ) First = Row;
            }
            const int Current = List->currentRow();
            if( Current < 0 || List->isRowHidden( Current ) ) List->setCurrentRow( First );
            if( List->currentItem() ) List->scrollToItem( List->currentItem() );
        }

        void StepList( QListWidget* List, int Step )
        {
            for( int Row = List->currentRow() + Step; Row >= 0 && Row < List->count(); Row += Step )
                if( !List->isRowHidden( Row ) )
                {
                    List->setCurrentRow( Row );
                    List->scrollToItem( List->item( Row ) );
                    return;
                }
        }

        // 탭(.ptabs span)과 칩(.chip). 모양만 다르고 배타 선택 · 키 이동은 팝업이 맡는다.
        class SegmentButton final : public QAbstractButton
        {
        public:
            enum class Kind { Tab, Chip };

            SegmentButton( const QString& Text, Kind Shape, QWidget* Parent )
                : QAbstractButton( Parent ), kind_( Shape )
            {
                setText( Text );
                setCheckable( true );
                setFocusPolicy( Qt::StrongFocus );
                setAttribute( Qt::WA_Hover );
                setSizePolicy( Shape == Kind::Tab ? QSizePolicy::Expanding : QSizePolicy::Fixed, QSizePolicy::Fixed );
            }

            QSize sizeHint() const override
            {
                const int Text = fontMetrics().horizontalAdvance( text() );
                // 탭: 위아래 4 px. 칩: 위아래 3 px · 좌우 9 px · 1 px 테두리.
                return kind_ == Kind::Tab ? QSize( Text + 16, LineHeight( font() ) + 8 )
                                          : QSize( Text + 20, LineHeight( font() ) + 8 );
            }
            QSize minimumSizeHint() const override
            {
                return kind_ == Kind::Tab ? QSize( 24, sizeHint().height() ) : sizeHint();
            }

        protected:
            void paintEvent( QPaintEvent* ) override
            {
                const auto& Colors = ColorsOf( this );
                const bool  Down   = isDown(), Hover = isEnabled() && underMouse();
                QPainter    Painter( this );
                Painter.setRenderHint( QPainter::Antialiasing );
                qreal Radius = 3;
                if( kind_ == Kind::Tab )
                {
                    // 바탕은 띠의 둥근 안쪽으로 자른다. 첫 탭이 아니면 왼쪽 1 px 은 구분선이다.
                    const bool   First = x() <= 1;
                    QPainterPath Clip;
                    Clip.addRoundedRect( QRectF( -x(), -y(), parentWidget()->width(), parentWidget()->height() ).adjusted( 1, 1, -1, -1 ), 4, 4 );
                    Painter.setClipPath( Clip );
                    const QRect Fill = rect().adjusted( First ? 0 : 1, 0, 0, 0 );
                    if( isChecked() ) Painter.fillRect( Fill, Colors[ T::AccentSoft ] );
                    else if( Down || Hover ) Painter.fillRect( Fill, HoverFill( this, Down ) );
                    if( !First ) Painter.fillRect( QRect( 0, 0, 1, height() ), Colors[ T::Line ] );
                    Painter.setClipping( false );
                }
                else
                {
                    const QRectF Box = QRectF( rect() ).adjusted( 0.5, 0.5, -0.5, -0.5 );
                    Radius = qMin< qreal >( 12, Box.height() / 2 );
                    Painter.setPen( QPen( isChecked() ? Colors[ T::Accent ] : Colors[ T::Line ], 1 ) );
                    if( isChecked() ) Painter.setBrush( Colors[ T::AccentSoft ] );
                    else if( Down || Hover ) Painter.setBrush( HoverFill( this, Down ) );
                    else Painter.setBrush( Qt::NoBrush );
                    Painter.drawRoundedRect( Box, Radius, Radius );
                }
                if( KeyboardFocus( this ) ) PaintFocus( Painter, this, Radius );
                Painter.setPen( isEnabled() ? Colors[ T::Fg ] : Colors[ T::Fg3 ] );
                Painter.drawText( rect(), Qt::AlignCenter, fontMetrics().elidedText( text(), Qt::ElideRight, width() - 8 ) );
            }

        private:
            Kind kind_;
        };

        // .ptabs — 1 px 테두리 · 모서리 5 의 띠. 탭은 1 px 안쪽에 놓이고 탭마다 쪽 하나가 딸린다.
        class TabStrip final : public QWidget
        {
        public:
            using QWidget::QWidget;
            QList< QWidget* > pages_;

        protected:
            void paintEvent( QPaintEvent* ) override
            {
                QPainter Painter( this );
                Painter.setRenderHint( QPainter::Antialiasing );
                Painter.setPen( QPen( ColorsOf( this )[ T::Line ], 1 ) );
                Painter.setBrush( Qt::NoBrush );
                Painter.drawRoundedRect( QRectF( rect() ).adjusted( 0.5, 0.5, -0.5, -0.5 ), 5, 5 );
            }
        };

        // .act — 왼쪽 정렬 글자, 오른쪽 흐린 단축키, 1 px 테두리 · 모서리 4, 위아래 5 px · 좌우 8 px.
        class ActionButton final : public QPushButton
        {
        public:
            ActionButton( const QString& Text, const QString& Shortcut, const QFont& NoteFont )
                : QPushButton( Text ), shortcut_( Shortcut ), noteFont_( NoteFont )
            {
                setAutoDefault( false );
                setFlat( true );
                setAttribute( Qt::WA_Hover );
                setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
            }

            QSize sizeHint() const override
            {
                const int Shortcut = shortcut_.isEmpty() ? 0 : QFontMetrics( noteFont_ ).horizontalAdvance( shortcut_ ) + 16;
                return { fontMetrics().horizontalAdvance( text() ) + Shortcut + 18, LineHeight( font() ) + 12 };
            }
            QSize minimumSizeHint() const override { return { 0, sizeHint().height() }; }

        protected:
            void paintEvent( QPaintEvent* ) override
            {
                const auto& Colors = ColorsOf( this );
                QPainter    Painter( this );
                Painter.setRenderHint( QPainter::Antialiasing );
                Painter.setPen( QPen( Colors[ T::Line ], 1 ) );
                if( isEnabled() && ( isDown() || underMouse() ) ) Painter.setBrush( HoverFill( this, isDown() ) );
                else Painter.setBrush( Qt::NoBrush );
                Painter.drawRoundedRect( QRectF( rect() ).adjusted( 0.5, 0.5, -0.5, -0.5 ), 4, 4 );
                if( KeyboardFocus( this ) ) PaintFocus( Painter, this, 3 );
                const QRect Inner = rect().adjusted( 8, 0, -8, 0 );
                int         Room  = Inner.width();
                if( !shortcut_.isEmpty() )
                {
                    Painter.setFont( noteFont_ );
                    Painter.setPen( Colors[ T::Fg3 ] );
                    Painter.drawText( Inner, Qt::AlignRight | Qt::AlignVCenter, shortcut_ );
                    Room -= QFontMetrics( noteFont_ ).horizontalAdvance( shortcut_ ) + 12;
                }
                Painter.setFont( font() );
                Painter.setPen( isEnabled() ? Colors[ T::Fg ] : Colors[ T::Fg3 ] );
                Painter.drawText( Inner, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText( text(), Qt::ElideRight, qMax( 0, Room ) ) );
            }

        private:
            QString shortcut_;
            QFont   noteFont_;
        };

        // .search — 26 px · 입력 바탕 · 1 px 테두리 · 모서리 4. 검색 칸은 왼쪽에 돋보기, 입력 칸은 안 오른쪽에 흐린 안내.
        class FieldEdit final : public QLineEdit
        {
        public:
            FieldEdit( bool Search, const QString& Hint, const QFont& NoteFont )
                : search_( Search ), hint_( Hint ), noteFont_( NoteFont )
            {
                setFrame( false );
                setFixedHeight( FieldHeight );
                setAttribute( Qt::WA_Hover );
                const int Right = hint_.isEmpty() ? 8 : 8 + QFontMetrics( noteFont_ ).horizontalAdvance( hint_ ) + 8;
                // QLineEdit 은 글자 양옆에 2 px 을 더 둔다. 시안: 좌우 8 px, 돋보기 14 px + 간격 6 px.
                setTextMargins( ( search_ ? 8 + 14 + 6 : 8 ) - 2, 0, Right - 2, 0 );
            }

            void SetInvalid( bool Invalid )
            {
                if( invalid_ == Invalid ) return;
                invalid_ = Invalid;
                update();
            }

        protected:
            void paintEvent( QPaintEvent* Event ) override
            {
                const auto& Colors = ColorsOf( this );
                {
                    QPainter Painter( this );
                    Painter.setRenderHint( QPainter::Antialiasing );
                    const QColor Border = invalid_ ? Colors[ T::Danger ] : hasFocus() ? Colors[ T::Accent ] : Colors[ T::Line ];
                    Painter.setPen( QPen( Border, 1 ) );
                    Painter.setBrush( Colors[ T::Field ] );
                    Painter.drawRoundedRect( QRectF( rect() ).adjusted( 0.5, 0.5, -0.5, -0.5 ), 4, 4 );
                    if( search_ )
                    {
                        const QPointF Origin( 8, ( height() - 14 ) / 2.0 );
                        Painter.setPen( QPen( Colors[ T::Fg3 ], 1.5, Qt::SolidLine, Qt::RoundCap ) );
                        Painter.setBrush( Qt::NoBrush );
                        Painter.drawEllipse( Origin + QPointF( 6, 6 ), 4.5, 4.5 );
                        Painter.drawLine( Origin + QPointF( 9.4, 9.4 ), Origin + QPointF( 13, 13 ) );
                    }
                }
                QLineEdit::paintEvent( Event );
                if( !hint_.isEmpty() )
                {
                    QPainter Painter( this );
                    Painter.setFont( noteFont_ );
                    Painter.setPen( Colors[ T::Fg3 ] );
                    Painter.drawText( rect().adjusted( 8, 0, -8, 0 ), Qt::AlignRight | Qt::AlignVCenter, hint_ );
                }
            }
            void focusInEvent( QFocusEvent* Event ) override { QLineEdit::focusInEvent( Event ); update(); }
            void focusOutEvent( QFocusEvent* Event ) override { QLineEdit::focusOutEvent( Event ); update(); }

        private:
            bool    search_  = false;
            bool    invalid_ = false;
            QString hint_;
            QFont   noteFont_;
        };

        // .warn — 왼쪽 3 px 경고색 막대, 경고색 10 % 바탕, 위아래 6 px · 좌우 8 px.
        class WarningLabel final : public QLabel
        {
        public:
            using QLabel::QLabel;

        protected:
            void paintEvent( QPaintEvent* Event ) override
            {
                {
                    const QColor Warn = ColorsOf( this )[ T::Warn ];
                    QPainter     Painter( this );
                    Painter.fillRect( rect(), Faded( Warn, 0.10 ) );
                    Painter.fillRect( QRect( 0, 0, 3, height() ), Warn );
                }
                QLabel::paintEvent( Event );
            }
        };

        // .list div — 위아래 4 px · 좌우 8 px, 오른쪽에 흐린 설명. 고른 행은 연한 강조(시안 --sel), 마우스 올림은 겹침.
        class ChoiceDelegate final : public QStyledItemDelegate
        {
        public:
            ChoiceDelegate( const QFont& NoteFont, QObject* Parent ) : QStyledItemDelegate( Parent ), noteFont_( NoteFont ) {}

            void paint( QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index ) const override
            {
                const auto& Colors = ColorsOf( Option.widget );
                Painter->save();
                if( Option.state & QStyle::State_Selected ) Painter->fillRect( Option.rect, Colors[ T::AccentSoft ] );
                else if( Option.state & QStyle::State_MouseOver ) Painter->fillRect( Option.rect, Faded( Colors[ T::Fg ], 0.07 ) );
                const QRect   Box  = Option.rect.adjusted( 8, 0, -8, 0 );
                const QString Note = Index.data( NoteRole ).toString();
                int           Room = Box.width();
                if( !Note.isEmpty() )
                {
                    Painter->setFont( noteFont_ );
                    Painter->setPen( Colors[ T::Fg3 ] );
                    Painter->drawText( Box, Qt::AlignRight | Qt::AlignVCenter, Note );
                    Room -= QFontMetrics( noteFont_ ).horizontalAdvance( Note ) + 12;
                }
                Painter->setFont( Option.font );
                Painter->setPen( Colors[ T::Fg ] );
                const auto Text = Option.fontMetrics.elidedText( Index.data( Qt::DisplayRole ).toString(), Qt::ElideRight, qMax( 0, Room ) );
                Painter->drawText( Box, Qt::AlignLeft | Qt::AlignVCenter, Text );
                Painter->restore();
            }
            QSize sizeHint( const QStyleOptionViewItem& Option, const QModelIndex& ) const override
            {
                return { Option.rect.width(), LineHeight( Option.font ) + 8 };
            }

        private:
            QFont noteFont_;
        };

        // .list — 최대 170 px, 입력 바탕, 1 px 테두리 · 모서리 4. 높이는 모든 행으로 정해 거르는 동안 카드 크기가 그대로다.
        class ChoiceList final : public QListWidget
        {
        public:
            ChoiceList()
            {
                // 1 px 평면 틀은 보기와 스크롤 막대를 테두리 안쪽에 둔다. 틀 자체는 event() 가 둥글게 그린다.
                // (스크롤 영역은 배치마다 내용 여백을 틀 두께로 되돌리므로 setContentsMargins 로는 남지 않는다.)
                setFrameStyle( QFrame::Box | QFrame::Plain );
                setLineWidth( 1 );
                viewport()->setAutoFillBackground( false );
                viewport()->setAttribute( Qt::WA_Hover );
                setMouseTracking( true );
                setUniformItemSizes( true );
                setSelectionMode( QAbstractItemView::SingleSelection );
                setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
                setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
                setMaximumHeight( ListMaximumHeight );
            }

            QSize sizeHint() const override
            {
                return { 200, qMin( ListMaximumHeight, ( LineHeight( font() ) + 8 ) * int( count() ) + 2 ) };
            }
            QSize minimumSizeHint() const override { return { 0, qMin( sizeHint().height(), LineHeight( font() ) + 10 ) }; }

        protected:
            bool event( QEvent* Event ) override
            {
                // 스크롤 영역 자신의 칠하기(틀). 보기는 투명하게 두어 이 바탕이 비친다.
                if( Event->type() == QEvent::Paint )
                {
                    const auto& Colors = ColorsOf( this );
                    QPainter    Painter( this );
                    Painter.setRenderHint( QPainter::Antialiasing );
                    Painter.setPen( QPen( hasFocus() ? Colors[ T::Focus ] : Colors[ T::Line ], 1 ) );
                    Painter.setBrush( Colors[ T::Field ] );
                    Painter.drawRoundedRect( QRectF( rect() ).adjusted( 0.5, 0.5, -0.5, -0.5 ), 4, 4 );
                    return true;
                }
                return QListWidget::event( Event );
            }
            void focusInEvent( QFocusEvent* Event ) override { QListWidget::focusInEvent( Event ); update(); }
            void focusOutEvent( QFocusEvent* Event ) override { QListWidget::focusOutEvent( Event ); update(); }
        };
    }

    DocumentStatusPopup::DocumentStatusPopup( QWidget* Anchor )
        : QFrame( Anchor, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint )
        , anchor_( Anchor )
    {
        setObjectName( "documentStatusPopup" );
        setAttribute( Qt::WA_DeleteOnClose );
        // 둥근 카드와 그림자를 직접 그린다(창을 만들기 전에 정해야 한다).
        setAttribute( Qt::WA_TranslucentBackground );
        setFrameShape( QFrame::NoFrame );
        // 시안 .popover 는 12.5 px. 글꼴 계열은 상태표시줄을 따른다.
        setFont( SizedFont( Anchor ? Anchor->font() : font(), 12.5 ) );

        auto* Layout = new QVBoxLayout( this );
        Layout->setContentsMargins( ShadowMargins + QMargins( 1 + PaddingX, 1 + PaddingY, 1 + PaddingX, 1 + PaddingY ) );
        Layout->setSpacing( 0 );
        title_ = new QLabel( this );
        title_->setObjectName( "statusPopupTitle" );
        title_->setTextFormat( Qt::PlainText );
        title_->setWordWrap( true );
        auto TitleFont = SizedFont( font(), 13 );
        TitleFont.setWeight( QFont::DemiBold );
        title_->setFont( TitleFont );
        Layout->addWidget( title_ );
        root_ = makePage( 6 );  // h4 아래 6 px
        root_->setObjectName( "statusPopupRoot" );
        Layout->addWidget( root_ );

        connect( &fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, &DocumentStatusPopup::applyTheme );
        applyTheme();
    }

    DocumentStatusPopup::~DocumentStatusPopup() = default;

    DocumentStatusPopup* DocumentStatusPopup::Open( QWidget* Anchor, const QString& Heading )
    {
        auto* Popup = new DocumentStatusPopup( Anchor );
        Popup->SetTitle( Heading );
        return Popup;
    }

    void DocumentStatusPopup::SetTitle( const QString& Heading )
    {
        title_->setText( Heading );
        title_->setVisible( !Heading.isEmpty() );
        setAccessibleName( Heading );
        setWindowTitle( Heading );
    }

    QString DocumentStatusPopup::Title() const
    {
        return title_->text();
    }

    QWidget* DocumentStatusPopup::Root() const
    {
        return root_;
    }

    QWidget* DocumentStatusPopup::AnchorWidget() const
    {
        return anchor_.data();
    }

    QLabel* DocumentStatusPopup::AddNote( QWidget* Page, const QString& Text )
    {
        auto* Note = new QLabel( Text );
        Note->setObjectName( NoteName() );
        Note->setTextFormat( Qt::PlainText );
        Note->setWordWrap( true );
        Note->setFont( SizedFont( font(), 11.5 ) );
        place( Page, Note, 2, 8 );
        return Note;
    }

    QList< QWidget* > DocumentStatusPopup::AddTabs( const QStringList& Labels, int Current )
    {
        if( Labels.isEmpty() ) return {};
        auto* Strip = new TabStrip;
        Strip->setObjectName( "statusPopupTabs" );
        Strip->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
        auto* Row = new QHBoxLayout( Strip );
        Row->setContentsMargins( 1, 1, 1, 1 );
        Row->setSpacing( 0 );
        for( int I = 0; I < Labels.size(); ++I )
        {
            auto* Tab = new SegmentButton( Labels[ I ], SegmentButton::Kind::Tab, Strip );
            Tab->setObjectName( TabName() );
            Row->addWidget( Tab, 1 );
            watch( Tab );
            connect( Tab, &QAbstractButton::clicked, this, [ this, Strip, I ] { selectTab( Strip, I ); } );
            // 쪽의 첫 항목 위 여백은 띠의 아래 여백(8 px)과 겹친다.
            Strip->pages_ << makePage( 8 );
        }
        place( nullptr, Strip, 6, 8 );
        // 쪽은 띠 바로 아래에 사이 없이 붙인다. 고른 쪽만 보이고 나머지는 자리를 차지하지 않는다.
        auto& RootState = pages_[ root_ ];
        for( auto* Page : Strip->pages_ )
        {
            Page->setObjectName( "statusPopupPage" );
            RootState.layout_->insertWidget( RootState.layout_->count() - 2, Page );
        }
        RootState.bottom_ = 0;
        setTail( RootState );
        selectTab( Strip, qBound( 0, Current, int( Labels.size() ) - 1 ) );
        return Strip->pages_;
    }

    void DocumentStatusPopup::AddChips( QWidget* Page, const QStringList& Labels, int Current, std::function< void( int ) > OnActivate )
    {
        auto* Row  = new QWidget;
        Row->setObjectName( "statusPopupChips" );
        auto* Flow = new fm::ui::FlowLayout( 6, 6, Row );
        Flow->setContentsMargins( 0, 0, 0, 0 );
        for( int I = 0; I < Labels.size(); ++I )
        {
            auto* Chip = new SegmentButton( Labels[ I ], SegmentButton::Kind::Chip, Row );
            Chip->setObjectName( ChipName() );
            Chip->setChecked( I == Current );
            Flow->addWidget( Chip );
            watch( Chip );
            connect( Chip, &QAbstractButton::clicked, this, [ Row, I, OnActivate ] {
                const auto Chips = Row->findChildren< QAbstractButton* >( Qt::FindDirectChildrenOnly );
                for( int J = 0; J < Chips.size(); ++J ) Chips[ J ]->setChecked( J == I );
                Rove( Row, Chips.value( I ) );
                if( OnActivate ) OnActivate( I );
            } );
        }
        Rove( Row, nullptr );
        place( Page, Row, 8, 0 );
    }

    QLineEdit* DocumentStatusPopup::AddSearch( QWidget* Page, const QString& Placeholder )
    {
        auto* Edit = new FieldEdit( true, {}, SizedFont( font(), 11.5 ) );
        Edit->setObjectName( "statusPopupSearch" );
        Edit->setPlaceholderText( Placeholder );
        Edit->setAccessibleName( Placeholder );
        watch( Edit );
        place( Page, Edit, 0, 6 );
        return Edit;
    }

    QListWidget* DocumentStatusPopup::AddList( QWidget* Page, const QList< QPair< QString, QString > >& Items, int Current,
                                               std::function< void( int ) > OnActivate, QLineEdit* Filter )
    {
        auto* List = new ChoiceList;
        List->setObjectName( "statusPopupList" );
        List->setItemDelegate( new ChoiceDelegate( SizedFont( font(), 11.5 ), List ) );
        List->setAccessibleName( title_->text() );
        for( const auto& [ Text, Note ] : Items )
        {
            auto* Item = new QListWidgetItem( Text, List );
            Item->setData( NoteRole, Note );
            if( !Note.isEmpty() ) Item->setData( Qt::AccessibleTextRole, Text + QStringLiteral( ", " ) + Note );
        }
        if( Current >= 0 && Current < List->count() ) List->setCurrentRow( Current );
        const auto Activate = [ List, OnActivate ] {
            const int Row = List->currentRow();
            if( Row >= 0 && !List->isRowHidden( Row ) && OnActivate ) OnActivate( Row );
        };
        activators_.insert( List, Activate );
        connect( List, &QListWidget::itemClicked, this, [ List, OnActivate ]( QListWidgetItem* Item ) {
            if( OnActivate ) OnActivate( List->row( Item ) );
        } );
        if( Filter )
        {
            filters_.insert( Filter, List );
            activators_.insert( Filter, Activate );
            connect( Filter, &QLineEdit::textChanged, List, [ List ]( const QString& Text ) { FilterList( List, Text ); } );
        }
        watch( List );
        place( Page, List, 0, 0 );
        return List;
    }

    QPushButton* DocumentStatusPopup::AddAction( QWidget* Page, const QString& Text, const QString& ShortcutText, std::function< void() > OnTrigger )
    {
        auto* Button = new ActionButton( Text, ShortcutText, SizedFont( font(), 11.5 ) );
        Button->setObjectName( "statusPopupAction" );
        Button->setAccessibleDescription( ShortcutText );
        // 명령은 팝업을 닫은 뒤에 부른다. 명령이 연 대화상자나 옮긴 포커스를 팝업 닫힘이 되돌리지 않는다.
        // 지우기는 이벤트 루프로 미뤄지므로(WA_DeleteOnClose) 콜백 안에서도 팝업 위젯을 읽을 수 있다.
        connect( Button, &QPushButton::clicked, this, [ this, OnTrigger ] {
            close();
            if( OnTrigger ) OnTrigger();
        } );
        watch( Button );
        place( Page, Button, 4, 0 );
        return Button;
    }

    QCheckBox* DocumentStatusPopup::AddCheck( QWidget* Page, const QString& Text, bool Checked, std::function< void( bool ) > OnToggle )
    {
        auto* Check = new QCheckBox( Text );
        Check->setObjectName( "statusPopupCheck" );
        Check->setChecked( Checked );
        // 사용자가 바꿀 때만 알린다(호출부의 setChecked 는 알리지 않는다).
        connect( Check, &QCheckBox::clicked, this, [ OnToggle ]( bool On ) {
            if( OnToggle ) OnToggle( On );
        } );
        watch( Check );
        place( Page, Check, 6, 0 );
        return Check;
    }

    QLabel* DocumentStatusPopup::AddWarning( QWidget* Page, const QString& Text )
    {
        auto* Warning = new WarningLabel( Text );
        Warning->setObjectName( "statusPopupWarning" );
        Warning->setTextFormat( Qt::PlainText );
        Warning->setWordWrap( true );
        Warning->setContentsMargins( 3 + 8, 6, 8, 6 );
        Warning->setFont( SizedFont( font(), 11.5 ) );
        place( Page, Warning, 8, 0 );
        return Warning;
    }

    QLineEdit* DocumentStatusPopup::AddInput( QWidget* Page, const QString& Text, const QString& Hint, std::function< bool( const QString& ) > OnAccept )
    {
        auto* Edit = new FieldEdit( false, Hint, SizedFont( font(), 11.5 ) );
        Edit->setObjectName( "statusPopupInput" );
        Edit->setText( Text );
        Edit->setAccessibleName( title_->text().isEmpty() ? Hint : title_->text() );
        Edit->setAccessibleDescription( Hint );
        activators_.insert( Edit, [ this, Edit, OnAccept ] {
            if( !OnAccept || OnAccept( Edit->text() ) )
            {
                close();
                return;
            }
            Edit->SetInvalid( true );
            Edit->selectAll();
        } );
        connect( Edit, &QLineEdit::textEdited, Edit, [ Edit ] { Edit->SetInvalid( false ); } );
        watch( Edit );
        place( Page, Edit, 0, 6 );
        return Edit;
    }

    void DocumentStatusPopup::SetFocusReturn( QWidget* Widget )
    {
        focusReturn_ = Widget;
    }

    void DocumentStatusPopup::ShowAbove()
    {
        fit();
        focusFirst();
        show();
        // 보인 뒤에야 보기 크기가 정해진다.
        for( auto* List : findChildren< QListWidget* >() )
            if( List->currentItem() ) List->scrollToItem( List->currentItem(), QAbstractItemView::PositionAtCenter );
    }

    QRect DocumentStatusPopup::PanelGeometry() const
    {
        const QRect Panel = panelRect();
        return { mapToGlobal( Panel.topLeft() ), Panel.size() };
    }

    void DocumentStatusPopup::paintEvent( QPaintEvent* Event )
    {
        const auto&  Colors = ColorsOf( this );
        QPainter     Painter( this );
        Painter.setRenderHint( QPainter::Antialiasing );
        const QRectF Card = QRectF( panelRect() ).adjusted( 0.5, 0.5, -0.5, -0.5 );
        QPainterPath Shape;
        Shape.addRoundedRect( Card, PanelRadius, PanelRadius );
        // fm::ui::Menu 와 같은 아래로 치우친 부드러운 그림자. 카드 안쪽만 다시 칠할 때(마우스 올림)는 건너뛴다.
        if( !Card.adjusted( PanelRadius, PanelRadius, -PanelRadius, -PanelRadius ).contains( QRectF( Event->rect() ) ) )
        {
            QColor             Shadow = Colors[ T::Shadow ];
            const qreal        Base   = Shadow.alphaF();
            const QPainterPath Lifted = Shape.translated( 0, 3 );
            Painter.setPen( Qt::NoPen );
            for( int I = 8; I >= 1; --I )
            {
                QPainterPathStroker Stroker;
                Stroker.setWidth( I * 2.0 );
                Stroker.setJoinStyle( Qt::RoundJoin );
                Shadow.setAlphaF( Base * 0.10 * ( 9 - I ) / 8.0 );
                Painter.setBrush( Shadow );
                Painter.drawPath( Stroker.createStroke( Lifted ).united( Lifted ) );
            }
        }
        Painter.setBrush( Colors[ T::Surface ] );
        Painter.setPen( QPen( Colors[ T::Line ], 1 ) );
        Painter.drawPath( Shape );
    }

    void DocumentStatusPopup::keyPressEvent( QKeyEvent* Event )
    {
        if( Event->key() == Qt::Key_Escape )
        {
            Event->accept();
            dismiss();
            return;
        }
        QFrame::keyPressEvent( Event );
    }

    void DocumentStatusPopup::mousePressEvent( QMouseEvent* Event )
    {
        if( panelRect().contains( Event->position().toPoint() ) )
        {
            QFrame::mousePressEvent( Event );
            return;
        }
        // 기준 항목을 다시 누르면 닫기만 한다. 누름을 되살리면 기준 항목이 팝업을 곧바로 다시 연다.
        if( anchor_ && QRect( anchor_->mapToGlobal( QPoint( 0, 0 ) ), anchor_->size() ).contains( Event->globalPosition().toPoint() ) )
            setAttribute( Qt::WA_NoMouseReplay );
        Event->accept();
        close();
    }

    bool DocumentStatusPopup::eventFilter( QObject* Object, QEvent* Event )
    {
        if( Event->type() == QEvent::FocusIn )
        {
            if( auto* Button = qobject_cast< QAbstractButton* >( Object ); Button && IsRoving( Button ) )
                Rove( Button->parentWidget(), Button );
            return false;
        }
        if( Event->type() != QEvent::KeyPress ) return QFrame::eventFilter( Object, Event );

        const auto* Key = static_cast< QKeyEvent* >( Event );
        if( Key->key() == Qt::Key_Escape )
        {
            dismiss();
            return true;
        }
        const bool Plain = !( Key->modifiers() & ~( Qt::KeypadModifier | Qt::ShiftModifier ) );
        if( !Plain ) return false;
        const bool Enter = Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter;
        if( const auto It = activators_.constFind( Object ); It != activators_.cend() )
        {
            // 입력 칸의 Space 는 글자다. 목록의 Space 는 Enter 와 같다.
            if( Enter || ( Key->key() == Qt::Key_Space && qobject_cast< QListWidget* >( Object ) ) )
            {
                if( !Key->isAutoRepeat() )
                {
                    const auto Activate = It.value();  // 동작이 팝업을 닫아도 함수 객체는 남는다
                    Activate();
                }
                return true;
            }
        }
        if( auto* Button = qobject_cast< QAbstractButton* >( Object ) )
        {
            if( Enter )
            {
                if( !Key->isAutoRepeat() ) Button->click();
                return true;
            }
            if( IsRoving( Button ) )
            {
                const int Step = Key->key() == Qt::Key_Left || Key->key() == Qt::Key_Up      ? -1
                               : Key->key() == Qt::Key_Right || Key->key() == Qt::Key_Down  ? 1
                                                                                             : 0;
                if( Step )
                {
                    StepWithin( Button, Step );
                    return true;
                }
            }
        }
        if( auto* List = filters_.value( Object ) )
        {
            if( Key->key() == Qt::Key_Down || Key->key() == Qt::Key_Up )
            {
                StepList( List, Key->key() == Qt::Key_Down ? 1 : -1 );
                return true;
            }
        }
        return false;
    }

    QWidget* DocumentStatusPopup::makePage( int Bottom )
    {
        auto* Page   = new QWidget( this );
        auto* Layout = new QVBoxLayout( Page );
        Layout->setContentsMargins( 0, 0, 0, 0 );
        Layout->setSpacing( 0 );
        Layout->addSpacing( 0 );    // 끝 여백: 마지막 항목의 아래 여백(카드 안쪽 여백과는 겹치지 않는다)
        Layout->addStretch( 1 );    // 탭을 바꿔도 내용이 위에 붙는다
        pages_.insert( Page, { Layout, Bottom } );
        return Page;
    }

    QWidget* DocumentStatusPopup::pageOf( QWidget* Page ) const
    {
        if( !Page ) return root_;
        if( pages_.contains( Page ) ) return Page;
        qCWarning( popupLog ) << "status popup: not a page of this popup; using the root page";
        return root_;
    }

    void DocumentStatusPopup::place( QWidget* Page, QWidget* Item, int Top, int Bottom )
    {
        auto&     State  = pages_[ pageOf( Page ) ];
        auto*     Layout = State.layout_;
        const int Tail   = Layout->count() - 2;
        Layout->insertSpacing( Tail, qMax( State.bottom_, Top ) );
        Layout->insertWidget( Tail + 1, Item );
        State.bottom_ = Bottom;
        setTail( State );
        ApplyRoles( Item );
    }

    void DocumentStatusPopup::setTail( PageState& State )
    {
        if( auto* Spacer = State.layout_->itemAt( State.layout_->count() - 2 )->spacerItem() )
            Spacer->changeSize( 0, State.bottom_, QSizePolicy::Minimum, QSizePolicy::Fixed );
        State.layout_->invalidate();
    }

    void DocumentStatusPopup::watch( QWidget* Widget )
    {
        Widget->installEventFilter( this );
    }

    void DocumentStatusPopup::applyTheme()
    {
        // 창은 부모의 팔레트를 물려받지 않는다(내장 뷰어는 테마를 뿌리 위젯에만 건다). 토큰에서 직접 만든다.
        setPalette( ColorsOf( this ).toPalette() );
        const auto Children = findChildren< QWidget* >();
        for( auto* Child : Children ) ApplyRoles( Child );
        for( auto* Child : Children ) Child->update();
        update();
    }

    void DocumentStatusPopup::dismiss()
    {
        const QPointer< QWidget > Target = focusReturn_ ? focusReturn_ : anchor_;
        close();
        if( Target ) Target->setFocus( Qt::PopupFocusReason );
    }

    void DocumentStatusPopup::focusFirst()
    {
        // 입력 칸 → 검색 칸 → 목록 → 처음 들르는 위젯(고른 칩 · 탭은 묶음에서 Tab 이 들르는 하나다).
        const auto Shown = [ this ]( const QWidget* Widget ) { return Widget->isEnabled() && Widget->isVisibleTo( this ); };
        QWidget*   Target = nullptr;
        for( const auto* Name : { "statusPopupInput", "statusPopupSearch", "statusPopupList" } )
        {
            for( auto* Widget : findChildren< QWidget* >( QString::fromLatin1( Name ) ) )
                if( Shown( Widget ) )
                {
                    Target = Widget;
                    break;
                }
            if( Target ) break;
        }
        for( auto* Widget = nextInFocusChain(); !Target && Widget && Widget != this; Widget = Widget->nextInFocusChain() )
            if( isAncestorOf( Widget ) && Shown( Widget ) && ( Widget->focusPolicy() & Qt::TabFocus ) ) Target = Widget;
        if( !Target ) return;
        // 창이 보이기 전이라 포커스 자리만 정해 두고, 팝업이 열릴 때 Qt 가 이 위젯에 포커스를 준다.
        Target->setFocus( Qt::PopupFocusReason );
        if( auto* Edit = qobject_cast< QLineEdit* >( Target ) ) Edit->selectAll();
    }

    void DocumentStatusPopup::fit()
    {
        const QWidget* Anchor = anchor_.data();
        const QWidget* Bar    = Anchor && Anchor->parentWidget() ? Anchor->parentWidget() : Anchor;
        const QWidget* Host   = Anchor ? Anchor->window() : nullptr;
        const QRect    Window = Host ? QRect( Host->mapToGlobal( QPoint( 0, 0 ) ), Host->size() )
                                     : ( screen() ? screen()->availableGeometry() : QRect( 0, 0, PanelWidth, 600 ) );
        const int      Width  = qMin( PanelWidth, qMax( MinimumPanelWidth, Window.width() - 2 * WindowInset ) );
        const int      Outer  = Width + ShadowMargins.left() + ShadowMargins.right();

        ensurePolished();
        auto* Layout = layout();
        Layout->activate();
        const int Height      = Layout->hasHeightForWidth() ? Layout->totalHeightForWidth( Outer ) : Layout->totalSizeHint().height();
        const int PanelHeight = Height - ShadowMargins.top() - ShadowMargins.bottom();

        int Left = Window.left() + WindowInset, Top = Window.top() + WindowInset;
        if( Anchor )
        {
            const QPoint BarAt = Bar->mapToGlobal( QPoint( 0, 0 ) );
            Left = qMax( qMin( Anchor->mapToGlobal( QPoint( 0, 0 ) ).x(), Window.right() + 1 - WindowInset - Width ), Window.left() + WindowInset );
            Top  = BarAt.y() - BarGap - PanelHeight;
            if( Top < Window.top() ) Top = BarAt.y() + Bar->height() + BarGap;
            if( const auto* Screen = Anchor->screen() )
            {
                const QRect Available = Screen->availableGeometry();
                Top = qMax( Available.top(), qMin( Top, Available.bottom() + 1 - PanelHeight ) );
            }
        }
        else
            qCWarning( popupLog ) << "status popup: the anchor is gone; showing at the window corner";
        setFixedSize( Outer, Height );
        move( Left - ShadowMargins.left(), Top - ShadowMargins.top() );
    }

    void DocumentStatusPopup::selectTab( QWidget* Strip, int Index )
    {
        auto*      Tabs    = static_cast< TabStrip* >( Strip );
        const auto Buttons = Strip->findChildren< QAbstractButton* >( Qt::FindDirectChildrenOnly );
        for( int I = 0; I < Buttons.size(); ++I ) Buttons[ I ]->setChecked( I == Index );
        for( int I = 0; I < Tabs->pages_.size(); ++I ) Tabs->pages_[ I ]->setVisible( I == Index );
        Rove( Strip, Buttons.value( Index ) );
        // 열려 있으면 카드 높이를 새 쪽에 맞춘다. 아래 끝은 상태표시줄 위에 그대로 있다.
        if( isVisible() ) fit();
    }

    QRect DocumentStatusPopup::panelRect() const
    {
        return rect().marginsRemoved( ShadowMargins );
    }
}
