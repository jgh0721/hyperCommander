#pragma once
#include <QtTest>
class EnvironmentCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void mergeSevenStages();
    void mergeAppendTypes();
    void sameStageReference();
    void undefinedAndCycles();
    void caseInsensitiveNames();
    void availabilityExceptions();
    void backendConflictAndTypes();
    void valueValidation();
    void pathNineChecks();
    void pathRootNormalization();
    void pathUnknownAndCancellation();
    void commandSearchContext();
    void pathextOrderAndCwd();
    void processBlockBounds();
    void processHiddenNames();
    void processPebLayouts();
    void processUnstableAndLimits();
    void secretNames();
};
