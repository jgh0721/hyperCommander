#include "stdafx.h"
#include "recentDocumentsDialog.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/qtMessageDialog.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShowEvent>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        using fm::style::Token;

        QFont SmallFont( QFont Font )
        {
            // 시안의 <small> · 12 px 머리(본문 13 px 기준)에 맞춘 비율.
            if( Font.pointSizeF() > 0 ) Font.setPointSizeF( Font.pointSizeF() * 0.92 );
            else if( Font.pixelSize() > 0 ) Font.setPixelSize( qMax( 1, qRound( Font.pixelSize() * 0.92 ) ) );
            return Font;
        }

        // 작은 압정: 둥근 머리 · 몸통 · 바늘.
        void PaintPin( QPainter* Painter, const QRectF& Box, const QColor& Color )
        {
            const qreal Unit = Box.width() / 12.0;
            const QPointF Top( Box.center().x(), Box.top() + 1.5 * Unit );
            QPainterPath Head;
            Head.addRoundedRect( QRectF( Top.x() - 3 * Unit, Top.y(), 6 * Unit, 4 * Unit ), 1.2 * Unit, 1.2 * Unit );
            QPainterPath Body;
            Body.moveTo( Top.x() - 4.5 * Unit, Top.y() + 7 * Unit );
            Body.lineTo( Top.x() + 4.5 * Unit, Top.y() + 7 * Unit );
            Body.lineTo( Top.x() + 2.5 * Unit, Top.y() + 4 * Unit );
            Body.lineTo( Top.x() - 2.5 * Unit, Top.y() + 4 * Unit );
            Body.closeSubpath();
            Painter->save();
            Painter->setRenderHint( QPainter::Antialiasing );
            Painter->setPen( Qt::NoPen );
            Painter->setBrush( Color );
            Painter->drawPath( Head.united( Body ) );
            Painter->setPen( QPen( Color, qMax< qreal >( 1.0, Unit ), Qt::SolidLine, Qt::RoundCap ) );
            Painter->drawLine( QPointF( Top.x(), Top.y() + 7 * Unit ), QPointF( Top.x(), Top.y() + 10.5 * Unit ) );
            Painter->restore();
        }

        /// 두 줄 행: 굵은 파일 이름과 오른쪽 날짜, 흐린 설명. 고정 항목은 이름 앞에 압정.
        class RecentItemDelegate final : public QStyledItemDelegate
        {
        public:
            using QStyledItemDelegate::QStyledItemDelegate;

            QSize sizeHint( const QStyleOptionViewItem& Option, const QModelIndex& ) const override
            {
                QFont Bold = Option.font;
                Bold.setWeight( QFont::DemiBold );
                return { 240, 8 + QFontMetrics( Bold ).height() + 2 + QFontMetrics( SmallFont( Option.font ) ).height() + 8 };
            }

            void paint( QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index ) const override
            {
                QStyleOptionViewItem Item( Option );
                initStyleOption( &Item, Index );
                Item.text.clear();
                Item.icon = {};
                const auto* Widget = Item.widget;
                auto*       Style  = Widget ? Widget->style() : QApplication::style();
                Style->drawPrimitive( QStyle::PE_PanelItemViewItem, &Item, Painter, Widget );

                const auto  Document = Index.data( RecentDocumentsDialog::DocumentRole ).value< RecentDocument >();
                const bool  Missing  = Index.data( RecentDocumentsDialog::MissingRole ).toBool();
                const auto& Colors   = fm::style::themeColorsFor( Widget );
                // 시안2 · 시안3은 선택을 강조색으로 채우므로 글자를 강조 위 글자색으로 바꾼다.
                const bool OnFill  = ( Item.state & QStyle::State_Selected ) && Colors.fillsSelection();
                const auto Primary = OnFill ? Colors[ Token::OnAccent ] : Colors[ Token::Fg ];
                auto       Muted   = OnFill ? Colors[ Token::OnAccent ] : Colors[ Token::Fg3 ];
                if( OnFill ) Muted.setAlphaF( 0.8f );

                QFont Bold = Item.font;
                Bold.setWeight( QFont::DemiBold );
                const auto        Small = SmallFont( Item.font );
                const QFontMetrics BoldMetrics( Bold ), SmallMetrics( Small );

                Painter->save();
                if( Missing ) Painter->setOpacity( 0.5 );
                const QRect Area     = Item.rect.adjusted( 10, 8, -10, -8 );
                const int   PinWidth = 16;
                if( Document.pinned_ ) PaintPin( Painter, QRectF( Area.left(), Area.top() + ( BoldMetrics.height() - 12 ) / 2.0, 12, 12 ), OnFill ? Primary : Colors[ Token::Accent ] );
                const int Left = Area.left() + PinWidth;

                const auto Date      = RecentDateText( Document.openedAt_, QDateTime::currentDateTime() );
                const int  DateWidth = SmallMetrics.horizontalAdvance( Date );
                const int  FirstLine = Area.top() + BoldMetrics.ascent();
                Painter->setFont( Small );
                Painter->setPen( Muted );
                Painter->drawText( Area.right() + 1 - DateWidth, FirstLine, Date );

                const int NameWidth = qMax( 0, Area.right() + 1 - DateWidth - 12 - Left );
                Painter->setFont( Bold );
                Painter->setPen( Primary );
                Painter->drawText( Left, FirstLine, BoldMetrics.elidedText( QFileInfo( Document.path_ ).fileName(), Qt::ElideMiddle, NameWidth ) );

                auto Description = RecentDescription( Document );
                if( Missing ) Description += QLatin1Char( ' ' ) + RecentDocumentsDialog::tr( "(찾을 수 없음)" );
                Painter->setFont( Small );
                Painter->setPen( Muted );
                Painter->drawText( Left, Area.top() + BoldMetrics.height() + 2 + SmallMetrics.ascent(),
                                   SmallMetrics.elidedText( Description, Qt::ElideRight, qMax( 0, Area.right() + 1 - Left ) ) );
                Painter->restore();
            }
        };
    }

    RecentDocumentsDialog::RecentDocumentsDialog( RecentDocumentStore* Store, QWidget* Parent )
        : QDialog( Parent ), store_( Store ), probe_( new RecentPathProbe( this ) )
    {
        setObjectName( QStringLiteral( "recentDocumentsDialog" ) );
        setWindowTitle( tr( "최근 항목" ) );
        setModal( false );

        auto* Layout = new QVBoxLayout( this );
        Layout->setSpacing( 8 );
        search_ = new QLineEdit( this );
        search_->setObjectName( QStringLiteral( "recentSearch" ) );
        search_->setPlaceholderText( tr( "최근 항목 검색" ) );
        search_->setClearButtonEnabled( true );
        search_->installEventFilter( this );
        list_ = new QListWidget( this );
        list_->setObjectName( QStringLiteral( "recentList" ) );
        list_->setItemDelegate( new RecentItemDelegate( list_ ) );
        list_->setSelectionMode( QAbstractItemView::SingleSelection );
        list_->setUniformItemSizes( true );
        list_->setMouseTracking( true );
        list_->viewport()->setAttribute( Qt::WA_Hover );
        list_->setMinimumSize( 520, 320 );
        list_->installEventFilter( this );
        Layout->addWidget( search_ );
        Layout->addWidget( list_, 1 );

        auto* Footer = hc::ui::ApplyDialogFrame( this, { {}, tr( "최근에 연 문서를 찾아 열거나 고정하고 목록에서 제거합니다." ), fm::ui::glyph::Clock } );
        const auto Button = [ Footer ]( const QString& Text, const char* Name ) {
            auto* Result = new QPushButton( Text, Footer->parentWidget() );
            Result->setObjectName( QLatin1String( Name ) );
            Footer->addWidget( Result );
            return Result;
        };
        open_   = Button( tr( "열기" ), "recentOpen" );
        pin_    = Button( tr( "고정" ), "recentPin" );
        remove_ = Button( tr( "목록에서 제거" ), "recentRemove" );
        clear_  = Button( tr( "모두 지우기" ), "recentClearAll" );
        close_  = Button( tr( "닫기" ), "recentClose" );
        fm::style::setButtonRole( open_, fm::style::ButtonRole::Primary );
        // 검색 입력에서 Enter 를 누르면 기본 단추(열기)가 받는다. 목록의 Enter 는 eventFilter 가 먼저 처리한다.
        open_->setDefault( true );

        connect( open_, &QPushButton::clicked, this, [ this ] { openItem( currentVisible() ? currentVisible() : firstVisible() ); } );
        connect( pin_, &QPushButton::clicked, this, &RecentDocumentsDialog::togglePin );
        connect( remove_, &QPushButton::clicked, this, &RecentDocumentsDialog::removeCurrent );
        connect( clear_, &QPushButton::clicked, this, &RecentDocumentsDialog::confirmClear );
        connect( close_, &QPushButton::clicked, this, &QDialog::close );
        connect( search_, &QLineEdit::textChanged, this, &RecentDocumentsDialog::applyFilter );
        connect( list_, &QListWidget::itemDoubleClicked, this, &RecentDocumentsDialog::openItem );
        connect( list_, &QListWidget::currentItemChanged, this, &RecentDocumentsDialog::refreshButtons );
        connect( probe_, &RecentPathProbe::Checked, this, &RecentDocumentsDialog::refreshMissing );
        connect( &fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, list_->viewport(), qOverload<>( &QWidget::update ) );
        if( Store ) connect( Store, &RecentDocumentStore::Changed, this, &RecentDocumentsDialog::rebuild );

        resize( 600, 480 );
        rebuild();
        search_->setFocus();
    }

    RecentDocumentsDialog::~RecentDocumentsDialog() = default;

    bool RecentDocumentsDialog::eventFilter( QObject* Watched, QEvent* Event )
    {
        if( Event->type() == QEvent::KeyPress ) {
            const auto* Key       = static_cast< QKeyEvent* >( Event );
            const auto  Modifiers = Key->modifiers() & ~Qt::KeypadModifier;
            if( Watched == list_ && Modifiers == Qt::NoModifier ) {
                // QAbstractItemView 는 Enter 에 activated 를 내고 이벤트를 넘겨 기본 단추까지 눌리게 하므로 여기서 끝낸다.
                if( Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter ) { openItem( currentVisible() ); return true; }
                if( Key->key() == Qt::Key_Delete ) { removeCurrent(); return true; }
            }
            if( Watched == search_ && Modifiers == Qt::NoModifier && ( Key->key() == Qt::Key_Down || Key->key() == Qt::Key_PageDown ) ) {
                if( !currentVisible() ) list_->setCurrentItem( firstVisible() );
                list_->setFocus( Qt::TabFocusReason );
                return true;
            }
        }
        return QDialog::eventFilter( Watched, Event );
    }

    void RecentDocumentsDialog::showEvent( QShowEvent* Event )
    {
        QDialog::showEvent( Event );
        rebuild();
    }

    void RecentDocumentsDialog::hideEvent( QHideEvent* Event )
    {
        // 닫으면 남은 확인을 멈춘다(계획 6.5).
        probe_->Cancel();
        QDialog::hideEvent( Event );
    }

    void RecentDocumentsDialog::rebuild()
    {
        QString Selected;
        if( const auto* Current = list_->currentItem() ) Selected = Current->data( DocumentRole ).value< RecentDocument >().Key();
        const int Row = list_->currentRow();

        list_->clear();
        QStringList Paths;
        if( store_ ) {
            for( const auto& Document : store_->Entries() ) {
                auto*      Item = new QListWidgetItem( list_ );
                const auto Name = QFileInfo( Document.path_ ).fileName();
                // 표시 문자열은 대리자가 그리지만, 입력 검색 · 접근성은 이 값을 쓴다.
                Item->setData( Qt::DisplayRole, Name );
                Item->setData( Qt::AccessibleTextRole, Name + QStringLiteral( ", " ) + RecentDescription( Document ) );
                Item->setData( DocumentRole, QVariant::fromValue( Document ) );
                Item->setData( MissingRole, probe_->StateOf( Document.path_ ) == RecentPathProbe::State::Missing );
                Item->setToolTip( QDir::toNativeSeparators( Document.path_ ) );
                Paths.append( Document.path_ );
            }
        }
        applyFilter();

        QListWidgetItem* Restore = nullptr;
        for( int Index = 0; Index < list_->count() && !Selected.isEmpty(); ++Index )
            if( list_->item( Index )->data( DocumentRole ).value< RecentDocument >().Key() == Selected ) Restore = list_->item( Index );
        // 지운 항목 자리는 같은 줄의 다음 항목이 이어받는다.
        if( !Restore && Row >= 0 && list_->count() > 0 ) Restore = list_->item( qMin( Row, list_->count() - 1 ) );
        if( Restore && Restore->isHidden() ) Restore = firstVisible();
        if( Restore ) list_->setCurrentItem( Restore );
        refreshButtons();
        if( isVisible() ) probe_->Check( Paths );
    }

    void RecentDocumentsDialog::applyFilter()
    {
        const auto Text = search_->text().trimmed();
        for( int Index = 0; Index < list_->count(); ++Index ) {
            auto*      Item     = list_->item( Index );
            const auto Document = Item->data( DocumentRole ).value< RecentDocument >();
            const bool Match    = Text.isEmpty() || QFileInfo( Document.path_ ).fileName().contains( Text, Qt::CaseInsensitive )
                || QDir::toNativeSeparators( Document.path_ ).contains( QDir::toNativeSeparators( Text ), Qt::CaseInsensitive );
            Item->setHidden( !Match );
        }
        if( !currentVisible() ) list_->setCurrentItem( firstVisible() );
        refreshButtons();
    }

    void RecentDocumentsDialog::refreshButtons()
    {
        const auto* Item = currentVisible();
        open_->setEnabled( Item || firstVisible() );
        pin_->setEnabled( Item );
        remove_->setEnabled( Item );
        clear_->setEnabled( list_->count() > 0 );
        const bool Pinned = Item && Item->data( DocumentRole ).value< RecentDocument >().pinned_;
        pin_->setText( Pinned ? tr( "고정 해제" ) : tr( "고정" ) );
    }

    void RecentDocumentsDialog::refreshMissing( const QString& Path )
    {
        const auto Key     = RecentKey( Path );
        const bool Missing = probe_->StateOf( Path ) == RecentPathProbe::State::Missing;
        for( int Index = 0; Index < list_->count(); ++Index ) {
            auto* Item = list_->item( Index );
            if( Item->data( DocumentRole ).value< RecentDocument >().Key() == Key ) Item->setData( MissingRole, Missing );
        }
    }

    void RecentDocumentsDialog::openItem( QListWidgetItem* Item )
    {
        if( !Item || Item->isHidden() ) return;
        // 받는 쪽이 기록을 갱신하면 목록이 다시 만들어지므로 값을 먼저 꺼낸다.
        const auto Document = Item->data( DocumentRole ).value< RecentDocument >();
        emit OpenRequested( Document );
    }

    void RecentDocumentsDialog::togglePin()
    {
        const auto* Item = currentVisible();
        if( !Item || !store_ ) return;
        const auto Document = Item->data( DocumentRole ).value< RecentDocument >();
        store_->Pin( Document.Key(), !Document.pinned_ );
    }

    void RecentDocumentsDialog::removeCurrent()
    {
        const auto* Item = currentVisible();
        if( !Item || !store_ ) return;
        store_->Remove( Item->data( DocumentRole ).value< RecentDocument >().Key() );
    }

    void RecentDocumentsDialog::confirmClear()
    {
        if( !store_ ) return;
        if( confirm_ ) {
            confirm_->raise();
            confirm_->activateWindow();
            return;
        }
        auto* Box = new hc::ui::MessageDialog( QMessageBox::Question, tr( "최근 항목 모두 지우기" ),
                                               tr( "고정한 항목을 포함해 최근 항목을 모두 지웁니다. 계속할까요?" ),
                                               QMessageBox::Yes | QMessageBox::No, this );
        Box->setObjectName( QStringLiteral( "recentClearConfirm" ) );
        Box->setAttribute( Qt::WA_DeleteOnClose );
        Box->button( QMessageBox::Yes )->setText( tr( "모두 지우기" ) );
        Box->button( QMessageBox::No )->setText( tr( "취소" ) );
        Box->setDefaultButton( QMessageBox::No );
        connect( Box, &QMessageBox::finished, this, [ this ]( int Result ) {
            if( Result == QMessageBox::Yes && store_ ) store_->Clear( true );
        } );
        confirm_ = Box;
        Box->open();
    }

    QListWidgetItem* RecentDocumentsDialog::currentVisible() const
    {
        auto* Item = list_->currentItem();
        return Item && !Item->isHidden() ? Item : nullptr;
    }

    QListWidgetItem* RecentDocumentsDialog::firstVisible() const
    {
        for( int Index = 0; Index < list_->count(); ++Index )
            if( !list_->item( Index )->isHidden() ) return list_->item( Index );
        return nullptr;
    }
}
