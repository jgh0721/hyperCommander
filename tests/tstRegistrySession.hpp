#pragma once
#include <QObject>
class RegistrySessionTests final : public QObject
{
    Q_OBJECT
private slots:
    void orderedPrepareAndUndo();
    void subtreeLinksAndRename();
    void conflictAndBackupFailure();
    void uncertainRecoveryAndPartialUndo();
    void serializationRejectsTampering();
    void redoAndBranchAfterRestart();
    void chunkedProtocolAndIdentity();
    void authenticatedPipeAndReconnect();
    void nativeRenameAndLinkUndo();
    void uacTransition();
    void mutationAdapter();
    void editorConflict();
    void valueRenameConflict();
    void crudConflicts();
    void staleRecovery();
    void policyJournal();
    void policyCancellation();
    void policyRemoteConflict();
};
