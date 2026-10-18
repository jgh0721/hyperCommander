#pragma once
#include "commonLibs/document/documentRecovery.hpp"
#include <QString>

namespace hc::document
{
    class DocumentRecoveryStore final : public IRecoveryStore
    {
    public:
        explicit DocumentRecoveryStore(QString Root) : root_(std::move(Root)) {}
        storage::Result<RecoverySnapshot> Store(RecoverySnapshot Snapshot) override;
        storage::Result<std::vector<RecoverySnapshot>> List() override;
        storage::Result<RecoverySnapshot> Read(const std::string& Path) override;
        storage::Result<storage::Unit> Remove(const DocumentId& Id, std::uint64_t ThroughRevision) override;
    private:
        QString root_;
    };
}
