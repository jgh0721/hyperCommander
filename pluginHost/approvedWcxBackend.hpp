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
    // Construction/destruction performs no I/O. Each worker call opens a host;
    // enumeration keeps that host only for the returned listing's lifetime.
    std::shared_ptr<IArchiveBackend> MakeApprovedWcxBackend(ApprovedWcxOptions Options);
}
