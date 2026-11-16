#pragma once
#include "commonLibs/system/processEnvironment.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <functional>
namespace hc::environment {
class ProcessEnvironmentServer final
{
public:
    using Reader = std::function<ProcessEnvironment(const hc::system::ProcessKey&)>;
    explicit ProcessEnvironmentServer(Reader Read = ReadProcessEnvironment) : read_(std::move(Read)) {}
    ~ProcessEnvironmentServer();
    QJsonObject Handle(const QString& Kind, const QJsonObject& Request);
private:
    void clear();
    Reader read_;
    QByteArray bytes_, hash_;
    QString id_;
    qint64 touched_ = 0;
};
using EnvironmentExchange = std::function<QJsonObject(const QString&, QJsonObject)>;
ProcessEnvironment ReadRemoteEnvironment(const hc::system::ProcessKey& Target, const EnvironmentExchange& Exchange);
}
