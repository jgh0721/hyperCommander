#include "stdafx.h"
#include "registryCompareLease.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/processInfo.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QDateTime>
#include <QRegularExpression>
#include <QScopeGuard>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryCompareLease, "hc.registry.compareLease")
namespace hc::registry {
namespace {
bool Dead(qint64 Pid, quint64 Stamp)
{
    if (!Pid) return true;
    if (Pid < 0 || Pid > MAXDWORD || !Stamp) return false;
    const auto Process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(Pid));
    if (!Process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const auto Close = qScopeGuard([&] { CloseHandle(Process); });
    const auto Actual = hc::system::ProcessCreateTime(Process);
    if (!Actual) return false;
    return Actual != Stamp || WaitForSingleObject(Process, 0) == WAIT_OBJECT_0;
}
bool Reparse(const QString& Path)
{
    const auto Attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(Path.utf16()));
    return Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
}
QString RegistryCompareLease::Base() { return QDir(QDir::tempPath()).filePath("HyperCommander/registry-compare"); }
RegistryCompareLease::RegistryCompareLease(const QString& Root)
{
    if (!QDir().mkpath(Root) || Reparse(Root)) return;
    directory_ = std::make_unique<QTemporaryDir>(QDir(Root).filePath("run-XXXXXX"));
    if (!directory_->isValid()) return;
    const auto Now = QDateTime::currentDateTimeUtc();
    marker_ = {{"format", "hc-registry-compare-v1"}, {"purpose", "Registry comparison inputs"},
        {"createdUtc", Now.toString(Qt::ISODateWithMs)}, {"expiresUtc", Now.addDays(7).toString(Qt::ISODateWithMs)},
        {"ownerPid", QString::number(QCoreApplication::applicationPid())},
        {"ownerStamp", QString::number(hc::system::ProcessCreateTime(GetCurrentProcess()))},
        {"consumerPid", "0"}, {"consumerStamp", "0"}, {"launchPending", false}};
    valid_ = save();
}
RegistryCompareLease::~RegistryCompareLease()
{
    if (directory_ && (marker_["launchPending"].toBool()
        || !Dead(marker_["consumerPid"].toString().toLongLong(), marker_["consumerStamp"].toString().toULongLong())))
        directory_->setAutoRemove(false);
}
void RegistryCompareLease::CancelStart() { marker_["launchPending"] = false; save(); }
bool RegistryCompareLease::save()
{
    if (!directory_) return false;
    QSaveFile File(directory_->filePath("owner.json")); const auto Bytes = QJsonDocument(marker_).toJson(QJsonDocument::Compact);
    return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size() && File.commit();
}
bool RegistryCompareLease::Starting() { marker_["launchPending"] = true; return save(); }
bool RegistryCompareLease::SetProcess(qint64 Pid)
{
    const auto Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(Pid));
    if (!Process) return false;
    const auto Stamp = hc::system::ProcessCreateTime(Process); CloseHandle(Process); if (!Stamp) return false;
    marker_["consumerPid"] = QString::number(Pid); marker_["consumerStamp"] = QString::number(Stamp); marker_["launchPending"] = false;
    return save();
}
int RegistryCompareLease::Sweep(const QString& Root)
{
    if (!QDir(Root).exists() || Reparse(Root)) return 0;
    int Removed = 0; const QRegularExpression Names("^run-[A-Za-z0-9]{6}$");
    for (const auto& Entry : QDir(Root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!Names.match(Entry.fileName()).hasMatch() || Reparse(Entry.absoluteFilePath())) continue;
        QFile File(QDir(Entry.absoluteFilePath()).filePath("owner.json"));
        if (!File.open(QIODevice::ReadOnly) || File.size() > 16384) continue;
        const auto Marker = QJsonDocument::fromJson(File.readAll()).object(); File.close();
        if (Marker["format"] != "hc-registry-compare-v1" || !Marker["launchPending"].isBool() || Marker["launchPending"].toBool()) continue;
        if (Marker["ownerPid"].toString().toLongLong() <= 0 || !Marker["ownerStamp"].toString().toULongLong()
            || !QDateTime::fromString(Marker["createdUtc"].toString(), Qt::ISODateWithMs).isValid()) continue;
        if (!Dead(Marker["ownerPid"].toString().toLongLong(), Marker["ownerStamp"].toString().toULongLong())
            || !Dead(Marker["consumerPid"].toString().toLongLong(), Marker["consumerStamp"].toString().toULongLong())) continue;
        // Only marked paths under this fixed root, with no junction/symlink to
        // another tree, can be reclaimed. Unknown process state is preserved.
        bool Safe = true; QDirIterator Children(Entry.absoluteFilePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (Children.hasNext()) if (Reparse(Children.next())) { Safe = false; break; }
        if (Safe && QDir(Entry.absoluteFilePath()).removeRecursively()) ++Removed;
    }
    return Removed;
}
}