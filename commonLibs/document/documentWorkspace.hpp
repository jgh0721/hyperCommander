#pragma once
#include "documentSession.hpp"

namespace hc::document
{
    class DocumentWorkspace final
    {
    public:
        storage::Result<storage::Unit> Add(std::shared_ptr<IDocumentSession> Session);
        std::shared_ptr<IDocumentSession> Find(const DocumentId& Id) const;
        storage::Result<storage::Unit> Activate(const DocumentId& Id);
        storage::Result<storage::Unit> Remove(const DocumentId& Id, CloseDisposition Disposition);
        std::vector<DocumentSnapshot> Snapshot() const;
        const DocumentId& ActiveId() const { return activeId_; }
    private:
        std::vector<std::shared_ptr<IDocumentSession>> sessions_;
        DocumentId activeId_;
    };
}
