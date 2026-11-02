#pragma once
#include "UniqueLibs/ProductDefaults_Compare.hpp"
#include <QFont>
#include <QJsonObject>
#include <QStringList>
#include <array>
#include <QMap>
#include <QVariant>
#include <memory>

class QSettings;
namespace hc::settings { class JsonStore; }

namespace hc::compare
{
    // All Compare settings use the same atomic JSON file beside the executable.
    class Preferences
    {
    public:
        Preferences();
        ~Preferences();
        static QString FileName();
        QVariant Value(const QString& Key);
        bool Contains(const QString& Key);
        bool SetValue(const QString& Key, const QVariant& Value);
        bool Remove(const QString& Key);
        bool Apply(const QMap<QString, QVariant>& Values, const QStringList& Removed = {});
        bool ImportLegacy(QSettings& Legacy);
        const QString& Error() const { return error_; }
    private:
        bool reload();
        bool finish(bool Success, const QString& Error = {});
        std::unique_ptr<hc::settings::JsonStore> store_;
        QString error_;
    };
    struct CompareSettings
    {
        QString theme_ = hc::defaults::compare::Theme;
        std::array<QFont, 3> fonts_;
        std::array<bool, 5> view_ = hc::defaults::compare::ViewOptions;
        QJsonObject shortcuts_;
        std::array<QJsonObject, 2> colors_;
        std::array<bool, 2> linked_{hc::defaults::compare::LinkColors, hc::defaults::compare::LinkColors};
        QStringList warnings_;
        static CompareSettings Load();
        bool Save(QString& Error) const;
        bool operator==(const CompareSettings& Other) const;
    };
    const CompareSettings& ActiveCompareSettings();
    void SetActiveCompareSettings(const CompareSettings& Settings);
    inline constexpr auto& compareViewKeys = hc::defaults::compare::ViewKeys;
}
