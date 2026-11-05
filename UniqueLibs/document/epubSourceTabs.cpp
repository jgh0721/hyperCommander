#include "stdafx.h"
#include "epubSourceTabs.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QPaintEvent>
#include <QStyleOptionFocusRect>
#include <QStyleOptionTab>
#include <QStylePainter>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    QFont EpubSourceTabs::FilenameFont() const
    {
        auto Font = font();
        if (Font.pixelSize() > 0) Font.setPixelSize(qMax(1, qRound(Font.pixelSize() * 0.9)));
        else Font.setPointSizeF(qMax(1.0, Font.pointSizeF() * 0.9));
        return Font;
    }
    QSize EpubSourceTabs::tabSizeHint(int Index) const
    {
        auto Size = QTabBar::tabSizeHint(Index);
        QStyleOptionTab Option; initStyleOption(&Option, Index);
        const auto Lines = tabText(Index).split('\n');
        auto Main = font(); Main.setWeight(QFont::DemiBold);
        const QFontMetrics MainMetrics(Main), SmallMetrics(FilenameFont());
        const bool Two = Lines.size() > 1;
        const int TextWidth = qMax(MainMetrics.horizontalAdvance(Lines.first()), Two ? SmallMetrics.horizontalAdvance(Lines.last()) : 0);
        const int Buttons = qMax(0, Option.leftButtonSize.width()) + qMax(0, Option.rightButtonSize.width());
        Size.setWidth(qBound(100, TextWidth + Buttons + style()->pixelMetric(QStyle::PM_TabBarTabHSpace, &Option, this) + 8, 360));
        Size.setHeight(qMax(Size.height(), MainMetrics.height() + (Two ? SmallMetrics.height() + 2 : 0) + 12));
        return Size;
    }
    QRect EpubSourceTabs::PrimaryTextRect(int Index) const
    {
        if (Index < 0 || Index >= count()) return {};
        QStyleOptionTab Option; initStyleOption(&Option, Index);
        auto Rect = style()->subElementRect(QStyle::SE_TabBarTabText, &Option, this);
        const int MainHeight = fontMetrics().height();
        const int TextHeight = MainHeight + (tabText(Index).contains('\n') ? QFontMetrics(FilenameFont()).height() + 2 : 0);
        Rect.setTop(Option.rect.top() + (Option.rect.height() - TextHeight) / 2);
        Rect.setHeight(MainHeight);
        return Rect;
    }
    void EpubSourceTabs::paintEvent(QPaintEvent* Event)
    {
        QStylePainter Painter(this); Painter.setClipRegion(Event->region());
        const auto& Colors = fm::style::themeColorsFor(this);
        Painter.setPen(Colors[fm::style::Token::Line]); Painter.drawLine(0, height() - 1, width(), height() - 1);
        const auto Draw = [&](int Index) {
            QStyleOptionTab Option; initStyleOption(&Option, Index);
            Painter.drawControl(QStyle::CE_TabBarTabShape, Option);
            const bool Selected = Option.state & QStyle::State_Selected;
            const auto Lines = Option.text.split('\n');
            auto Main = font(); if (Selected) Main.setWeight(QFont::DemiBold);
            const auto Rect = PrimaryTextRect(Index);
            Painter.save(); Painter.setClipRect(Option.rect, Qt::IntersectClip); Painter.setFont(Main);
            Painter.setPen(Colors[Selected ? fm::style::Token::Fg : fm::style::Token::Fg2]);
            Painter.drawText(Rect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                QFontMetrics(Main).elidedText(Lines.first(), Qt::ElideRight, Rect.width()));
            if (Lines.size() > 1) {
                const auto Small = FilenameFont(); const QFontMetrics Metrics(Small);
                const QRect Bottom(Rect.left(), Rect.bottom() + 3, Rect.width(), Metrics.height());
                Painter.setFont(Small); Painter.setPen(Colors[fm::style::Token::Fg3]);
                Painter.drawText(Bottom, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                    Metrics.elidedText(Lines.last(), Qt::ElideRight, Bottom.width()));
            }
            if (Selected && hasFocus()) {
                QStyleOptionFocusRect Focus; Focus.initFrom(this); Focus.rect = Option.rect.adjusted(2, 2, -2, -3);
                Painter.drawPrimitive(QStyle::PE_FrameFocusRect, Focus);
            }
            Painter.restore();
        };
        for (int Index = 0; Index < count(); ++Index) if (Index != currentIndex()) Draw(Index);
        if (currentIndex() >= 0) Draw(currentIndex());
    }
}
