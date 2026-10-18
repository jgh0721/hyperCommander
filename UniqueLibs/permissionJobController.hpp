#pragma once
#include <QObject>
#include <QCoreApplication>
#include <QVariant>
#include <QJsonObject>
namespace hc::permissions {
class JobController {
public:
    virtual ~JobController()=default;
    virtual QString SubmitSecurityJob(const QString& Path,bool Grant,const QJsonObject& Restore={})=0;
};
inline JobController* Controller() {
    const auto Object=QCoreApplication::instance()->property("hc.permission.controller").value<QObject*>();
    return dynamic_cast<JobController*>(Object);
}
}
