#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <functional>

namespace hc::filelocks {
// Native inspection runs only in the isolated administrator executable.
struct ScanResult {
    QJsonArray rows_;
    bool partial_ = false;
    QStringList reasons_;
};
using StopRequested = std::function<bool()>;
QString ProcessCreationTime(void* Process);
QString ProcessImage(void* Process);
QString ProcessUserSid(void* Process);
QString NativeFileIdentity(void* File);
bool IsLocalLockPath(const QString& Path);
ScanResult Scan(const QStringList& Paths, quint32 CallerPid, StopRequested Stop = {});
// Only server-generated rows may cross this boundary; RPC accepts capabilities.
QJsonObject ExecuteAction(const QJsonObject& Row, const QString& Action, quint32 CallerPid);
}
