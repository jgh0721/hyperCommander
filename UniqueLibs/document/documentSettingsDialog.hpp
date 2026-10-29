#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QMap>
class QAction;
class QLabel;
class QWidget;
namespace hc::epub::poc {class ReadingSettings;}
namespace hc::document {
class DocumentSettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit DocumentSettingsDialog(QString Path, const QList<QAction*>& Actions, QWidget* Parent = nullptr);
    bool Apply(bool ResolveConflicts = false);
signals:
    void PreferencesSaved();
    void ShortcutsValidated();
private:
    void loadControls();
    void validateShortcuts();
    QJsonObject values() const;
    QString path_;
    QJsonObject base_, initial_, imported_;
    QMap<QString, QWidget*> controls_;
    QMap<QString, QAction*> actions_;
    QLabel* error_;
    bool valid_ = false;
    hc::epub::poc::ReadingSettings* epubSettings_=nullptr;
};
}
