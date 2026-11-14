#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QString>

namespace hc::sqlite
{
    // 시안(SQLite 작업 창 · 부품 보드)의 16 px 선 아이콘. 이름: db · table · view · index · trigger · folder · col · pk · fk ·
    // sql · lock · refresh · attach · collapse · more · filter · panel · export · play · playline · stop · commit ·
    // rollback · plan · format · newq · open · save · info · err · ok · pin · addrow · delrow · edit · import.
    QIcon Glyph(const QString& Name, const QColor& Color, int Size = 16);
    // Target(가족 · 굵기)은 두고 크기만 Base 보다 Points 만큼 크거나 작게. 앱 글꼴이 픽셀 크기면(fm 테마는 13 px)
    // pointSizeF() 가 -1 이므로 픽셀로 계산한다(1 pt ≈ 4/3 px).
    inline QFont SizedLike(QFont Target, const QFont& Base, qreal Points, qreal Minimum = 6.5)
    {
        if (Base.pixelSize() > 0) Target.setPixelSize(qMax(qRound(Minimum * 4.0 / 3.0), Base.pixelSize() + qRound(Points * 4.0 / 3.0)));
        else Target.setPointSizeF(qMax(Minimum, Base.pointSizeF() + Points));
        return Target;
    }
    inline QFont Resized(const QFont& Font, qreal Points) { return SizedLike(Font, Font, Points, 6.0); }
}
