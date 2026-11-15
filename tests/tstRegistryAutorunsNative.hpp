#pragma once
#include <QObject>
class RegistryAutorunsNativeTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void approvals();
    void shellBlock();
    void linkIndex();
    void stagedIndex();
};
