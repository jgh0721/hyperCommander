#pragma once
#include "documentTypes.hpp"

namespace hc::document
{
    inline constexpr std::uint32_t RecoveryEngineSchema = 1;
    struct RecoverySnapshot
    {
        DocumentSnapshot document_;
        std::string viewId_ = "text", bytes_, checksum_, snapshotPath_;
        std::int64_t utcMs_ = 0;
        // ExternalModel requires a native-validated model snapshot, never bytes
        // alone. These pins describe recovery compatibility, not write authority.
        std::string bundleSha256_, format_;
        std::uint32_t engineSchema_ = 0;
    };
    class IRecoveryStore
    {
    public:
        virtual ~IRecoveryStore() = default;
        virtual storage::Result<RecoverySnapshot> Store(RecoverySnapshot Snapshot) = 0;
        virtual storage::Result<std::vector<RecoverySnapshot>> List() = 0;
        virtual storage::Result<RecoverySnapshot> Read(const std::string& Path) = 0;
        virtual storage::Result<storage::Unit> Remove(const DocumentId& Id, std::uint64_t ThroughRevision) = 0;
    };
}
