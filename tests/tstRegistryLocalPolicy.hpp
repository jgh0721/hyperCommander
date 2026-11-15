#pragma once
#include <QObject>
class RegistryLocalPolicyTests final : public QObject
{
    Q_OBJECT
private slots:
    void nativeRoundtrip();
};
