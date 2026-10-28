#include "stdafx.h"
#include "compareSettings.hpp"
#include "compareCommands.hpp"
#include <QApplication>
#include <QColor>
#include <QJsonDocument>
#include <QSettings>

namespace hc::compare
{
    namespace { CompareSettings activeSettings; }
    const CompareSettings& ActiveCompareSettings() { return activeSettings; }
    void SetActiveCompareSettings(const CompareSettings& Settings) { activeSettings = Settings; }
    CompareSettings CompareSettings::Load()
    {
        CompareSettings result; QSettings settings;
        result.theme_ = settings.value("theme/mode", settings.value("theme/dark", false).toBool() ? "dark" : "product").toString();
        if (result.theme_ != "dark" && result.theme_ != "light") result.theme_ = "product";
        result.fonts_ = {qApp->font(), QFont("D2Coding", 12), QFont("D2Coding ligature", 14)};
        const char* fonts[]{"fonts/ui", "fonts/list", "fonts/text"};
        for (int i = 0; i < 3; ++i) result.fonts_[i].fromString(settings.value(fonts[i], result.fonts_[i].toString()).toString());
        for (int i = 0; i < 5; ++i) result.view_[i] = settings.value(compareViewKeys[i], true).toBool();
        const auto data = settings.value("compare/customizationV1").toByteArray();
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
                    result.linked_[i] = palette["linked"].toBool(true); result.colors_[i] = palette["colors"].toObject();
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
        QSettings settings; QMap<QString, QVariant> previous; QStringList absent;
        for (auto it = values.cbegin(); it != values.cend(); ++it)
        {
            if (settings.contains(it.key())) previous[it.key()] = settings.value(it.key()); else absent.append(it.key());
            settings.setValue(it.key(), it.value());
        }
        settings.sync();
        if (settings.status() == QSettings::NoError) return true;
        // QSettings does not promise a transaction across legacy keys. Restore the prior snapshot on failure.
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) settings.setValue(it.key(), it.value());
        for (const auto& key : absent) settings.remove(key); settings.sync();
        Error = QObject::tr("설정을 저장하지 못했습니다. 저장 위치의 쓰기 권한을 확인하십시오. 변경 내용은 이 창에 유지됩니다.");
        return false;
    }
    bool CompareSettings::operator==(const CompareSettings& Other) const
    { return theme_ == Other.theme_ && fonts_ == Other.fonts_ && view_ == Other.view_ && shortcuts_ == Other.shortcuts_ && colors_ == Other.colors_ && linked_ == Other.linked_; }
}
