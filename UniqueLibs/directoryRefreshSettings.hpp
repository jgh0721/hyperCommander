#pragma once
#include "UniqueLibs/productDefaults.hpp"
#include <QDir>
#include <QJsonObject>
#include <QRegularExpression>

namespace hc::refresh {
struct Settings {
    bool names = hc::defaults::DefaultValue("general/refresh/names").toBool();
    bool metadata = hc::defaults::DefaultValue("general/refresh/metadata").toBool();
    bool appendNewEntries = hc::defaults::DefaultValue("general/refresh/appendNewEntries").toBool();
    bool pauseInBackground = hc::defaults::DefaultValue("general/refresh/pauseInBackground").toBool();
    bool updateFooter = hc::defaults::DefaultValue("general/refresh/updateFooter").toBool();
    bool updateHeader = hc::defaults::DefaultValue("general/refresh/updateHeader").toBool();
    QString excludedDrives = hc::defaults::DefaultValue("general/refresh/excludedDrives").toString();
    bool fallbackEnabled = hc::defaults::DefaultValue("general/refresh/fallbackEnabled").toBool();
    int fallbackSeconds = hc::defaults::DefaultValue("general/refresh/fallbackSeconds").toInt();

    QJsonObject ToJson() const {
        return {{"names", names}, {"metadata", metadata}, {"appendNewEntries", appendNewEntries},
            {"pauseInBackground", pauseInBackground}, {"updateFooter", updateFooter}, {"updateHeader", updateHeader},
            {"excludedDrives", excludedDrives}, {"fallbackEnabled", fallbackEnabled}, {"fallbackSeconds", fallbackSeconds}};
    }
    static Settings FromJson(const QJsonObject& Object) {
        Settings Value;
        Value.names = Object["names"].toBool(hc::defaults::DefaultValue("general/refresh/names").toBool()); Value.metadata = Object["metadata"].toBool(hc::defaults::DefaultValue("general/refresh/metadata").toBool());
        Value.appendNewEntries = Object["appendNewEntries"].toBool(hc::defaults::DefaultValue("general/refresh/appendNewEntries").toBool());
        Value.pauseInBackground = Object["pauseInBackground"].toBool(hc::defaults::DefaultValue("general/refresh/pauseInBackground").toBool());
        Value.updateFooter = Object["updateFooter"].toBool(hc::defaults::DefaultValue("general/refresh/updateFooter").toBool()); Value.updateHeader = Object["updateHeader"].toBool(hc::defaults::DefaultValue("general/refresh/updateHeader").toBool());
        Value.excludedDrives = Object["excludedDrives"].toString(hc::defaults::DefaultValue("general/refresh/excludedDrives").toString());
        Value.fallbackEnabled = Object["fallbackEnabled"].toBool(hc::defaults::DefaultValue("general/refresh/fallbackEnabled").toBool());
        Value.fallbackSeconds = qBound(1, Object["fallbackSeconds"].toInt(hc::defaults::DefaultValue("general/refresh/fallbackSeconds").toInt()), 86400);
        return Value;
    }
    bool Excludes(QString Path) const {
        Path = QDir::fromNativeSeparators(Path).toUpper();
        if (Path.startsWith("//?/UNC/")) Path = "//" + Path.mid(8);
        else if (Path.startsWith("//?/")) Path = Path.mid(4);
        const auto Parts = excludedDrives.split(QRegularExpression("[;,\\s]+"), Qt::SkipEmptyParts);
        for (auto Part : Parts) {
            Part = QDir::fromNativeSeparators(Part.trimmed()).toUpper();
            while (Part.endsWith('/')) Part.chop(1);
            if (Part.size() == 1 && Part.front().isLetter()) Part += ':';
            if (!Part.isEmpty() && (Path == Part || Path.startsWith(Part + '/'))) return true;
        }
        return false;
    }
    bool operator==(const Settings&) const = default;
};
}
