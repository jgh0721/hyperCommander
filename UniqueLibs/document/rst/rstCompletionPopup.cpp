#include "stdafx.h"
#include "rstCompletionPopup.hpp"

/// 모듈
#include "rstOfflineCompletions.hpp"
#include "core/ThemeManager.h"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QListWidget>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSet>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

constexpr int kRoleLabel = Qt::UserRole + 1;
constexpr int kRoleInsertText = Qt::UserRole + 2;
constexpr int kRoleDetail = Qt::UserRole + 3;
constexpr int kRoleKind = Qt::UserRole + 4;
constexpr int kRoleMatches = Qt::UserRole + 5;

constexpr int kMaxVisibleRows = 12;
constexpr int kIconSize = 16;
constexpr int kMargin = 4;

struct KindIcon
{
    QChar                               letter;
    const char*                         color;
};

/// LSP CompletionItemKind -> 한 글자 배지.
KindIcon iconForKind( const int kind )
{
    switch( kind )
    {
        case 1:  return { QLatin1Char( 'T' ), "#6b9bd2" };   // Text
        case 2:
        case 3:
        case 4:  return { QLatin1Char( 'F' ), "#a855f7" };   // Method/Function/Constructor
        case 5:
        case 6:  return { QLatin1Char( 'V' ), "#22c55e" };   // Field/Variable
        case 7:  return { QLatin1Char( 'C' ), "#e8a838" };   // Class -> directive
        case 8:  return { QLatin1Char( 'I' ), "#e8a838" };   // Interface
        case 9:  return { QLatin1Char( 'M' ), "#e8a838" };   // Module
        case 10: return { QLatin1Char( 'P' ), "#22c55e" };   // Property -> 옵션
        case 12: return { QLatin1Char( 'V' ), "#22c55e" };   // Value
        case 13:
        case 20: return { QLatin1Char( 'E' ), "#e8a838" };   // Enum/EnumMember
        case 14: return { QLatin1Char( 'K' ), "#ef4444" };   // Keyword -> role
        case 15: return { QLatin1Char( 'S' ), "#6b9bd2" };   // Snippet
        case 17: return { QLatin1Char( 'F' ), "#8b5cf6" };   // File
        case 18: return { QLatin1Char( 'R' ), "#3b82f6" };   // Reference -> :ref: 대상
        case 19: return { QLatin1Char( '/' ), "#8b5cf6" };   // Folder — 'D' 보다 '/' 가 즉시 읽힌다
        case rstcomplete::kKindImageFile:
                 return { QChar( 0x25A3 ), "#10b981" };      // 상세에 프리뷰가 뜨는 이미지
        case rstcomplete::kKindSubstitution:
                 return { QLatin1Char( '|' ), "#0ea5e9" };   // 치환 참조 |name|
        case 21: return { QLatin1Char( 'C' ), "#22c55e" };   // Constant
        case 22: return { QLatin1Char( 'S' ), "#e8a838" };   // Struct
        default: return { QLatin1Char( 0x00b7 ), "#888888" };
    }
}

