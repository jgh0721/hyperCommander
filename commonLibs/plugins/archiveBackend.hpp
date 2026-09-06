#pragma once
#include "../storage/storage.hpp"

namespace hc::plugins
{
    struct ArchiveHeader
    {
        std::string name_;
        std::uint64_t size_ = 0, packedSize_ = 0;
        std::uint32_t attributes_ = 0, crc_ = 0;
        std::int64_t modifiedUtcMs_ = 0;
        bool directory_ = false;
    };
    struct ArchivePage { std::vector<ArchiveHeader> entries_; bool end_ = false; };
    class IArchiveListing
    {
    public:
        virtual ~IArchiveListing() = default;
        virtual storage::Result<ArchivePage> Next(std::size_t Count, storage::OperationContext Context) = 0;
    };
    struct ArchiveUpdate
    {
        // Prepared copies only. The operation coordinator owns final commit.
        std::string archive_, inputDirectory_;
        std::vector<std::string> add_, remove_;
        std::uint64_t maxOutputBytes_ = 256ULL * 1024 * 1024;
    };
    class IArchiveBackend
    {
    public:
        virtual ~IArchiveBackend() = default;
        // All methods and destruction are worker-side operations. A listing
        // processes each member before advancing and owns the archive handle.
        virtual storage::Result<std::unique_ptr<IArchiveListing>> Enumerate(std::string Archive, bool Test,
            storage::OperationContext Context) = 0;
        virtual storage::Result<storage::Unit> Extract(std::string Archive, ArchiveHeader Expected, std::string Output,
            storage::OperationContext Context) = 0;
        virtual storage::Result<storage::Unit> Update(ArchiveUpdate Request, storage::OperationContext Context) = 0;
    };
}
