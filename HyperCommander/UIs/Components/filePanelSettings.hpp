#pragma once
#include <QJsonObject>
#include "UniqueLibs/productDefaults.hpp"
#include <QStringList>
#include <fmfilelist/ListAppearance.h>

namespace hc::ui {
struct FilePanelSettings {
    QString mode_ = "one-line";
    fm::filelist::ListAppearance appearance_;
};
inline const QStringList ListModes{"inherit", "one-line", "two-line", "auto", "thumbnails"};
inline FilePanelSettings ReadFilePanelSettings(QJsonObject Object)
{
    const auto Base = hc::defaults::FilePanel();
    for (auto It = Base.cbegin(); It != Base.cend(); ++It)
        if (!Object.contains(It.key())) Object[It.key()] = It.value();
    FilePanelSettings Result;
    if (Object.value("version").toInt(1) != 1) return ReadFilePanelSettings(Base);
    const auto Mode = Object.value("defaultMode").toString();
    if (ListModes.contains(Mode) && Mode != "inherit") Result.mode_ = Mode;
    auto& Appearance = Result.appearance_;
    const QStringList Separators{"none", "zebra", "line", "space", "tint"};
    const auto Separator = [&](const char* Key, fm::filelist::RecordSeparator Fallback) {
        const int Index = Separators.indexOf(Object.value(Key).toString());
        return Index < 0 ? Fallback : static_cast<fm::filelist::RecordSeparator>(Index);
    };
    Appearance.oneLineSeparator = Separator("oneLineSeparator", Appearance.oneLineSeparator);
    Appearance.twoLineSeparator = Separator("twoLineSeparator", Appearance.twoLineSeparator);
    Appearance.nameBelow = Object.value("nameBelow").toBool();
    Appearance.autoWidth = std::clamp(Object.value("autoWidthDip").toInt(640), 100, 4000);
    Appearance.autoPercent = std::clamp(Object.value("autoClippedPercent").toInt(25), 5, 100);
    const int Density = QStringList{"compact", "normal", "relaxed"}.indexOf(Object.value("density").toString("normal"));
    Appearance.density = static_cast<fm::filelist::RowDensity>(std::max(0, Density));
    const int Elide = QStringList{"keep-extension", "end", "middle"}.indexOf(Object.value("nameElide").toString("keep-extension"));
    Appearance.nameElide = static_cast<fm::filelist::NameElide>(std::max(0, Elide));
    return Result;
}
}