/// [배지] 라벨 ............ 상세
///
/// 일치한 글자를 굵게+강조색으로 칠한다. 어떤 글자 때문에 이 후보가 남았는지
/// 보이지 않으면 퍼지 매칭 결과가 임의로 느껴진다.
class CompletionItemDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint( QPainter* painter, const QStyleOptionViewItem& option,
                const QModelIndex& index ) const override
    {
        QStyleOptionViewItem styled = option;
        initStyleOption( &styled, index );
        styled.text.clear();   // 텍스트는 직접 그린다

        QStyle* style = styled.widget != nullptr ? styled.widget->style() : QApplication::style();
        style->drawPrimitive( QStyle::PE_PanelItemViewItem, &styled, painter, styled.widget );

        painter->save();
        painter->setRenderHint( QPainter::Antialiasing, true );

        const QRect rect = styled.rect;
        const QFont baseFont = styled.font;

        // ── 종류 배지 ──
        const KindIcon icon = iconForKind( index.data( kRoleKind ).toInt() );
        const QRect iconRect( rect.left() + kMargin,
                             rect.top() + ( rect.height() - kIconSize ) / 2,
                             kIconSize, kIconSize );
        QColor accent( QLatin1String( icon.color ) );
        QColor background = accent;
        background.setAlpha( 48 );
        painter->setPen( Qt::NoPen );
        painter->setBrush( background );
        painter->drawRoundedRect( iconRect, 3, 3 );

        QFont letterFont = baseFont;
        letterFont.setPointSize( qMax( baseFont.pointSize() - 2, 7 ) );
        letterFont.setBold( true );
        painter->setFont( letterFont );
        painter->setPen( accent );
        painter->drawText( iconRect, Qt::AlignCenter, QString( icon.letter ) );

        // ── 상세는 오른쪽에 흐리게 ──
        const QString detail = index.data( kRoleDetail ).toString();
        QFont detailFont = baseFont;
        detailFont.setPointSize( qMax( baseFont.pointSize() - 1, 7 ) );
        const QFontMetrics detailMetrics( detailFont );
        const int detailWidth = detail.isEmpty()
                                    ? 0
                                    : qMin( detailMetrics.horizontalAdvance( detail ) + kMargin * 2,
                                           rect.width() * 2 / 5 );

        const int labelLeft = iconRect.right() + kMargin * 2;
        const QRect labelRect( labelLeft, rect.top(),
                              qMax( rect.right() - labelLeft - detailWidth - kMargin, 40 ),
                              rect.height() );

        drawLabel( painter, labelRect, index.data( kRoleLabel ).toString(),
                  index.data( kRoleMatches ).toList(), baseFont, styled );

        if( !detail.isEmpty() )
        {
            painter->setFont( detailFont );
            // 팔레트에서 읽지 않는다. 테마를 바꿀 때 Qlementine 이
            // QApplication::setPalette() 를 부르는 순서가 우리 그리기와 보장되지 않는다.
            // 또 이 열은 경로 후보에서 동명 파일을 가르는 정보라 알파로 더 흐리게
            // 만들지 않는다 (작은 글씨만으로도 충분히 부차적으로 보인다).
            QColor dimmed = ThemeManager::instance().color( QStringLiteral( "common.foregroundMuted" ) );
            if( styled.state.testFlag( QStyle::State_Selected ) )
            {
                // 선택 배경 위에서는 테마의 보조 글자색이 묻힌다.
                dimmed = styled.palette.highlightedText().color();
                dimmed.setAlpha( 180 );
            }
            painter->setPen( dimmed );
            const QRect detailRect( rect.right() - detailWidth - kMargin, rect.top(),
                                   detailWidth, rect.height() );
            // 경로에서 구분에 쓰이는 정보는 **꼬리**다. ElideRight 로 자르면
            // "Resources/Novel/Pt1/Vol1" 과 "Resources/Novel/Pt5/Vol12" 가
            // 둘 다 "Resources/Novel/Pt..." 가 되어 구분이 완전히 사라진다.
            const int kind = index.data( kRoleKind ).toInt();
            const bool pathLike = kind == 17 || kind == 19 || kind == rstcomplete::kKindImageFile;
            painter->drawText( detailRect, Qt::AlignVCenter | Qt::AlignRight,
                              detailMetrics.elidedText( detail,
                                                       pathLike ? Qt::ElideLeft : Qt::ElideRight,
                                                       detailRect.width() ) );
        }

        painter->restore();
    }

    QSize sizeHint( const QStyleOptionViewItem& option, const QModelIndex& index ) const override
    {
        Q_UNUSED( index );
        return { 0, qMax( option.fontMetrics.height() + 6, 22 ) };
    }

