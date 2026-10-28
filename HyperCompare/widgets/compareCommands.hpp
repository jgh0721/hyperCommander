#pragma once
#include <QJsonObject>
#include <QKeySequence>
#include <QList>
#include <QString>
class QObject;
class QWidget;
class QKeyEvent;
namespace hc::compare
{
    struct CompareCommand { QString id_, label_, category_, scope_; QList<QKeySequence> defaults_; };
    QList<CompareCommand> CompareCommands();
    QList<QKeySequence> CompareKeys(const QString& Id, const QJsonObject& Overrides);
    QString CompareKeyError(const QKeySequence& Key);
    QString ValidateCompareShortcuts(const QJsonObject& Overrides);
    void BindCompareCommand(QObject* Object, const QString& Id);
    void ApplyCompareShortcuts(QObject* Root);
    bool HandleCompareEditorKey(QWidget* Editor, QKeyEvent* Event, bool Execute);
}
