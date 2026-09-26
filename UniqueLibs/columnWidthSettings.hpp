#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include "settings/appSettings.hpp"
#include "commonLibs/view/columnWidthPolicy.hpp"

namespace hc::view
{
    // Storage is independent of Qtitan. Unknown versions are never overwritten.
    inline QJsonObject EncodeWidth(const ColumnWidthPolicy& Policy)
    {
        QJsonObject Object{{"unit", Policy.unit_ == WidthUnit::Percent ? "percent" : "pixels"},
            {"mode", Policy.mode_ == WidthMode::Fixed ? "fixed" : "flexible"}, {"value", Policy.value_}, {"hidden", Policy.hidden_}};
        if (Policy.minimum_) Object["minimum"] = *Policy.minimum_;
        if (Policy.maximum_) Object["maximum"] = *Policy.maximum_;
        return Object;
    }
    inline std::optional<ColumnWidthPolicy> DecodeWidth(const QJsonObject& Object)
    {
        const auto Unit = Object.value("unit").toString();
        const auto Mode = Object.value("mode").toString();
        if ((Unit != "pixels" && Unit != "percent") || (Mode != "fixed" && Mode != "flexible") || !Object.value("value").isDouble()) return {};
        if (Object.contains("hidden") && !Object.value("hidden").isBool()) return {};
        ColumnWidthPolicy Policy;
        Policy.unit_ = Unit == "percent" ? WidthUnit::Percent : WidthUnit::Pixels;
        Policy.mode_ = Mode == "fixed" ? WidthMode::Fixed : WidthMode::Flexible;
        Policy.value_ = Object.value("value").toDouble();
        Policy.hidden_ = Object.value("hidden").toBool();
        for (const auto& Key : {QStringLiteral("minimum"), QStringLiteral("maximum")})
        {
            if (!Object.contains(Key)) continue;
            if (!Object.value(Key).isDouble()) return {};
            (Key == "minimum" ? Policy.minimum_ : Policy.maximum_) = Object.value(Key).toDouble();
        }
        return Validate(Policy).empty() ? std::optional(Policy) : std::nullopt;
    }
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
        Profiles[Key] = QJsonObject{{"version", 1}, {"columns", Columns}};
        return Settings.Put(hc::settings::Section::Columns, Profiles);
    }
}
