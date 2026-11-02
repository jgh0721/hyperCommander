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
    fm::filelist::ThumbnailProvider::Method method_ = static_cast<fm::filelist::ThumbnailProvider::Method>(hc::defaults::Thumbnails()["method"].toInt());
    int targets_ = hc::defaults::Thumbnails()["targets"].toInt();
    int threads_ = hc::defaults::Thumbnails()["threads"].toInt();
    int cacheMb_ = hc::defaults::Thumbnails()["cacheMb"].toInt();
    int autoPercent_ = hc::defaults::Thumbnails()["autoPercent"].toInt();
    bool skipSlow_ = hc::defaults::Thumbnails()["skipSlow"].toBool();
    bool autoSwitch_ = hc::defaults::Thumbnails()["autoSwitch"].toBool();
};
inline ThumbnailSettings ReadThumbnailSettings(QJsonObject Object)
{
    const auto Base = hc::defaults::Thumbnails();
    for (auto It = Base.cbegin(); It != Base.cend(); ++It)
        if (!Object.contains(It.key())) Object[It.key()] = It.value();
    ThumbnailSettings Result;
    if (Object.value("version").toInt(hc::defaults::Thumbnails()["version"].toInt()) != 1) return ReadThumbnailSettings(Base);
    auto& Appearance = Result.appearance_;
    Appearance.size = Base["size"].toInt();
    const int Size = Object.value("size").toInt(hc::defaults::Thumbnails()["size"].toInt());
    if (Size == 64 || Size == 96 || Size == 160 || Size == 256) Appearance.size = Size;
    Appearance.nameLines = std::clamp(Object.value("nameLines").toInt(hc::defaults::Thumbnails()["nameLines"].toInt()), 0, 2);
    Appearance.info = static_cast<fm::filelist::ThumbnailAppearance::Info>(std::clamp(Object.value("info").toInt(hc::defaults::Thumbnails()["info"].toInt()), 0, 3));
    Appearance.fill = Object.value("fill").toBool(hc::defaults::Thumbnails()["fill"].toBool()); Appearance.badges = Object.value("badges").toBool(hc::defaults::Thumbnails()["badges"].toBool());
    Result.method_ = static_cast<fm::filelist::ThumbnailProvider::Method>(std::clamp(Object.value("method").toInt(hc::defaults::Thumbnails()["method"].toInt()), 0, 2));
    Result.targets_ = Object.value("targets").toInt(hc::defaults::Thumbnails()["targets"].toInt()) & 31;
    Result.threads_ = std::clamp(Object.value("threads").toInt(hc::defaults::Thumbnails()["threads"].toInt()), 1, 4);
    Result.cacheMb_ = std::clamp(Object.value("cacheMb").toInt(hc::defaults::Thumbnails()["cacheMb"].toInt()), 4, 96);
    Result.skipSlow_ = Object.value("skipSlow").toBool(hc::defaults::Thumbnails()["skipSlow"].toBool());
    Result.autoSwitch_ = Object.value("autoSwitch").toBool(hc::defaults::Thumbnails()["autoSwitch"].toBool());
    Result.autoPercent_ = std::clamp(Object.value("autoPercent").toInt(hc::defaults::Thumbnails()["autoPercent"].toInt()), 5, 100);
    return Result;
}
}
