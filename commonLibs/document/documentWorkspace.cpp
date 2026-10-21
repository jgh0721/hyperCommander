#include "documentWorkspace.hpp"
namespace hc::document
{
    using namespace storage;
    Result<Unit> DocumentWorkspace::Add(std::shared_ptr<IDocumentSession> Session)
    {
        if (!Session || Session->Snapshot().id_.empty() || Session->Snapshot().lifecycle_ == DocumentLifecycle::Closed || Find(Session->Snapshot().id_) || (maxTabs_ && sessions_.size() >= maxTabs_))
            return Error{ErrorCategory::InvalidRequest, "document-workspace-session"};
        activeId_ = Session->Snapshot().id_; sessions_.push_back(std::move(Session)); return Unit{};
    }
    std::shared_ptr<IDocumentSession> DocumentWorkspace::Find(const DocumentId& Id) const
    { for (const auto& Session : sessions_) if (Session->Snapshot().id_ == Id) return Session; return {}; }
    Result<Unit> DocumentWorkspace::Activate(const DocumentId& Id)
    {
        if (!Find(Id)) return Error{ErrorCategory::NotFound, "document-stale-id"};
        activeId_ = Id; return Unit{};
    }
    Result<Unit> DocumentWorkspace::Remove(const DocumentId& Id, CloseDisposition Disposition)
    {
        auto Session = Find(Id); if (!Session) return Error{ErrorCategory::NotFound, "document-stale-id"};
        auto Closed = Session->Close(Disposition); if (!Closed.HasValue()) return Closed.Failure();
        std::erase(sessions_, Session);
        if (activeId_ == Id) activeId_ = sessions_.empty() ? DocumentId{} : sessions_.back()->Snapshot().id_;
        return Unit{};
    }
    std::vector<DocumentSnapshot> DocumentWorkspace::Snapshot() const
    { std::vector<DocumentSnapshot> Result; for (const auto& Session : sessions_) Result.push_back(Session->Snapshot()); return Result; }
}
