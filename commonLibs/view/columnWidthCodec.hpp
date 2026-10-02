#pragma once
#include <QJsonObject>
#include "columnWidthPolicy.hpp"

namespace hc::view
{
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
}
