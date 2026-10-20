#pragma once
#include "commonLibs/storage/storage.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QElapsedTimer>
#include <QSet>
#include <set>
#include <windows.h>

namespace hc::folders {
using namespace hc::storage;
inline bool IsNetwork(const Locator& Location)
{
    if (Location.kind_ == LocatorKind::Smb) return true;
    if (Location.kind_ != LocatorKind::Local) return false;
    const auto Path = QDir::toNativeSeparators(QString::fromStdString(Location.root_));
    if (Path.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive) || (Path.startsWith("\\\\") && !Path.startsWith("\\\\?\\"))) return true;
    const auto DrivePath = Path.startsWith("\\\\?\\") ? Path.mid(4) : Path;
    const auto Drive = DrivePath.left(3).toStdWString();
    return GetDriveTypeW(Drive.c_str()) == DRIVE_REMOTE;
}
inline bool Automatic(const QString& Mode, const Locator& Location)
{
    const bool Native = Location.kind_ == LocatorKind::Local || Location.kind_ == LocatorKind::Smb;
    return Native && !IsNetwork(Location) && (Mode == "everything" || Mode == "all" || Mode == "local");
}
// Folder sizes are logical file bytes. Links/junctions are never followed;
// a failed or incomplete traversal must not be presented as a measured zero.
inline Result<std::uint64_t> Measure(IStorageProvider& Provider, const EntrySnapshot& Root, OperationContext Context)
{
    if (IsNetwork(Root.locator_)) return Error{ErrorCategory::Unsupported, "folder-size-network-disabled"};
    std::vector<std::pair<Locator, unsigned>> Pending{{Root.locator_, 0}};
    std::set<std::string> Visited;
    std::uint64_t Bytes = 0, Entries = 0;
    if (!Root.directory_ || (Root.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT))
        return Error{ErrorCategory::Unsupported, "folder-size-link-or-file"};
    Context.freshEnumerationMetadata_ = true;
    while (!Pending.empty()) {
        if (const auto Failure = Context.Check(Provider.Id())) return *Failure;
        const auto [At, Depth] = Pending.back(); Pending.pop_back();
        if (Depth > 128 || !Visited.insert(Serialize(At)).second)
            return Error{ErrorCategory::BudgetExceeded, "folder-size-tree"};
        auto Open = Provider.Enumerate(At, Context).get(); if (!Open.HasValue()) return Open.Failure();
        for (;;) {
            if (const auto Failure = Context.Check(Provider.Id())) return *Failure;
            auto Page = Open.Value()->Next(Context.maxPageItems_, Context).get();
            if (!Page.HasValue()) return Page.Failure();
            if (Page.Value().entries_.empty() && !Page.Value().end_) return Error{ErrorCategory::Internal, "folder-size-no-progress"};
            for (const auto& Entry : Page.Value().entries_) {
                if (++Entries > Context.EnumerationLimit()) return Error{ErrorCategory::BudgetExceeded, "folder-size-entry-limit"};
                if (Entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (Entry.directory_) Pending.emplace_back(Entry.locator_, Depth + 1);
                else {
                    if (Entry.size_ > (std::numeric_limits<std::uint64_t>::max)() - Bytes) return Error{ErrorCategory::BudgetExceeded, "folder-size-overflow"};
                    Bytes += Entry.size_;
                }
            }
            if (Page.Value().end_) break;
        }
    }
    return Bytes;
}
inline Result<QMap<QString, quint64>> Indexed(const QString& Parent, OperationContext Context,
    const QString& Executable = QCoreApplication::applicationDirPath() + "/hc-search-index.exe")
{
    if (IsNetwork({LocatorKind::Local, Parent.toStdString()})) return Error{ErrorCategory::Unsupported, "folder-size-network-disabled"};
    QProcess Process;
    if (const auto Failure = Context.Check({"everything"})) return *Failure;
    Process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* Arguments) {
        Arguments->flags |= CREATE_NO_WINDOW;
        Arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        Arguments->startupInfo->wShowWindow = SW_HIDE;
    });
    Process.start(Executable, {"--folder-sizes", Parent});
    QElapsedTimer Timer; Timer.start();
    if (!Process.waitForStarted(1000)) return Error{ErrorCategory::Unavailable, "folder-size-index-start"};
    while (!Process.waitForFinished(30)) {
        if (const auto Failure = Context.Check({"everything"})) { Process.kill(); Process.waitForFinished(); return *Failure; }
        if (Timer.elapsed() > 3000) { Process.kill(); Process.waitForFinished(); return Error{ErrorCategory::Unavailable, "folder-size-index-timeout"}; }
    }
    if (const auto Failure = Context.Check({"everything"})) return *Failure;
    if (Process.exitStatus() != QProcess::NormalExit || Process.exitCode()) return Error{ErrorCategory::Unavailable, "folder-size-index-unavailable"};
    const auto Output = Process.readAllStandardOutput();
    if (Output.size() > 16 * 1024 * 1024) return Error{ErrorCategory::BudgetExceeded, "folder-size-index-response"};
    const auto Document = QJsonDocument::fromJson(Output);
    if (!Document.isObject() || !Document["sizes"].isArray()) return Error{ErrorCategory::Internal, "folder-size-index-protocol"};
    QMap<QString, quint64> Sizes;
    for (const auto& Value : Document["sizes"].toArray()) {
        const auto Row = Value.toObject(); const auto Name = Row["name"].toString(); bool Ok = false;
        const auto Bytes = Row["bytes"].toString().toULongLong(&Ok);
        if (!Ok || Name.isEmpty() || Name == "." || Name == ".." || Name.contains('/') || Name.contains('\\'))
            return Error{ErrorCategory::Internal, "folder-size-index-entry"};
        Sizes.insert(Name, Bytes);
    }
    return Sizes;
}
}
