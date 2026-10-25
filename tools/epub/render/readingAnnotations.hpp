#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
namespace hc::epub::poc {
struct ReadingAnnotationTransfer {
    QJsonArray annotations;
    QString book,title,error;
    explicit operator bool() const { return error.isEmpty(); }
};
class ReadingAnnotations final {
public:
    static bool Valid(const QJsonObject& Annotation);
    static bool Valid(const QJsonArray& Annotations);
    static QString Export(const QString& Destination,const QString& Book,const QString& Title,const QJsonArray& Annotations);
    static ReadingAnnotationTransfer Import(const QString& Input);
};
}
