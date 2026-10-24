#pragma once
#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <fmstyle/ThemeManager.h>

namespace hc::document::epubui {
inline QJsonObject Appearance() {
    const auto& Theme = fm::style::ThemeManager::instance();
    QJsonArray Colors;
    for (std::size_t I = 0; I < fm::style::kTokenCount; ++I)
        Colors.append(Theme.colors(fm::style::Design::Watercolor, Theme.effectiveVariant()).color(static_cast<fm::style::Token>(I)).name(QColor::HexArgb));
    return {{"variant", int(Theme.effectiveVariant())}, {"colors", Colors},
        {"monoFamily", Theme.monoFontFamily()}, {"monoPx", Theme.monoFontPx()}, {"font", QApplication::font().toString()}};
}
inline bool ApplyAppearance(const QJsonObject& Value) {
    const auto Variant = Value["variant"].toInt(-1);
    const auto Colors = Value["colors"].toArray();
    QFont Font;
    if (Variant < 0 || Variant > 2 || Value["variant"] != QJsonValue(Variant)
        || Colors.size() != int(fm::style::kTokenCount) || Value["monoFamily"].toString().size() > 128
        || Value["monoPx"].toInt() < 6 || Value["monoPx"].toInt() > 100
        || Value["font"].toString().size() > 512 || !Font.fromString(Value["font"].toString())) return false;
    for (const auto& Color : Colors) {
        const auto Text = Color.toString();
        if (Text.size() != 9 || !Text.startsWith('#') || !QColor(Text).isValid()) return false;
    }
    auto& Theme = fm::style::ThemeManager::instance();
    fm::style::ThemeManager::Batch Batch(Theme);
    Theme.setDesign(fm::style::Design::Watercolor);
    Theme.setDarkTone(Variant == 2 ? fm::style::ThemeManager::DarkTone::Navy : fm::style::ThemeManager::DarkTone::Gray);
    Theme.setScheme(Variant ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
    for (int I = 0; I < Colors.size(); ++I)
        Theme.setOverride(fm::style::Design::Watercolor, static_cast<fm::style::Variant>(Variant), static_cast<fm::style::Token>(I), QColor(Colors[I].toString()));
    Theme.setMonoFont(Value["monoFamily"].toString(), Value["monoPx"].toInt());
    QApplication::setFont(Font);
    return true;
}
}
