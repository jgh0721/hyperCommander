#include "stdafx.h"
#include "sqliteGlyphs.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace hc::sqlite
{
    namespace
    {
        // 시안 SqlWindow.dc.html 의 ICON 표와 같은 경로(16 × 16, 선 1.3).
        const QHash<QString, const char*>& Paths()
        {
            static const QHash<QString, const char*> Table{
                {"newq", "M3.5 1.5h5.5l3 3v3.5 M9 1.5v3h3 M3.5 1.5v13h5 M12 10v5 M9.5 12.5h5"},
                {"open", "M1.5 13V3.5h4l1.5 1.5h6V7 M1.5 13l2-6h11l-2 6z"},
                {"save", "M2.5 2.5h9l2 2v9h-11z M5 2.5V6h5V2.5 M5 13.5V9.5h6v4"},
                {"play", "M4.5 2.5v11l9-5.5z"},
                {"playline", "M1.5 3.5h5 M1.5 7.5h5 M1.5 11.5h5 M9 4v8l5.5-4z"},
                {"stop", "M3.5 3.5h9v9h-9z"},
                {"commit", "M2.5 8.5l3.5 3.5 7.5-8"},
                {"rollback", "M5.5 3.5l-3 3 3 3 M2.5 6.5h7a3.5 3.5 0 0 1 0 7H7"},
                {"plan", "M3.5 2v11.5 M3.5 4.5h3.5 M3.5 11h3.5 M8.5 2.5h5v4h-5z M8.5 9h5v4h-5z"},
                {"format", "M2.5 3.5h11 M5.5 6.5h8 M5.5 9.5h8 M2.5 12.5h11"},
                {"import", "M8 1.5v8.5 M4.5 6.5L8 10l3.5-3.5 M2.5 10.5v3h11v-3"},
                {"export", "M8 10V1.5 M4.5 5L8 1.5 11.5 5 M2.5 10.5v3h11v-3"},
                {"edit", "M2.5 13.5l1-3.5 7.5-7.5 2.5 2.5-7.5 7.5z M9.5 4l2.5 2.5"},
                {"info", "M8 1.5a6.5 6.5 0 1 1 0 13a6.5 6.5 0 1 1 0-13z M8 7v4.5 M8 4.6v.3"},
                {"refresh", "M13 8a5 5 0 1 1-1.5-3.5 M11.5 1.5v3h-3"},
                {"attach", "M10.5 4.5L5 10a1.8 1.8 0 0 0 2.5 2.5l6-6a3.2 3.2 0 0 0-4.5-4.5l-6 6a4.6 4.6 0 0 0 6.5 6.5L14 10"},
                {"collapse", "M2.5 2.5h11v11h-11z M5.5 8h5"},
                {"more", "M3 7.2a.8.8 0 1 0 0 1.6a.8.8 0 1 0 0-1.6z M8 7.2a.8.8 0 1 0 0 1.6a.8.8 0 1 0 0-1.6z M13 7.2a.8.8 0 1 0 0 1.6a.8.8 0 1 0 0-1.6z"},
                {"filter", "M2 3h12l-4.5 5.5v4.5l-3 1.5V8.5z"},
                {"panel", "M2.5 2.5h11v11h-11z M9.5 2.5v11"},
                {"pin", "M5.5 1.5h5 M6.5 1.5v5l-2 2h7l-2-2v-5 M8 8.5v6"},
                {"addrow", "M2.5 2.5h11v6h-11z M2.5 5.5h11 M11.5 10v5 M9 12.5h5"},
                {"delrow", "M2.5 2.5h11v6h-11z M2.5 5.5h11 M9 12.5h5"},
                {"db", "M3 3.5c0-1.1 2.2-2 5-2s5 .9 5 2v9c0 1.1-2.2 2-5 2s-5-.9-5-2z M3 3.5c0 1.1 2.2 2 5 2s5-.9 5-2 M3 8c0 1.1 2.2 2 5 2s5-.9 5-2"},
                {"table", "M2.5 3h11v10h-11z M2.5 6.3h11 M6.5 6.3V13"},
                {"view", "M1.5 8s2.5-4.5 6.5-4.5S14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z M8 6.3a1.7 1.7 0 1 1 0 3.4a1.7 1.7 0 1 1 0-3.4z"},
                {"index", "M2.5 3.5h11 M2.5 6.5h8 M2.5 9.5h5 M2.5 12.5h2.5"},
                {"trigger", "M9 1.5L3.5 9H8l-1 5.5L12.5 7H8z"},
                {"folder", "M1.5 13V3.5h4l1.5 1.5h7.5V13z"},
                {"col", "M5.5 2.5h5v11h-5z"},
                {"pk", "M5.5 3.5a3 3 0 1 1 0 6a3 3 0 1 1 0-6z M7.8 8.6l5.7 5.4 M11 11.2l1.6-1.6 M12.7 12.8l1.3-1.3"},
                {"fk", "M6.5 9.5l3-3 M7 4.5l1.5-1.5a2.5 2.5 0 0 1 3.5 3.5L10.5 8 M9 11.5L7.5 13a2.5 2.5 0 0 1-3.5-3.5L5.5 8"},
                {"sql", "M4 1.5h5.5l3 3v10h-8.5z M9.5 1.5v3h3 M6 8h4.5 M6 10.5h4.5"},
                {"eyeoff", "M1.5 8s2.5-4.5 6.5-4.5S14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z M2.5 2.5l11 11"},
                {"lock", "M3.5 7.5h9v6.5h-9z M5.5 7.5V5a2.5 2.5 0 0 1 5 0v2.5"},
                {"err", "M8 1.5a6.5 6.5 0 1 1 0 13a6.5 6.5 0 1 1 0-13z M5.5 5.5l5 5 M10.5 5.5l-5 5"},
                {"ok", "M8 1.5a6.5 6.5 0 1 1 0 13a6.5 6.5 0 1 1 0-13z M5 8.2l2 2 4-4.2"},
                {"warn", "M8 1.8l6.5 12h-13z M8 6.5v3.5 M8 12v.4"},
                {"search", "M7 2.5a4.5 4.5 0 1 1 0 9a4.5 4.5 0 1 1 0-9z M10.3 10.3l3.2 3.2"},
                // 자동 완성 목록(시안 ‘부품 · 자동 완성’): CTE 는 SQL 문서 모양, 함수 · 키워드는 이 둘을 더했다.
                {"cte", "M4 1.5h5.5l3 3v10h-8.5z M9.5 1.5v3h3 M6 8h4.5 M6 10.5h4.5"},
                {"fn", "M11.5 2.5c-1.8-.4-3 .3-3.4 2.2L6.6 11.8c-.4 1.9-1.6 2.6-3.4 2.2 M5 6.5h6"},
                {"kw", "M2.5 4.5h11v7h-11z M5 8h6"}};
            return Table;
        }
    }

    QIcon Glyph(const QString& Name, const QColor& Color, int Size)
    {
        const auto* Path = Paths().value(Name, nullptr);
        if (!Path) return {};
        const auto Svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16' fill='none' stroke='%1' "
                                        "stroke-width='1.3' stroke-linecap='round' stroke-linejoin='round'><path d='%2'/></svg>")
            .arg(Color.name(QColor::HexRgb), QLatin1String(Path));
        QSvgRenderer Renderer(Svg.toUtf8());
        QIcon Icon;
        for (const qreal Scale : {1.0, 1.5, 2.0}) {
            QPixmap Pixmap(QSize(Size, Size) * Scale);
            Pixmap.fill(Qt::transparent);
            Pixmap.setDevicePixelRatio(Scale);
            QPainter Painter(&Pixmap);
            Painter.setRenderHint(QPainter::Antialiasing);
            Renderer.render(&Painter, QRectF(0, 0, Size, Size));
            Painter.end();
            Icon.addPixmap(Pixmap);
        }
        return Icon;
    }
}
