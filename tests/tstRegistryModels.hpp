#pragma once
#include <QObject>
class RegistryModelsTests final : public QObject
{
    Q_OBJECT
private slots:
    void registryLinks();
    void physicalClasses();
    void linkCacheResume();
    void autorunApprovals();
    void autorunGuardedToggle();
    void groupedResults();
    void policyScan();
    void lazySharedCache();
    void refreshPreservesIndexes();
    void rootProxyAndBitView();
    void closePending();
    void descendingKeysFirst();
    void nativeWatchRefresh();
    void navigationAndRestore();
    void failedHistoryNavigation();
    void flatRowsAndFilter();
    void sharedFavorites();
    void historyCandidates();
    void historyRecovery();
};
