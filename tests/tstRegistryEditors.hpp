#pragma once
#include <QObject>
class RegistryEditorTests final : public QObject
{
    Q_OBJECT
private slots:
    void textDraftAndSave();
    void policyOptions();
    void malformedPreservation();
    void numbersAndBinary();
    void pathsAndCancellation();
};
