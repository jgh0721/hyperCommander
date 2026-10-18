#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace hc::view
{
    enum class WidthUnit { Pixels, Percent };
    enum class WidthMode { Flexible, Fixed };
    struct ColumnWidthPolicy
    {
        WidthUnit unit_ = WidthUnit::Pixels;
        WidthMode mode_ = WidthMode::Flexible;
        double value_ = 100;
        std::optional<double> minimum_;
        std::optional<double> maximum_;
        bool hidden_ = false;
        bool operator==(const ColumnWidthPolicy&) const = default;
    };
    struct ColumnWidthResult { int width_; int minimum_; int maximum_; };
    inline std::string Validate(const ColumnWidthPolicy& Policy)
    {
        if (Policy.unit_ != WidthUnit::Pixels && Policy.unit_ != WidthUnit::Percent) return "unit";
        if (Policy.mode_ != WidthMode::Flexible && Policy.mode_ != WidthMode::Fixed) return "mode";
        const double Limit = Policy.unit_ == WidthUnit::Percent ? 100 : 1000000;
        const auto Valid = [Limit](double Value) { return std::isfinite(Value) && Value >= 0 && Value <= Limit; };
        if (!Valid(Policy.value_) || (Policy.minimum_ && !Valid(*Policy.minimum_)) || (Policy.maximum_ && !Valid(*Policy.maximum_))) return "range";
        const auto Minimum = Policy.minimum_.value_or(0);
        const auto Maximum = Policy.maximum_.value_or(Limit);
        if (Minimum > Maximum) return "minimum-maximum";
        if (Policy.mode_ == WidthMode::Fixed && (Policy.value_ < Minimum || Policy.value_ > Maximum)) return "fixed-bounds";
        return {};
    }
    inline std::optional<ColumnWidthResult> Resolve(const ColumnWidthPolicy& Policy, int Available)
    {
        if (Available <= 0 || !Validate(Policy).empty()) return {};
        const double Scale = Policy.unit_ == WidthUnit::Percent ? Available / 100.0 : 1.0;
        const auto Pixels = [Scale](double Value) { return std::max(1, static_cast<int>(std::min(1000000.0, std::round(Value * Scale)))); };
        const auto Minimum = Pixels(Policy.minimum_.value_or(0));
        const auto Maximum = Pixels(Policy.maximum_.value_or(Policy.unit_ == WidthUnit::Percent ? 100 : 1000000));
        const auto Width = std::clamp(Pixels(Policy.value_), Minimum, Maximum);
        return Policy.mode_ == WidthMode::Fixed ? ColumnWidthResult{Width, Width, Width} : ColumnWidthResult{Width, Minimum, Maximum};
    }
    inline ColumnWidthPolicy FromDrag(ColumnWidthPolicy Policy, int Pixels, int Available)
    {
        const auto Resolved = Resolve(Policy, Available);
        if (!Resolved || Policy.mode_ == WidthMode::Fixed) return Policy;
        const auto Width = std::clamp(Pixels, Resolved->minimum_, Resolved->maximum_);
        Policy.value_ = Policy.unit_ == WidthUnit::Percent ? Width * 100.0 / Available : Width;
        Policy.value_ = std::clamp(Policy.value_, Policy.minimum_.value_or(0), Policy.maximum_.value_or(Policy.unit_ == WidthUnit::Percent ? 100 : 1000000));
        return Policy;
    }
}
