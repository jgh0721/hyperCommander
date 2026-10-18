#pragma once
#include "commonLibs/operations/fileElevation.hpp"
#include <QJsonObject>
#include <Windows.h>
namespace hc::permissions {
// A launcher can supply an isolated fixture process. The production helper
// independently requires an elevated token; injection never bypasses that check.
using FileElevationLauncher=std::function<storage::Result<HANDLE>(const QString&,const QString&)>;
std::shared_ptr<operations::IFileElevationService> MakeFileElevationService(FileElevationLauncher Launcher={});
// Called by the administrator executable only after checking its elevated token.
DWORD ExecuteFileElevation(const QJsonObject& Request);
// A tab owns this read-only session while browsing the approved local subtree.
class DirectoryElevationSession {
public:
    virtual ~DirectoryElevationSession() = default;
    virtual bool Contains(const QString& Path) const = 0;
    virtual storage::Result<std::unique_ptr<storage::IEntryCursor>> Enumerate(const QString& Path,
        storage::OperationContext Context, std::shared_ptr<storage::IStorageProvider> Reader) = 0;
};
storage::Result<std::shared_ptr<DirectoryElevationSession>> OpenElevatedDirectorySession(
    const QString& Path, storage::OperationContext Context, FileElevationLauncher Launcher = {});
// Convenience for single-request callers and protocol tests.
storage::Result<std::unique_ptr<storage::IEntryCursor>> EnumerateElevatedDirectory(
    const QString& Path, storage::OperationContext Context, FileElevationLauncher Launcher = {},
    std::shared_ptr<storage::IStorageProvider> Reader = {});
DWORD ExecuteDirectoryElevation(const QJsonObject& Request);
}
