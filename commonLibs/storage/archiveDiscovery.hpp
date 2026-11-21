#pragma once
#include "storage.hpp"

namespace hc::storage
{
    // Keep the mounted container revision stable while a copy job walks its index.
    // Releasing the returned handle ends this job's lease.
    class IArchiveDiscovery
    {
    public:
        virtual ~IArchiveDiscovery() = default;
        virtual Result<std::shared_ptr<IReadStream>> PinForDiscovery(OperationContext Context) = 0;
    };
}
