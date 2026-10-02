#pragma once
#include "hostedArchiveBackend.hpp"

namespace hc::plugins
{
    struct ApprovedWcxOptions
    {
        QString executable_, manifestPath_, binaryPath_;
        PluginApproval approval_;
        HostLimits limits_;
        QString defaultIniPath_;
        storage::PasswordPrompt passwordPrompt_;
    };
    // Worker calls share an admitted host until it fails or this backend closes.
    // Listings retain their epoch; final process reaping runs off the GUI thread.
    std::shared_ptr<IArchiveBackend> MakeApprovedWcxBackend(ApprovedWcxOptions Options);
}
