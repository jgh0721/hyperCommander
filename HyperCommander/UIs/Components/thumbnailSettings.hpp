#pragma once
#include <QJsonObject>
#include "UniqueLibs/productDefaults.hpp"
#include <QTextLayout>
#include <fmfilelist/ListAppearance.h>
#include <fmfilelist/ThumbnailProvider.h>

namespace hc::ui {
inline int ThumbnailNameLines(const QString& Name, int Width, const QFont& Font)
{
    QTextLayout Layout(Name, Font); QTextOption Option; Option.setWrapMode(QTextOption::WrapAnywhere); Layout.setTextOption(Option);
    Layout.beginLayout(); int Count = 0;
    while (true) { auto Line = Layout.createLine(); if (!Line.isValid()) break; Line.setLineWidth(std::max(1, Width)); ++Count; }
    Layout.endLayout(); return std::max(1, Count);
}
struct ThumbnailSettings {
    fm::filelist::ThumbnailAppearance appearance_;
    fm::filelist::ThumbnailProvider::Method method_ = fm::filelist::ThumbnailProvider::Method::ShellThenBuiltin;
    int targets_ = 15, threads_ = 2, cacheMb_ = 16, autoPercent_ = 70;
    bool skipSlow_ = true, autoSwitch_ = false;
};
inline ThumbnailSettings ReadThumbnailSettings(QJsonObject Object)
{
    const auto Base = hc::defaults::Thumbnails();
    for (auto It = Base.cbegin(); It != Base.cend(); ++It)
        if (!Object.contains(It.key())) Object[It.key()] = It.value();
    ThumbnailSettings Result;
    if (Object.value("version").toInt(1) != 1) return ReadThumbnailSettings(Base);
    auto& Appearance = Result.appearance_;
    const int Size = Object.value("size").toInt(96);
    if (Size == 64 || Size == 96 || Size == 160 || Size == 256) Appearance.size = Size;
    Appearance.nameLines = std::clamp(Object.value("nameLines").toInt(2), 0, 2);
    Appearance.info = static_cast<fm::filelist::ThumbnailAppearance::Info>(std::clamp(Object.value("info").toInt(1), 0, 3));
    Appearance.fill = Object.value("fill").toBool(); Appearance.badges = Object.value("badges").toBool(true);
    Result.method_ = static_cast<fm::filelist::ThumbnailProvider::Method>(std::clamp(Object.value("method").toInt(2), 0, 2));
    Result.targets_ = Object.value("targets").toInt(15) & 31;
    Result.threads_ = std::clamp(Object.value("threads").toInt(2), 1, 4);
    Result.cacheMb_ = std::clamp(Object.value("cacheMb").toInt(16), 4, 96);
    Result.skipSlow_ = Object.value("skipSlow").toBool(true);
    Result.autoSwitch_ = Object.value("autoSwitch").toBool();
    Result.autoPercent_ = std::clamp(Object.value("autoPercent").toInt(70), 5, 100);
    return Result;
}
}
