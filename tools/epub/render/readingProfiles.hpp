#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
namespace hc::epub::poc {
struct ReadingProfileResult {
    QJsonArray profiles;
    QJsonObject profile;
    QString name,error;
    explicit operator bool() const { return error.isEmpty(); }
};
class ReadingProfiles final {
public:
    static ReadingProfileResult List(const QString& Settings);
    static ReadingProfileResult Save(const QString& Settings,const QString& Name,const QJsonObject& Profile,bool Overwrite=false);
    static ReadingProfileResult Remove(const QString& Settings,const QString& Name);
    static ReadingProfileResult Export(const QString& Settings,const QString& Destination,const QString& Name,const QJsonObject& Profile);
    static ReadingProfileResult Import(const QString& Settings,const QString& Input,bool Overwrite=false);
};
}
