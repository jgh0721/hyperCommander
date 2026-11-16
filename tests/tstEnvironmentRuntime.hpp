#pragma once
#include <QObject>
class EnvironmentRuntimeTests final : public QObject
{
    Q_OBJECT
private slots:
    void rebuildPreservesOverlayAndDeletes();
    void failedWriteKeepsPublishedGeneration();
    void childBlockIsImmutableAndTerminated();
    void pathBudgetDoesNotProbeAfterExpiry();
};
