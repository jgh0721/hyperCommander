#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
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
    inline WidthSettings LoadWidths(QSettings& Settings, const QString& Key, const QMap<QString, int>& Legacy)
    {
        WidthSettings Result;
        for (auto It = Legacy.begin(); It != Legacy.end(); ++It) Result.columns_[It.key()].value_ = std::clamp(It.value(), 1, 1000000);
        if (!Settings.contains(Key)) return Result;
        QJsonParseError Error;
        const auto Document = QJsonDocument::fromJson(Settings.value(Key).toByteArray(), &Error);
        const auto Root = Document.object();
        if (Error.error != QJsonParseError::NoError || !Document.isObject() || Root.value("version").toInt() != 1 || !Root.value("columns").isObject())
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
    inline bool SaveWidths(QSettings& Settings, const QString& Key, const WidthSettings& Value)
    {
        if (!Value.writable_) return false;
        QJsonObject Columns;
        for (auto It = Value.columns_.begin(); It != Value.columns_.end(); ++It)
        {
            if (!Validate(It.value()).empty()) return false;
            Columns[It.key()] = EncodeWidth(It.value());
        }
        Settings.setValue(Key, QJsonDocument(QJsonObject{{"version", 1}, {"columns", Columns}}).toJson(QJsonDocument::Compact));
        Settings.sync();
        return Settings.status() == QSettings::NoError;
    }
}
