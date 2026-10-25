#pragma once
#include <QByteArray>
#include <QString>
namespace hc::epub::poc {
struct ReadingAsset { QString id, error; };
class ReadingAssets final {
public:
    static ReadingAsset Import(const QString& Input, const QString& Settings);
    static ReadingAsset Install(const QByteArray& Png,const QString& Settings,const QString& ExpectedId);
    static QByteArray Load(const QString& Settings, const QString& Id, QString& Error);
};
}
