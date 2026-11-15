#pragma once

/// 외부 라이브러리
#include <fmstyle/ThemeManager.h>
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QAbstractItemModel>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <algorithm>

// 프로세스 트리의 연결선. Qtitan은 조상 칸마다 '다음 형제 있음'을 늘 켠 채 PE_IndicatorBranch를 부르므로,
// 행의 실제 조상(셀 옵션의 모델 인덱스)으로 칸마다 다시 그린다. 나머지 요소는 모두 지금의 앱 스타일에 넘긴다
// (테마를 바꾸면 앱 스타일이 바뀌므로 소유하지 않고 호출할 때마다 찾는다).
class TreeBranchStyle final : public QStyle
{
public:
    explicit TreeBranchStyle( QObject* Parent ) { setParent( Parent ); }
    bool lines_ = true;

    void drawPrimitive( PrimitiveElement Element, const QStyleOption* Option, QPainter* Painter, const QWidget* Widget ) const override
    {
        if( Element == PE_IndicatorBranch ) { drawBranch( Option, Painter, Widget ); return; }
        app()->drawPrimitive( Element, Option, Painter, Widget );
    }
    void drawControl( ControlElement Element, const QStyleOption* Option, QPainter* Painter, const QWidget* Widget ) const override { app()->drawControl( Element, Option, Painter, Widget ); }
    QRect subElementRect( SubElement Element, const QStyleOption* Option, const QWidget* Widget ) const override { return app()->subElementRect( Element, Option, Widget ); }
    void drawComplexControl( ComplexControl Control, const QStyleOptionComplex* Option, QPainter* Painter, const QWidget* Widget ) const override { app()->drawComplexControl( Control, Option, Painter, Widget ); }
    SubControl hitTestComplexControl( ComplexControl Control, const QStyleOptionComplex* Option, const QPoint& Point, const QWidget* Widget ) const override { return app()->hitTestComplexControl( Control, Option, Point, Widget ); }
    QRect subControlRect( ComplexControl Control, const QStyleOptionComplex* Option, SubControl Sub, const QWidget* Widget ) const override { return app()->subControlRect( Control, Option, Sub, Widget ); }
    int pixelMetric( PixelMetric Metric, const QStyleOption* Option, const QWidget* Widget ) const override { return app()->pixelMetric( Metric, Option, Widget ); }
    QSize sizeFromContents( ContentsType Type, const QStyleOption* Option, const QSize& Size, const QWidget* Widget ) const override { return app()->sizeFromContents( Type, Option, Size, Widget ); }
    int styleHint( StyleHint Hint, const QStyleOption* Option, const QWidget* Widget, QStyleHintReturn* Return ) const override { return app()->styleHint( Hint, Option, Widget, Return ); }
    QPixmap standardPixmap( StandardPixmap Pixmap, const QStyleOption* Option, const QWidget* Widget ) const override { return app()->standardPixmap( Pixmap, Option, Widget ); }
    QIcon standardIcon( StandardPixmap Icon, const QStyleOption* Option, const QWidget* Widget ) const override { return app()->standardIcon( Icon, Option, Widget ); }
    QPixmap generatedIconPixmap( QIcon::Mode Mode, const QPixmap& Pixmap, const QStyleOption* Option ) const override { return app()->generatedIconPixmap( Mode, Pixmap, Option ); }
    int layoutSpacing( QSizePolicy::ControlType First, QSizePolicy::ControlType Second, Qt::Orientation Orientation, const QStyleOption* Option, const QWidget* Widget ) const override
    { return app()->layoutSpacing( First, Second, Orientation, Option, Widget ); }
    void polish( QWidget* Widget ) override { app()->polish( Widget ); }
    void unpolish( QWidget* Widget ) override { app()->unpolish( Widget ); }
    void polish( QPalette& Palette ) override { app()->polish( Palette ); }
    QPalette standardPalette() const override { return app()->standardPalette(); }
    QRect itemTextRect( const QFontMetrics& Metrics, const QRect& Rect, int Flags, bool Enabled, const QString& Text ) const override { return app()->itemTextRect( Metrics, Rect, Flags, Enabled, Text ); }
    QRect itemPixmapRect( const QRect& Rect, int Flags, const QPixmap& Pixmap ) const override { return app()->itemPixmapRect( Rect, Flags, Pixmap ); }
    void drawItemText( QPainter* Painter, const QRect& Rect, int Flags, const QPalette& Palette, bool Enabled, const QString& Text, QPalette::ColorRole Role ) const override
    { app()->drawItemText( Painter, Rect, Flags, Palette, Enabled, Text, Role ); }
    void drawItemPixmap( QPainter* Painter, const QRect& Rect, int Alignment, const QPixmap& Pixmap ) const override { app()->drawItemPixmap( Painter, Rect, Alignment, Pixmap ); }

private:
    static QStyle* app() { return QApplication::style(); }
    static bool hasNext( const QModelIndex& Index ) { return Index.row() + 1 < Index.model()->rowCount( Index.parent() ); }

