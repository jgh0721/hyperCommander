#pragma once
#include <QFont>
#include <QJsonObject>
#include <QStringList>
#include <array>

namespace hc::compare
{
    struct CompareSettings
    {
        QString theme_ = "product";
        std::array<QFont, 3> fonts_;
        std::array<bool, 5> view_{true, true, true, true, true};
        QJsonObject shortcuts_;
        std::array<QJsonObject, 2> colors_;
        std::array<bool, 2> linked_{true, true};
        QStringList warnings_;
        static CompareSettings Load();
        bool Save(QString& Error) const;
        bool operator==(const CompareSettings& Other) const;
    };
    const CompareSettings& ActiveCompareSettings();
    void SetActiveCompareSettings(const CompareSettings& Settings);
    inline constexpr const char* compareViewKeys[]{"view/alignAllPanes", "view/alignSourceRows",
        "view/synchronizeSources", "view/inlineHighlights", "view/overview"};
}
