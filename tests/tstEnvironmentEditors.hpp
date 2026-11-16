#pragma once
#include <QObject>
class EnvironmentEditorTests final : public QObject
{
    Q_OBJECT
private slots:
    void draftAndReadOnly();
    void variantsAndMask();
    void pathKeyboardAndMoves();
    void folderProbeIsInjected();
};
