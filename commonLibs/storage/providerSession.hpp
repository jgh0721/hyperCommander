#pragma once
#include "storage.hpp"

namespace hc::storage
{
    enum class ProviderSessionState { Ready, Busy, AwaitingInteraction, Closing, Closed, Faulted };
    struct ProviderSessionSnapshot
    {
        std::string epoch_;
        ProviderSessionState state_ = ProviderSessionState::Closed;
    };
    class IProviderSession
    {
    public:
        virtual ~IProviderSession() = default;
        virtual ProviderSessionSnapshot Session() const = 0;
    };
    inline Result<Unit> ValidateEntryId(const EntryId& Entry, const ProviderSessionSnapshot& Session, const ResourceRevision& Revision)
    {
        if (Entry.token_.empty() || Entry.sessionEpoch_.empty()) return Error{ErrorCategory::InvalidRequest, "entry-token-required"};
        if (Session.state_ == ProviderSessionState::Closed || Session.state_ == ProviderSessionState::Faulted
            || Session.state_ == ProviderSessionState::Closing || Entry.sessionEpoch_ != Session.epoch_ || Entry.containerRevision_ != Revision)
            return Error{ErrorCategory::StaleRevision, "entry-session-changed"};
        return Unit{};
    }
}
