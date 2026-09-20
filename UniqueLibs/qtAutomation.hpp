#pragma once
#include <QString>
#include <QList>
#include "commonLibs/commands/automation.hpp"
class QWidget;
class QDialog;
class QMainWindow;
class QSettings;
namespace hc::automation {
struct Diagnostic { qsizetype line_ = 0, column_ = 0; QString message_; };
storage::Result<Program> Compile(const QString& Source, Diagnostic* Diagnostic = nullptr, SchemaLookup Schema = {});
struct SavedScript { QString id_, title_, source_, shortcut_; bool toolbar_ = false; QString language_ = "dsl"; };
storage::Result<QList<SavedScript>> DecodeLibrary(const QByteArray& Bytes);
storage::Result<QByteArray> EncodeLibrary(const QList<SavedScript>& Scripts);
class LibraryStore {
public:
    explicit LibraryStore(QSettings& Settings) : settings_(Settings) {}
    storage::Result<QList<SavedScript>> Load();
    storage::Result<storage::Unit> Save(const QList<SavedScript>& Scripts);
private:
    QSettings& settings_;
    QByteArray original_;
    bool exists_ = false, loaded_ = false;
};
QDialog* Show(QWidget* Parent, std::function<Host()> MakeHost, QSettings* Settings = nullptr,
    QString Id = {}, bool Run = false, std::function<void()> Changed = {});
void Install(QMainWindow* Window, QSettings& Settings, std::function<Host()> MakeHost);
}
