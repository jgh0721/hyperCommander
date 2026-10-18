#pragma once
#include <QDir>
#include <QJsonObject>
#include <QRegularExpression>

namespace hc::refresh {
struct Settings {
    bool names = true;
    bool metadata = true;
    bool appendNewEntries = false;
    bool pauseInBackground = true;
    bool updateFooter = true;
    bool updateHeader = true;
    QString excludedDrives;
    bool fallbackEnabled = false;
    int fallbackSeconds = 15;

    QJsonObject ToJson() const {
        return {{"names", names}, {"metadata", metadata}, {"appendNewEntries", appendNewEntries},
            {"pauseInBackground", pauseInBackground}, {"updateFooter", updateFooter}, {"updateHeader", updateHeader},
            {"excludedDrives", excludedDrives}, {"fallbackEnabled", fallbackEnabled}, {"fallbackSeconds", fallbackSeconds}};
    }
    static Settings FromJson(const QJsonObject& Object) {
        Settings Value;
        Value.names = Object["names"].toBool(true); Value.metadata = Object["metadata"].toBool(true);
        Value.appendNewEntries = Object["appendNewEntries"].toBool();
        Value.pauseInBackground = Object["pauseInBackground"].toBool(true);
        Value.updateFooter = Object["updateFooter"].toBool(true); Value.updateHeader = Object["updateHeader"].toBool(true);
        Value.excludedDrives = Object["excludedDrives"].toString();
        Value.fallbackEnabled = Object["fallbackEnabled"].toBool();
        Value.fallbackSeconds = qBound(1, Object["fallbackSeconds"].toInt(15), 86400);
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
