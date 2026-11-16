#pragma once
#include <QObject>
class EnvironmentStorageTests final : public QObject
{
    Q_OBJECT
private slots:
    void sessionRefreshAndDeletion();
    void transferValidationAndRedaction();
    void modelMasksDerivedValues();
    void environmentScopeIsolation();
    void environmentProtectedKeys();
    void environmentFunctionBar();
    void profileLifecycle();
    void activeProfileUpdate();
    void profileConflictAndPersistence();
    void runtimeOverlayAndDelete();
    void typedReadAndConflict();
    void failedRenameRestoresDestination();
    void durableUndoRedo();
    void recoverOnlyCallerSideWrites();
    void protectedHistory();
    void derivedProtectedHistory();
    void missingUserKeyAndUndo();
    void systemRequiresHelper();
    void mixedScopeUsesCaller();
};
