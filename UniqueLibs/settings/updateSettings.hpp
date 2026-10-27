#pragma once
#include <QDateTime>
#include <QJsonObject>

namespace hc::settings {
// Missing interval values retain the old enabled/disabled preference without
// writing settings merely because the application or settings page was opened.
inline int UpdateCheckIntervalDays(const QJsonObject& Update)
{
    const auto Value = Update.value("checkIntervalDays");
    if (Value.isDouble())
        for (const int Days : {0, 1, 7, 30}) if (Value.toDouble() == Days) return Days;
    return Update["automaticCheck"].toBool(true) ? 7 : 0;
}
inline bool UpdateCheckDue(const QJsonObject& Update, const QDateTime& Now)
{
    const int Days = UpdateCheckIntervalDays(Update);
    if (Days == 0 || !Now.isValid()) return false;
    const auto Last = QDateTime::fromString(Update["lastAttemptUtc"].toString(), Qt::ISODate);
    return !Last.isValid() || Last > Now || Last.secsTo(Now) >= qint64(Days) * 86400;
}
} // namespace hc::settings
