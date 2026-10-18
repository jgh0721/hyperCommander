#include "core/Watermark.h"

#include <QJsonObject>
#include <QJsonDocument>

namespace Watermark {

QByteArray toJson(const Config& cfg)
{
    QJsonObject obj;
    obj["enabled"]      = cfg.enabled;
    obj["forced"]       = cfg.forced;
    obj["text"]         = cfg.text;
    obj["opacity"]      = cfg.opacity;
    obj["rotation"]     = cfg.rotation;
    obj["posX"]         = cfg.relativePos.x();
    obj["posY"]         = cfg.relativePos.y();
    obj["sizeW"]        = cfg.relativeSize.width();
    obj["sizeH"]        = cfg.relativeSize.height();
    obj["alignment"]    = static_cast<int>(cfg.alignment);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

Config fromJson(const QByteArray& data)
{
    Config cfg;
    auto obj = QJsonDocument::fromJson(data).object();
    cfg.enabled     = obj["enabled"].toBool();
    cfg.forced      = obj["forced"].toBool();
    cfg.text        = obj["text"].toString();
    cfg.opacity     = obj["opacity"].toDouble(0.3);
    cfg.rotation    = obj["rotation"].toDouble(-45.0);
    cfg.relativePos = {obj["posX"].toDouble(0.5), obj["posY"].toDouble(0.5)};
    cfg.relativeSize= {obj["sizeW"].toDouble(0.5), obj["sizeH"].toDouble(0.5)};
    cfg.alignment   = static_cast<Qt::Alignment>(obj["alignment"].toInt(Qt::AlignCenter));
    return cfg;
}

} // namespace Watermark

