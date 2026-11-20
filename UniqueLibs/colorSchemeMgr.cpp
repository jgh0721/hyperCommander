#include "stdafx.h"
#include "colorSchemeMgr.hpp"

#include "productTheme.hpp"
#include "settings/appSettings.hpp"
#include <fmstyle/ThemeManager.h>
#include <QStyleHints>

DECLARE_CMNLIBSV2_NAMESPACE

namespace {
QFont DisplayFont(const QJsonObject& Fonts, const QString& Role)
{
    const auto Record = Fonts[Role].toObject();
    const auto Default = hc::defaults::FontDefaults(Role);
    const auto Family = Record["family"].toString(Default["family"].toString());
    const int Size = Record["pointSize"].toInt(Default["pointSize"].toInt());
    QFont Font(Family, Size);
    Font.setStyleStrategy(QFont::PreferAntialias);
    Font.setHintingPreference(QFont::PreferFullHinting);
    return Font;
}
}

void CColorSchemeMgr::Refresh()
{
    const auto General = hc::settings::AppSettings().Read(hc::settings::Section::General).toObject();
    const auto Mode = General["themeMode"].toString();
    const bool Dark = Mode == "dark" || (Mode == "system" && qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    current_ = ForMode(Dark);
}

TyColorScheme CColorSchemeMgr::ForMode(bool Dark) const
{
    using namespace fm::style;
    const auto Appearance = hc::settings::AppSettings().Read(hc::settings::Section::Appearance).toObject();
    auto Theme = hc::theme::ReadProductScheme(Appearance["watercolorTheme"].toObject());
    if (Theme.seeds.useSystemAccent) Theme.seeds.accent = ThemeManager::systemAccent();
    const auto Mode = Dark ? Variant::Dark : Variant::Light;
    const auto Colors = deriveTheme(Mode, Theme.seeds, Theme.overridesFor(hc::theme::ProductDesign, Mode), hc::theme::ProductDesign).colors;

    const auto Fonts = Appearance["fonts"].toObject();
    TyColorScheme Scheme;
    Scheme.IsDarkMode = Dark;
    Scheme.Menu_Font = DisplayFont(Fonts, "menu");
    Scheme.Dialog_Font = DisplayFont(Fonts, "dialog");
    Scheme.FileList_Font = DisplayFont(Fonts, "list");
    Scheme.IsInverseCursor = Appearance["inverseCursor"].toBool();
    Scheme.IsInverseSelect = Appearance["inverseSelection"].toBool();
    Scheme.FileList_FGColor = Colors[Token::Fg];
    Scheme.FileList_BGColor = Colors[Token::Surface];
    return Scheme;
}
