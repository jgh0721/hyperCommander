#pragma once
#include <QObject>
class RegistrySearchTests : public QObject
{
    Q_OBJECT
private slots:
    void rootAndResume();
    void typeTimeAndRegex();
    void incrementalModel();
    void mergedOrigins();
    void orderedImportPreview();
    void compareLeaseOwnership();
    void exportAndNormalize();
    void replacementConflictAndUndo();
};