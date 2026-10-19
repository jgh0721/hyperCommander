#pragma once
#include "jobScheduler.hpp"
#include "fileElevation.hpp"
#include "../storage/writableStorage.hpp"
#include "../storage/entryIndex.hpp"

namespace hc::operations
{
    // All mutation coordinators share the same volume/share/plugin admission lane.
    inline std::string FileOperationLane(const storage::Locator& At)
    {
        auto Path = At.root_; std::replace(Path.begin(),Path.end(),'/','\\');
        if (Path.starts_with("\\\\?\\UNC\\")) Path = "\\\\" + Path.substr(8);
        else if (Path.starts_with("\\\\?\\")) Path.erase(0,4);
        while (!Path.empty() && Path.back() == '\\') Path.pop_back();
        for (auto& C : Path) if (C >= 'A' && C <= 'Z') C += 'a'-'A';
        if (At.kind_ == storage::LocatorKind::Archive) return "archive:" + Path;
        if (At.kind_ == storage::LocatorKind::Plugin) return "plugin:" + Path.substr(0,Path.find('\\'));
        if (Path.starts_with("\\\\")) {
            const auto Server = Path.find('\\',2); const auto Share = Server == std::string::npos ? Server : Path.find('\\',Server+1);
            return "share:" + Path.substr(0,Share);
        }
        return "volume:" + Path.substr(0,2);
    }
    enum class FileOperation { Copy, Move, Delete, MakeDirectory, Rename };
    enum class DeleteMode { Permanent, Recycle };
    enum class ReadOnlyPolicy { Ask, Allow, Skip };
    enum class ConflictPolicy { Ask, Replace, Skip };
    struct OperationSource
    {
        storage::EntrySnapshot entry_;
        std::shared_ptr<storage::IStorageProvider> reader_;
        std::shared_ptr<storage::IWritableStorage> writer_;
    };
    struct FileOperationRequest
    {
        FileOperation operation_ = FileOperation::Copy;
        std::vector<OperationSource> sources_;
        std::shared_ptr<storage::ISelectionManifest> selection_;
        storage::Locator selectionRoot_;
        std::shared_ptr<storage::IWritableStorage> selectionWriter_;
        // Copy/move: destination directory. Mkdir/rename: complete destination.
        storage::Locator destination_;
        std::shared_ptr<storage::IWritableStorage> destinationWriter_;
        ConflictPolicy conflict_ = ConflictPolicy::Ask;
        // A single-item helper preserves the caller's immutable overwrite
        // decision instead of making a fresh replacement decision after UAC.
        std::optional<storage::DestinationExpectation> approvedDestination_;
        bool confirmDelete_ = true;
        // Product UI opts in; headless callers retain terminal failure semantics.
        bool fileLockInteractions_ = false;
        bool calculateSizeBefore_ = true;
        bool deleteWhileListing_ = true;
        DeleteMode deleteMode_ = DeleteMode::Permanent;
        ReadOnlyPolicy readOnly_ = ReadOnlyPolicy::Ask;
        bool createParents_ = false;
        storage::CopyOptions copyOptions_;
        std::shared_ptr<IFileElevationService> elevation_;
        std::shared_ptr<FileElevationApproval> elevationApproval_;
        // Semicolon-separated basename wildcards. Directories are always visited;
        // excluded files keep their source parent alive during a filtered move.
        std::string includePattern_ = "*";
        std::uint64_t bytesPerSecondLimit_ = 0; // Zero means unlimited, per job.
        // Optional single-selection output name for copy/move.
        std::string destinationName_;
        std::size_t maxEntries_ = std::numeric_limits<std::size_t>::max();
        std::size_t maxDepth_ = 32;
        std::uint64_t maxBytes_ = std::numeric_limits<std::uint64_t>::max();
        std::string correlationId_;
    };
    inline bool IsValidCopyPattern(const std::string& Pattern)
    {
        if (Pattern.empty() || Pattern.size() > 4096 || Pattern.find_first_of("/\\:") != std::string::npos) return false;
        for (const auto Ch : Pattern) if (static_cast<unsigned char>(Ch) < 32) return false;
        for (std::size_t Begin = 0; Begin <= Pattern.size();) {
            const auto End = Pattern.find(';', Begin);
            if (Pattern.substr(Begin, End == std::string::npos ? End : End - Begin).find_first_not_of(' ') == std::string::npos) return false;
            if (End == std::string::npos) break;
            Begin = End + 1;
        }
        return true;
    }
    // Pure construction/validation, no I/O; allows coordinators to compose tasks
    // while retaining one scheduler job and one set of resource lanes.
    Result<PreparedJob> PrepareFileOperation(FileOperationRequest Request, storage::OperationContext Context = {});
    Result<JobHandle> SubmitFileOperation(JobScheduler& Scheduler, FileOperationRequest Request,
        storage::OperationContext Context = {});
}
