#pragma once
#include <QObject>
class EnvironmentProcessTests final : public QObject
{
    Q_OBJECT
private slots:
    void chunkedSnapshot();
    void rejectsCorruptSnapshot();
    void rejectsInvalidIdentity();
    void propagatesReadFailure();
};