private:
    static void drawLabel( QPainter* painter, const QRect& rect, const QString& label,
                           const QVariantList& matches, const QFont& font,
                           const QStyleOptionViewItem& option )
    {
        const QColor normal = option.state.testFlag( QStyle::State_Selected )
                                  ? option.palette.highlightedText().color()
                                  : option.palette.text().color();

        if( matches.isEmpty() )
        {
            painter->setFont( font );
            painter->setPen( normal );
            painter->drawText( rect, Qt::AlignVCenter | Qt::AlignLeft,
                              QFontMetrics( font ).elidedText( label, Qt::ElideRight, rect.width() ) );
            return;
        }

        QSet< int > highlighted;
        for( const QVariant& value : matches )
            highlighted.insert( value.toInt() );

        QFont boldFont = font;
        boldFont.setBold( true );

        const QFontMetrics metrics( font );
        const int baseline = rect.center().y() + metrics.ascent() / 2 - 1;
        // 선택된 행에서는 강조색이 배경에 묻히므로 굵게만 남긴다.
        // 하드코딩한 #1a73e8 은 다크 배경에서 대비가 3.2:1 로 모자랐다.
        // 테마의 강조색은 라이트/다크 양쪽에서 검증된 값이다.
        const QColor accent = option.state.testFlag( QStyle::State_Selected )
                                  ? normal
                                  : ThemeManager::instance().color( QStringLiteral( "common.accent" ) );

        int x = rect.left();
        for( int index = 0; index < label.length(); ++index )
        {
            if( x > rect.right() )
                break;
            const bool isMatch = highlighted.contains( index );
            painter->setFont( isMatch ? boldFont : font );
            painter->setPen( isMatch ? accent : normal );
            painter->drawText( x, baseline, QString( label.at( index ) ) );
            x += QFontMetrics( painter->font() ).horizontalAdvance( label.at( index ) );
        }
    }
};

}  // namespace


CompletionPopupWidget::CompletionPopupWidget( QWidget* parent )
    : QFrame( parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint )
{
    setAttribute( Qt::WA_ShowWithoutActivating );
    setFocusPolicy( Qt::NoFocus );
    setFrameShape( QFrame::StyledPanel );
    setFrameShadow( QFrame::Plain );
    setAutoFillBackground( true );

    QPalette framePalette = palette();
    framePalette.setColor( QPalette::Window, framePalette.color( QPalette::Base ) );
    setPalette( framePalette );

    list_ = new QListWidget( this );
    list_->setFocusPolicy( Qt::NoFocus );
    list_->setFrameShape( QFrame::NoFrame );
    list_->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    list_->setVerticalScrollBarPolicy( Qt::ScrollBarAsNeeded );
    list_->setSelectionMode( QAbstractItemView::SingleSelection );
    list_->setUniformItemSizes( true );
    list_->setItemDelegate( new CompletionItemDelegate( list_ ) );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 1, 1, 1, 1 );
    layout->setSpacing( 0 );
    layout->addWidget( list_ );

    connect( list_, &QListWidget::clicked, this, [ this ]( const QModelIndex& ) { acceptCurrent(); } );
    connect( list_, &QListWidget::currentItemChanged, this,
             [ this ]( QListWidgetItem*, QListWidgetItem* ) { emitCurrentIfChanged(); } );
}

CompletionDisplayItem CompletionPopupWidget::currentItem() const
{
    CompletionDisplayItem item;
    const QListWidgetItem* current = list_ != nullptr ? list_->currentItem() : nullptr;
    if( current == nullptr )
        return item;

    item.label = current->data( kRoleLabel ).toString();
    item.insertText = current->data( kRoleInsertText ).toString();
    item.detail = current->data( kRoleDetail ).toString();
    item.kind = current->data( kRoleKind ).toInt();
    return item;
}

void CompletionPopupWidget::setItems( const QList< CompletionDisplayItem >& items )
{
    allItems_ = items;
    prefix_.clear();
    rebuild();
    emitCurrentIfChanged();
}

void CompletionPopupWidget::updateFilter( const QString& prefix )
{
    // 같은 접두로 목록만 새로 받은 경우(LSP 응답 도착 등) 사용자가 방향키로
    // 고른 항목을 유지한다. 그러지 않으면 선택이 매번 첫 줄로 되돌아간다.
    const bool samePrefix = prefix == prefix_;
    const QString keepSelected = samePrefix ? currentInsertText() : QString{};

    prefix_ = prefix;
    rebuild();

    if( list_->count() == 0 )
    {
        hide();
        return;
    }
    if( !keepSelected.isEmpty() )
        selectByInsertText( keepSelected );

    emitCurrentIfChanged();
}

void CompletionPopupWidget::showAt( const QPoint& globalTopLeft )
{
    if( list_->count() == 0 )
    {
        hide();
        return;
    }

    anchor_ = globalTopLeft;
    hasAnchor_ = true;
    applyGeometry();
    show();
    raise();
}

void CompletionPopupWidget::refreshGeometry()
{
    if( !isVisible() || !hasAnchor_ || list_->count() == 0 )
        return;
    applyGeometry();
}

