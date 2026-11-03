#pragma once
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include <QSettings>
#include <QCoreApplication>
#include <QDateTime>
#include "UniqueLibs/settings/appSettings.hpp"
// Compatibility surface for the adopted views. Writes use the product's single
// JSON transaction rather than QSettings' delayed, independent INI writer.
class AppSettings : public QObject {
public:
    explicit AppSettings(QObject* Parent = nullptr) : QObject(Parent)
    {
        const auto Override = QCoreApplication::instance()->property("documentViewsConfigDir");
        path_ = hc::settings::UnifiedConfig::FileName(Override.isValid() ? Override.toString() : hc::settings::DocumentPaths().directory_);
        valid_ = hc::settings::UnifiedConfig::Load(path_, base_, error_); draft_ = base_;
    }
    ~AppSettings() override { sync(); }
    QVariant value(const QString& Key) const
    {
        const auto Value = hc::settings::UnifiedConfig::Value(draft_["documentViews"].toObject(), Key.split('/'));
        return Value.isUndefined() ? hc::defaults::edit::ViewValue(Key) : hc::settings::UnifiedConfig::Decode(Value);
    }
    bool contains(const QString& Key) const
    {
        return !hc::settings::UnifiedConfig::Value(draft_["documentViews"].toObject(), Key.split('/')).isUndefined();
    }
    void setValue(const QString& Key, const QVariant& Value)
    {
        auto Views = draft_["documentViews"].toObject();
        hc::settings::UnifiedConfig::Set(Views, Key.split('/'), hc::settings::UnifiedConfig::Encode(Value));
        draft_["documentViews"] = Views; dirty_ = true;
    }
    // Drops a stored key; value() then falls back to the product default.
    // The unified-config merge applies the removal to the file on sync().
    void remove(const QString& Key)
    {
        auto Views = draft_["documentViews"].toObject();
        if (!removePath(Views, Key.split('/'))) return;
        draft_["documentViews"] = Views; dirty_ = true;
    }
    void sync()
    {
        if (!dirty_) return;
        QJsonObject Saved;
        if (!valid_ || !hc::settings::UnifiedConfig::Save(path_, base_, draft_, Saved, error_)) {
            QCoreApplication::instance()->setProperty("documentSettingsError", path_ + ": " + error_); return;
        }
        base_ = draft_ = Saved; dirty_ = false;
        QCoreApplication::instance()->setProperty("documentSettingsError", QVariant());
        QCoreApplication::instance()->setProperty("documentSettingsRevision", QDateTime::currentMSecsSinceEpoch());
    }
    QSettings::Status status() const { return error_.isEmpty() ? QSettings::NoError : QSettings::AccessError; }
    QString fileName() const { return path_; }
private:
    static bool removePath(QJsonObject& Object, const QStringList& Path)
    {
        if (Path.isEmpty() || !Object.contains(Path.first())) return false;
        if (Path.size() == 1) { Object.remove(Path.first()); return true; }
        auto Child = Object[Path.first()].toObject();
        if (!removePath(Child, Path.mid(1))) return false;
        Object[Path.first()] = Child; return true;
    }
    QString path_, error_;
    QJsonObject base_, draft_;
    bool dirty_ = false, valid_ = false;
};
