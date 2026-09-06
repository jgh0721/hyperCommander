#pragma once
#include "writableStorage.hpp"

namespace hc::storage
{
    enum class ArchiveChangeKind { Put, Remove, Rename, MakeDirectory };
    struct ArchiveChange
    {
        ArchiveChangeKind kind_ = ArchiveChangeKind::Put;
        std::string path_;
        std::string target_;
        std::shared_ptr<IContentSource> content_;
        std::optional<ResourceId> source_;
        DestinationExpectation destination_;
    };
    struct ArchiveMutationLimits
    {
        std::size_t maxEntries_ = 100000;
        std::size_t maxDepth_ = 32;
        std::uint64_t maxInputBytes_ = 1024ULL * 1024 * 1024;
        std::uint64_t maxArchiveBytes_ = 256ULL * 1024 * 1024;
    };
    class IArchiveMutationSession
    {
    public:
        virtual ~IArchiveMutationSession() = default;
        // One worker owns the session. Prepare may perform archive I/O and
        // remains cancellable; it never replaces the original archive.
        virtual Result<Unit> Prepare(std::vector<ArchiveChange> Changes, OperationContext Context) = 0;
        // The scheduler must enter its commit boundary before calling Commit.
        // A successful receipt means the entire prepared batch was published.
        virtual Result<WriteReceipt> Commit(OperationContext Context) = 0;
        virtual std::vector<Error> Abort() = 0;
    };
}
