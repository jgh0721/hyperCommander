#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include "settings/appSettings.hpp"
#include "commonLibs/view/columnWidthCodec.hpp"

namespace hc::view
{
    // Storage is independent of Qtitan. Unknown versions are never overwritten.
    struct WidthSettings
    {
        QMap<QString, ColumnWidthPolicy> columns_;
        bool writable_ = true;
    };
    inline WidthSettings LoadWidthPolicies(hc::settings::Repository& Settings, const QString& Key, const QMap<QString, ColumnWidthPolicy>& Defaults)
    {
        WidthSettings Result;
        Result.columns_ = Defaults;
        const auto Profiles = Settings.Read(hc::settings::Section::Columns).toObject();
        if (!Profiles.contains(Key)) return Result;
        const auto Root = Profiles[Key].toObject();
        if (!Profiles[Key].isObject() || Root.value("version").toInt() != 1 || !Root.value("columns").isObject())
        { Result.writable_ = false; return Result; }
        const auto Columns = Root.value("columns").toObject();
        for (auto It = Columns.begin(); It != Columns.end(); ++It)
        {
            const auto Policy = It.value().isObject() ? DecodeWidth(It.value().toObject()) : std::nullopt;
            if (!Policy) { Result.writable_ = false; continue; }
            Result.columns_[It.key()] = *Policy;
        }
        return Result;
    }
    inline WidthSettings LoadWidths(hc::settings::Repository& Settings, const QString& Key, const QMap<QString, int>& Defaults)
    {
        QMap<QString, ColumnWidthPolicy> Policies;
        for (auto It = Defaults.begin(); It != Defaults.end(); ++It) Policies[It.key()].value_ = std::clamp(It.value(), 1, 1000000);
        return LoadWidthPolicies(Settings, Key, Policies);
    }
    inline bool SaveWidths(hc::settings::Repository& Settings, const QString& Key, const WidthSettings& Value)
    {
        if (!Value.writable_) return false;
        QJsonObject Columns;
        for (auto It = Value.columns_.begin(); It != Value.columns_.end(); ++It)
        {
            if (!Validate(It.value()).empty()) return false;
            Columns[It.key()] = EncodeWidth(It.value());
        }
        auto Profiles = Settings.Read(hc::settings::Section::Columns).toObject();
        auto Profile = Profiles[Key].toObject();
        Profile["version"] = 1; Profile["columns"] = Columns;
        Profiles[Key] = Profile;
        return Settings.Put(hc::settings::Section::Columns, Profiles);
    }
}