    // 디자인마다 펼침 표시가 다르다(수채화 = 상자 + 점선, 기본 = 꺾쇠만). 연결선은 디자인과 관계없이 여기서
    // 그리고, 펼침 표시만 앱 스타일에 맡긴다. 상자 크기의 사각형을 넘겨 스타일이 자기 선을 긋지 않게 한다.
    void drawBranch( const QStyleOption* Option, QPainter* Painter, const QWidget* Widget ) const
    {
        const auto* Item = qstyleoption_cast<const QStyleOptionViewItem*>( Option );
        // 조상 칸은 마지막 칸(State_Item)을 그릴 때 함께 그린다.
        if( !Item || !( Option->state & State_Item ) || !Item->index.isValid() )
            return;
        const auto Index = Item->index.siblingAtColumn( 0 );
        QVector<QModelIndex> Chain;
        for( auto Up = Index; Up.isValid(); Up = Up.parent() )
            Chain.prepend( Up );
        const int Depth = int( Chain.size() );
        const QRect Own = Option->rect;
        const int Indent = Own.width();
        const int MidX = Own.left() + Indent / 2;
        const int MidY = Own.top() + Own.height() / 2;
        const bool Children = Option->state & State_Children;
        // 최상위 프로세스끼리는 잇지 않는다(목록 왼쪽 끝을 가로지르는 긴 선이 생긴다).
        if( lines_ && Depth > 1 )
        {
            const auto& Colors = fm::style::ThemeManager::instance().colors();
            const QColor Line = Colors[fm::style::Token::Fg3];
            const int Gap = Children ? 6 : 0;   // 펼침 표시 둘레는 비운다
            const auto Vertical = [&]( int X, int Top, int Bottom ) { if( Bottom > Top ) Painter->fillRect( QRect( X, Top, 1, Bottom - Top ), Line ); };
            for( int Level = 1; Level + 1 < Depth; ++Level )
                if( hasNext( Chain[Level] ) )
                    Vertical( MidX - ( Depth - 1 - Level ) * Indent, Own.top(), Own.bottom() + 1 );
            Vertical( MidX, Own.top(), MidY - Gap );
            if( hasNext( Index ) )
                Vertical( MidX, MidY + Gap + 1, Own.bottom() + 1 );
            Painter->fillRect( QRect( MidX + Gap + 1, MidY, std::max( 0, Own.right() - 1 - MidX - Gap ), 1 ), Line );
        }
        if( !Children )
            return;
        QStyleOptionViewItem Copy = *Item;
        Copy.state = Option->state & ~( State_Item | State_Sibling );
        Copy.rect = QRect( MidX - 4, MidY - 4, 9, 9 );
        app()->drawPrimitive( PE_IndicatorBranch, &Copy, Painter, Widget );
    }
};