void CompletionPopupWidget::applyGeometry()
{
    resizeToRows();

    QPoint target = anchor_;
    const QScreen* screen = QGuiApplication::screenAt( anchor_ );
    if( screen == nullptr )
        screen = QGuiApplication::primaryScreen();
    if( screen != nullptr )
    {
        const QRect available = screen->availableGeometry();
        if( target.x() + width() > available.right() )
            target.setX( qMax( available.left(), available.right() - width() ) );
        // 아래로 넘치면 캐럿 위로 올린다. 잘린 목록보다 낫다.
        if( target.y() + height() > available.bottom() )
        {
            const QFontMetrics metrics( font() );
            target.setY( qMax( available.top(), anchor_.y() - height() - metrics.height() ) );
        }
    }

    if( pos() != target )
        move( target );
}

void CompletionPopupWidget::emitCurrentIfChanged()
{
    const QString key = currentInsertText();
    if( key == lastEmittedInsertText_ )
        return;
    lastEmittedInsertText_ = key;
    emit currentItemChanged( currentItem() );
}

bool CompletionPopupWidget::isActive() const
{
    return isVisible() && list_ != nullptr && list_->count() > 0;
}

int CompletionPopupWidget::visibleCount() const
{
    return list_ != nullptr ? list_->count() : 0;
}

int CompletionPopupWidget::pageStep() const
{
    if( list_ == nullptr || list_->count() <= 0 )
        return 1;
    const int rowHeight = qMax( list_->sizeHintForRow( 0 ), 1 );
    const int visible = qMax( 1, list_->viewport()->height() / rowHeight );
    return qMin( visible, list_->count() );
}

void CompletionPopupWidget::selectRow( const int row )
{
    if( list_->count() <= 0 )
        return;
    list_->setCurrentRow( qBound( 0, row, list_->count() - 1 ) );
    list_->scrollToItem( list_->currentItem() );
}

void CompletionPopupWidget::selectNextPage()
{
    selectRow( list_->currentRow() + pageStep() );
}

void CompletionPopupWidget::selectPreviousPage()
{
    selectRow( list_->currentRow() - pageStep() );
}

void CompletionPopupWidget::selectNext()
{
    if( list_->count() <= 0 )
        return;
    const int next = ( list_->currentRow() + 1 ) % list_->count();
    list_->setCurrentRow( next );
    list_->scrollToItem( list_->currentItem() );
}

void CompletionPopupWidget::selectPrevious()
{
    if( list_->count() <= 0 )
        return;
    int previous = list_->currentRow() - 1;
    if( previous < 0 )
        previous = list_->count() - 1;
    list_->setCurrentRow( previous );
    list_->scrollToItem( list_->currentItem() );
}

bool CompletionPopupWidget::acceptCurrent()
{
    const QString insertText = currentInsertText();
    if( insertText.isEmpty() )
        return false;

    hide();
    emit itemSelected( insertText );
    return true;
}

void CompletionPopupWidget::hideEvent( QHideEvent* event )
{
    // 다시 뜰 때 같은 항목이라도 상세 패널을 새로 채워야 한다.
    lastEmittedInsertText_.clear();
    hasAnchor_ = false;
    QFrame::hideEvent( event );
    emit popupHidden();
}

bool CompletionPopupWidget::handleKeyPress( QKeyEvent* event )
{
    if( !isActive() || event == nullptr )
        return false;

    switch( event->key() )
    {
        case Qt::Key_Escape:
            hide();
            return true;
        case Qt::Key_Down:
            selectNext();
            return true;
        case Qt::Key_Up:
            selectPrevious();
            return true;
        // 삼켜야 한다. 그냥 두면 목록은 그대로인 채 **편집기가** 스크롤되어
        // 캐럿과 팝업이 따로 논다.
        case Qt::Key_PageDown:
            selectNextPage();
            return true;
        case Qt::Key_PageUp:
            selectPreviousPage();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Tab:
            return acceptCurrent();
        default:
            return false;
    }
}

