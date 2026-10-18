#pragma once

#include <QString>
#include <QPixmap>
#include <QPointF>
#include <QSizeF>

/// 워터마크 설정을 직렬화/역직렬화하기 위한 유틸리티
namespace Watermark {

struct Config
{
    bool            enabled      = false;
    bool            forced       = false;
    QString         text;
    QPixmap         image;
    qreal           opacity      = 0.3;
    qreal           rotation     = -45.0;
    QPointF         relativePos  {0.5, 0.5};
    QSizeF          relativeSize {0.5, 0.5};
    Qt::Alignment   alignment    = Qt::AlignCenter;
};

/// JSON 직렬화 (이미지 제외)
QByteArray  toJson(const Config& cfg);
Config      fromJson(const QByteArray& data);

} // namespace Watermark

