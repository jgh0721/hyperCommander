#pragma once
#include <QObject>
class EnvironmentVmTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void typedWriteRestoreThroughHelper();
    void nativeAndWow64ProcessEnvironment();
};
