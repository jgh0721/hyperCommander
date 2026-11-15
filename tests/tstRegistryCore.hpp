#pragma once
#include <QObject>
class RegistryCoreTests final : public QObject
{
    Q_OBJECT
private slots:
    void addresses();
    void queryInfoAcl();
    void rawTypesAndMutation();
    void regFileRoundtrip();
    void regFileErrors();
    void policyRoundtripAndTruncation();
    void watchAndCancel();
    void viewsLinksAndConcurrentDeletion();
    void machineViewsInVm();
    void regeditCompatibility();
    void editorValueCodec();
    void environmentExpansion();
};