void CompletionPopupWidget::rebuild()
{
    struct Scored
    {
        const CompletionDisplayItem*    item;
        int                             score;
        QVector< int >                  matches;
    };

    QVector< Scored > visible;
    visible.reserve( allItems_.size() );
    for( const CompletionDisplayItem& item : allItems_ )
    {
        const QString candidate = item.filterText.isEmpty() ? item.label : item.filterText;
        int score = 0;
        QVector< int > matches;
        if( rstcomplete::fuzzyMatchCompletion( prefix_, candidate, &score, &matches ) )
            visible.push_back( { &item, score + item.scoreBias, matches } );
    }

    // 접두가 없으면 공급자(LSP/오프라인 표)가 준 순서를 그대로 존중한다.
    if( !prefix_.isEmpty() )
    {
        std::stable_sort( visible.begin(), visible.end(),
                         []( const Scored& left, const Scored& right )
                         {
                             if( left.score != right.score )
                                 return left.score > right.score;
                             return left.item->label.length() < right.item->label.length();
                         } );
    }

    // clear() 는 current 를 무효화해 currentItemChanged 를 흘린다. 그대로 두면
    // 글자 하나에 상세 패널이 껐다 켜진다 (숨김 → 첫 항목 선택 → 다시 표시).
    // 채우기가 끝난 뒤 호출자가 emitCurrentIfChanged() 로 한 번만 알린다.
    const QSignalBlocker blocker( list_ );

    list_->clear();
    for( const Scored& scored : visible )
    {
        auto* row = new QListWidgetItem( list_ );
        row->setData( kRoleLabel, scored.item->label );
        row->setData( kRoleInsertText, scored.item->insertText.isEmpty() ? scored.item->label
                                                                        : scored.item->insertText );
        row->setData( kRoleDetail, scored.item->detail );
        row->setData( kRoleKind, scored.item->kind );

        QVariantList matches;
        // 강조 위치는 filterText 기준이므로 label 과 다르면 표시하지 않는다.
        if( scored.item->filterText.isEmpty() || scored.item->filterText == scored.item->label )
        {
            for( const int position : scored.matches )
                matches.append( position );
        }
        row->setData( kRoleMatches, matches );
        // 오른쪽 흐린 글씨는 잘릴 수 있다. 마우스 사용자에게는 전체를 보여 준다.
        row->setToolTip( scored.item->insertText.isEmpty() ? scored.item->detail
                                                           : scored.item->insertText );
    }

    selectFirst();
    if( isVisible() )
        resizeToRows();
}

void CompletionPopupWidget::resizeToRows()
{
    if( list_->count() <= 0 )
        return;

    const int rowHeight = qMax( list_->sizeHintForRow( 0 ), 22 );
    const int rows = qMin( list_->count(), kMaxVisibleRows );

    // 깊은 경로 하나가 목록 폭을 늘 상한까지 밀어 올리지 않게 상세 기여분을 자른다.
    // 델리게이트도 어차피 rect.width()*2/5 까지만 그린다.
    constexpr int kMaxDetailHint = 200;

    int widest = 0;
    const QFontMetrics metrics( font() );
    for( int index = 0; index < list_->count(); ++index )
    {
        const int labelWidth = metrics.horizontalAdvance( list_->item( index )->data( kRoleLabel ).toString() );
        const int detailWidth = metrics.horizontalAdvance( list_->item( index )->data( kRoleDetail ).toString() );
        widest = qMax( widest, labelWidth + qMin( detailWidth, kMaxDetailHint ) );
    }
    const int chrome = kIconSize + kMargin * 6 + list_->verticalScrollBar()->sizeHint().width();

    setFixedSize( qBound( 280, widest + chrome, 720 ), rowHeight * rows + 2 );
}

QString CompletionPopupWidget::currentInsertText() const
{
    const QListWidgetItem* current = list_ != nullptr ? list_->currentItem() : nullptr;
    return current != nullptr ? current->data( kRoleInsertText ).toString() : QString{};
}

bool CompletionPopupWidget::selectByInsertText( const QString& insertText )
{
    for( int index = 0; index < list_->count(); ++index )
    {
        if( list_->item( index )->data( kRoleInsertText ).toString() == insertText )
        {
            list_->setCurrentRow( index );
            list_->scrollToItem( list_->item( index ) );
            return true;
        }
    }
    return false;
}

void CompletionPopupWidget::selectFirst()
{
    if( list_->count() <= 0 )
        return;
    list_->setCurrentRow( 0 );
    list_->scrollToTop();
}

}  // namespace hc::rst
