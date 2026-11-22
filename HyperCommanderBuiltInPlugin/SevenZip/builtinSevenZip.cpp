#include "builtinSevenZip.hpp"
#include "hcSevenZipManifest.hpp"
#include "sevenZipBackend.hpp"
#include "UniqueLibs/nativeFilePath.hpp"
#include "pluginHost/manifestFile.hpp"
#include <QFileInfo>
#include <QScopeGuard>
#include "commonLibs/layout/productLayout.hpp"
#include <QCoreApplication>
namespace hc::plugins {
ApprovedWcxOptions BuiltinSevenZipOptions()
{
    ApprovedWcxOptions Options; const auto Root = QCoreApplication::applicationDirPath();
    const auto Package = Root + '/' + hc::layout::SevenZipPackage;
    Options.executable_ = Root + '/' + hc::layout::PluginHost;
    Options.binaryPath_ = Package + "/7z.dll";
    Options.manifestPath_ = Package + "/sevenzip.json";
    auto Manifest = ParseManifest(QByteArray(HcBuiltinSevenZipManifest));
    if (Manifest.HasValue()) {
        Options.approval_.reviewed_ = std::move(Manifest.Value()); Options.approval_.approved_ = true;
        Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
    }
    Options.limits_.requestMs_ = 30000;
    return Options;
}
storage::Result<storage::Unit> CreateBuiltinSevenZipArchive(ArchiveUpdate Request, storage::OperationContext Context)
{
    using namespace storage;
    if (!Request.creation_ || Request.encrypt_ || !Request.remove_.empty() || !Request.metadataOrigins_.empty()
        || !Request.reuseData_.empty() || QFileInfo::exists(QString::fromStdString(Request.archive_)))
        return Error{ErrorCategory::InvalidRequest, "sevenzip-native-creation-only"};
    // The encoder is a product component. Resolve its path and identity only
    // from the compiled product layout and manifest, never from plugin settings.
    const auto Options = BuiltinSevenZipOptions();
    std::vector<HANDLE> Pins;
    const auto Release = qScopeGuard([&] { for (const auto Pin : Pins) CloseHandle(Pin); });
    auto Path = QFileInfo(Options.binaryPath_).absoluteFilePath();
    for (bool File = true;; File = false) {
        const auto Native = native::FilePath(Path);
        const auto Pin = CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),
            File ? GENERIC_READ : FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            File ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (Pin == INVALID_HANDLE_VALUE) return Error{ErrorCategory::Unavailable, "sevenzip-native-package-pin"};
        Pins.push_back(Pin);
        BY_HANDLE_FILE_INFORMATION Info{};
        if (!GetFileInformationByHandle(Pin, &Info) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || bool(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == File)
            return Error{ErrorCategory::Unsupported, "sevenzip-native-package-path"};
        const auto Parent = QFileInfo(Path).absolutePath();
        if (Parent == Path) break;
        Path = Parent;
    }
    auto Inspected = InspectManifestFile(Options.manifestPath_, Context);
    if (!Inspected.HasValue()) return Inspected.Failure();
    if (!Options.approval_.approved_ || Inspected.Value().manifest_ != Options.approval_.reviewed_
        || !Inspected.Value().dependencyPaths_.isEmpty() || !Inspected.Value().helperPaths_.isEmpty()
        || Inspected.Value().binaryPath_.compare(QFileInfo(Options.binaryPath_).canonicalFilePath(), Qt::CaseInsensitive))
        return Error{ErrorCategory::AccessDenied, "sevenzip-native-identity"};
    const auto Module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(Inspected.Value().binaryPath_.utf16()), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!Module) return Error{ErrorCategory::Unavailable, "sevenzip-native-package-load"};
    const auto Unload = qScopeGuard([&] { FreeLibrary(Module); });
    return EncodePrivateSevenZipCandidate(Module, std::move(Request), std::move(Context));
}
}
