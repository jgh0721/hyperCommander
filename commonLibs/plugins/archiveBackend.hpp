#pragma once
#include "../storage/storage.hpp"
#include "archiveCreation.hpp"

namespace hc::plugins
{
    struct ArchiveHeader
    {
        std::string name_;
        std::uint64_t size_ = 0, packedSize_ = 0;
        std::uint32_t attributes_ = 0, crc_ = 0;
        std::int64_t modifiedUtcMs_ = 0;
        bool directory_ = false;
        bool encrypted_ = false;
        // Some adapters cannot update encrypted solid archives in place.
        bool rebuildEncrypted_ = false;
        // This adapter can reuse the stored payload when only its name changes.
        bool metadataRename_ = false;
    };
    struct ArchivePage { std::vector<ArchiveHeader> entries_; bool end_ = false; };
    class IArchiveListing
    {
    public:
        virtual ~IArchiveListing() = default;
        virtual storage::Result<ArchivePage> Next(std::size_t Count, storage::OperationContext Context) = 0;
    };
    struct ArchiveCreationOptions
    {
        unsigned level_ = 5;
        bool solid_ = true;
        std::optional<SevenZipCreateSettings> advanced_;
    };
    struct ArchiveUpdate
    {
        // Prepared copies only. The operation coordinator owns final commit.
        std::string archive_, inputDirectory_;
        std::vector<std::string> add_, remove_;
        std::uint64_t maxOutputBytes_ = std::numeric_limits<std::uint64_t>::max();
        bool encrypt_ = false;
        // Only creation accepts explicit compression settings. An SDK may reduce
        // a solid dictionary after deletion without changing codec or encryption.
        std::optional<ArchiveCreationOptions> creation_;
        // Optional adapter metadata hints, aligned with add_. Empty elements mean
        // new content; other elements identify the replaced/renamed old record.
        // Native adapters can preserve properties absent from generic snapshots.
        std::vector<std::string> metadataOrigins_;
        // Optional flags aligned with add_. True requires an explicit origin
        // and reuses its payload; no filesystem input is opened for that member.
        std::vector<bool> reuseData_;
        // Dedicated NEW capability path; never relaxes mutation requirements.
        bool createNew_ = false;
    };
    // Forward-only, bounded memory output. EOF is returned only after the SDK
    // has validated the selected member. Calls follow increasing listing ordinals.
    class IArchiveReadSession {
    public:
        virtual ~IArchiveReadSession() = default;
        virtual storage::Result<storage::ReadChunk> Read(std::uint32_t Ordinal, std::size_t Count, storage::OperationContext Context) = 0;
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
        // Explicit whole-archive reads may stage one decoder pass. OutputDirectory
        // is a private empty directory; files use listing ordinal + ".bin", never
        // an archive-controlled path. Older adapters retain single-file reads.
        virtual storage::Result<storage::Unit> ExtractAll(std::string, std::string, storage::OperationContext)
        { return storage::Error{storage::ErrorCategory::Unsupported, "archive-batch-extract-unsupported"}; }
        virtual storage::Result<std::unique_ptr<IArchiveReadSession>> OpenReadSession(std::string, std::vector<std::uint32_t>, storage::OperationContext)
        { return storage::Error{storage::ErrorCategory::Unsupported, "archive-stream-unsupported"}; }
        virtual storage::Result<storage::Unit> Update(ArchiveUpdate Request, storage::OperationContext Context) = 0;
    };
}
