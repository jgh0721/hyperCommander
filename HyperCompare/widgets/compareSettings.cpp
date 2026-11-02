#include "stdafx.h"
#include "compareSettings.hpp"
#include "compareCommands.hpp"
#include <QApplication>
#include <QColor>
#include <QJsonDocument>
#include <QSettings>
#include <QMainWindow>
#include <QStatusBar>
#include <QDebug>
#include "UniqueLibs/settings/appSettings.hpp"

namespace hc::compare
{
    namespace
    {
        CompareSettings activeSettings;
        QJsonValue PreferenceJson(const QString& Key, const QVariant& Value)
        {
            // Keep shortcut/color customization readable JSON instead of a base64 byte array.
            if (Key == "compare/customizationV1" && Value.metaType().id() == QMetaType::QByteArray) {
                const auto Document = QJsonDocument::fromJson(Value.toByteArray());
                if (Document.isObject()) return Document.object();
            }
            return hc::settings::UnifiedConfig::Encode(Value);
        }
    }
    QString Preferences::FileName()
    {
        const auto Override = qApp->property("compareSettingsDirectory");
        return hc::defaults::compare::SettingsFile(Override.isValid() ? Override.toString() : hc::defaults::compare::ConfigDirectory());
    }
    Preferences::Preferences() : store_(std::make_unique<hc::settings::JsonStore>(FileName())) { reload(); }
    Preferences::~Preferences() = default;
    bool Preferences::finish(bool Success, const QString& Error)
    {
        error_ = Success ? QString{} : Error;
        if (!Success) {
            qWarning().noquote() << error_;
            for (auto* Widget : QApplication::topLevelWidgets()) if (auto* Window = qobject_cast<QMainWindow*>(Widget))
                Window->statusBar()->showMessage(QObject::tr("설정을 저장하거나 읽지 못했습니다: %1").arg(error_));
        }
        return Success;
    }
    bool Preferences::reload()
    {
        if (!store_->Reload()) return finish(false, store_->Error());
        // The old registry is only an import source. Never write or remove its values.
        if (!store_->Exists() && !qApp->property("compareSettingsDirectory").isValid()) {
            QSettings Legacy(QSettings::NativeFormat, QSettings::UserScope, hc::defaults::compare::LegacyOrganization, hc::defaults::compare::LegacyApplication);
            Legacy.setFallbacksEnabled(false);
            return ImportLegacy(Legacy);
        }
        return finish(true);
    }
    bool Preferences::ImportLegacy(QSettings& Legacy)
    {
        if (!store_->Writable()) return finish(false, store_->Error());
        if (store_->Exists()) return finish(true);
        auto Document = store_->Object();
        const auto Keys = Legacy.allKeys();
        for (const auto& Key : Keys)
            hc::settings::UnifiedConfig::Set(Document, Key.split('/'), PreferenceJson(Key, Legacy.value(Key)));
        if (Legacy.status() != QSettings::NoError) return finish(false, QObject::tr("기존 Compare 설정을 읽지 못했습니다."));
        if (Keys.isEmpty()) return finish(true);
        const auto Saved = store_->Replace(Document);
        return finish(Saved, store_->Error());
    }
    QVariant Preferences::Value(const QString& Key)
    {
        // A settings object may outlive another window's save (including Apply/Cancel dialogs).
        if (!reload()) return hc::defaults::compare::Value(Key);
        const auto Value = hc::settings::UnifiedConfig::Value(store_->Object(), Key.split('/'));
        if (Value.isUndefined()) return hc::defaults::compare::Value(Key);
        if (Key == "compare/customizationV1" && Value.isObject() && !Value.toObject().contains("$type"))
            return QJsonDocument(Value.toObject()).toJson(QJsonDocument::Compact);
        return hc::settings::UnifiedConfig::Decode(Value);
    }
    bool Preferences::Contains(const QString& Key)
    { return reload() && !hc::settings::UnifiedConfig::Value(store_->Object(), Key.split('/')).isUndefined(); }
    bool Preferences::SetValue(const QString& Key, const QVariant& Value) { return Apply({{Key, Value}}); }
    bool Preferences::Remove(const QString& Key) { return Apply({}, {Key}); }
    bool Preferences::Apply(const QMap<QString, QVariant>& Values, const QStringList& Removed)
    {
        // Refresh before each transaction so another window/process's unrelated settings survive.
        if (!reload()) return false;
        auto Document = store_->Object();
        for (auto It = Values.cbegin(); It != Values.cend(); ++It)
            hc::settings::UnifiedConfig::Set(Document, It.key().split('/'), PreferenceJson(It.key(), It.value()));
        for (const auto& Key : Removed)
            hc::settings::UnifiedConfig::Set(Document, Key.split('/'), QJsonValue::Undefined);
        const auto Saved = store_->Replace(Document);
        return finish(Saved, store_->Error());
    }
    const CompareSettings& ActiveCompareSettings() { return activeSettings; }
    void SetActiveCompareSettings(const CompareSettings& Settings) { activeSettings = Settings; }
    CompareSettings CompareSettings::Load()
    {
        CompareSettings result; Preferences settings;
        if (!settings.Error().isEmpty()) result.warnings_.append(settings.Error());
        result.theme_ = hc::defaults::compare::ThemeMode(settings.Contains("theme/mode") ? QJsonValue(settings.Value("theme/mode").toString()) : QJsonValue(QJsonValue::Undefined), QJsonValue(settings.Value("theme/dark").toBool()));
        result.fonts_ = hc::defaults::compare::Fonts(qApp->font());
        const char* fonts[]{"fonts/ui", "fonts/list", "fonts/text"};
        for (int i = 0; i < 3; ++i) result.fonts_[i].fromString(settings.Value(fonts[i]).toString());
        for (int i = 0; i < 5; ++i) result.view_[i] = settings.Value(compareViewKeys[i]).toBool();
        const auto data = settings.Value("compare/customizationV1").toByteArray();
        if (!data.isEmpty())
        {
            QJsonParseError error; const auto document = QJsonDocument::fromJson(data, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject() || document.object()["version"].toInt() != 1)
                result.warnings_.append(QObject::tr("단축키·색상 설정 형식을 읽을 수 없어 기본값을 사용합니다."));
            else
            {
                const auto object = document.object(); result.shortcuts_ = object["shortcuts"].toObject();
                for (int i = 0; i < 2; ++i)
                {
                    const auto palette = object[i ? "dark" : "light"].toObject();
                    result.linked_[i] = palette["linked"].toBool(hc::defaults::compare::LinkColors); result.colors_[i] = palette["colors"].toObject();
                    for (const auto& key : result.colors_[i].keys())
                    {
                        const auto value = result.colors_[i][key];
                        const bool valid = key.endsWith("Alpha") ? value.isDouble() && value.toDouble() == value.toInt(-1) && value.toInt() >= 0 && value.toInt() <= 255 :
                            value.isString() && QColor(value.toString()).isValid();
                        if (!valid) { result.colors_[i].remove(key); result.warnings_.append(QObject::tr("잘못된 색상 값을 복원했습니다: %1").arg(key)); }
                    }
                }
                // Reject invalid overrides individually, then validate the final map. Unknown IDs are retained.
                for (const auto& command : CompareCommands()) if (result.shortcuts_.contains(command.id_))
                {
                    const auto value = result.shortcuts_[command.id_]; const auto key = QKeySequence::fromString(value.toString(), QKeySequence::PortableText);
                    if (!value.isString() || (!value.toString().isEmpty() && (!CompareKeyError(key).isEmpty() || key.isEmpty())))
                    { result.shortcuts_.remove(command.id_); result.warnings_.append(QObject::tr("잘못된 단축키를 복원했습니다: %1").arg(command.label_)); }
                }
                if (!ValidateCompareShortcuts(result.shortcuts_).isEmpty())
                {
                    // Restoring a default can reveal another collision; keep unrelated user bindings intact.
                    for (int pass = 0; pass < CompareCommands().size(); ++pass)
                    {
                        QMap<QKeySequence, QString> owners; QStringList remove;
                        for (const auto& command : CompareCommands()) for (const auto& key : CompareKeys(command.id_, result.shortcuts_))
                        {
                            if (key.isEmpty()) continue;
                            if (owners.contains(key) && owners[key] != command.id_)
                                for (const auto& id : {owners[key], command.id_}) if (result.shortcuts_.contains(id)) remove.append(id);
                            owners[key] = command.id_;
                        }
                        if (remove.isEmpty()) break;
                        for (const auto& id : remove) result.shortcuts_.remove(id);
                    }
                    result.warnings_.append(QObject::tr("충돌한 단축키만 기본값으로 복원했습니다. 나머지 사용자 지정은 유지됩니다."));
                }
            }
        }
        return result;
    }
    bool CompareSettings::Save(QString& Error) const
    {
        Error = ValidateCompareShortcuts(shortcuts_); if (!Error.isEmpty()) return false;
        QJsonObject object{{"version", 1}, {"shortcuts", shortcuts_}};
        for (int i = 0; i < 2; ++i) object[i ? "dark" : "light"] = QJsonObject{{"linked", linked_[i]}, {"colors", colors_[i]}};
        QMap<QString, QVariant> values{{"compare/customizationV1", QJsonDocument(object).toJson(QJsonDocument::Compact)},
            {"theme/mode", theme_}, {"theme/dark", theme_ == "dark"}, {"fonts/ui", fonts_[0].toString()},
            {"fonts/list", fonts_[1].toString()}, {"fonts/text", fonts_[2].toString()}};
        for (int i = 0; i < 5; ++i) values[compareViewKeys[i]] = view_[i];
        Preferences Settings;
        if (Settings.Apply(values)) return true;
        Error = QObject::tr("설정을 저장하지 못했습니다. 변경 내용은 이 창에 유지됩니다.\n%1").arg(Settings.Error());
        return false;
    }
    bool CompareSettings::operator==(const CompareSettings& Other) const
    { return theme_ == Other.theme_ && fonts_ == Other.fonts_ && view_ == Other.view_ && shortcuts_ == Other.shortcuts_ && colors_ == Other.colors_ && linked_ == Other.linked_; }
}
