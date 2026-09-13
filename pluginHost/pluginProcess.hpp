#pragma once
#include "manifestFile.hpp"
#include "commonLibs/plugins/pluginWire.hpp"
#include <QJsonObject>

namespace hc::plugins
{
    struct HostLimits
    {
        std::uint32_t startupMs_ = 5000, requestMs_ = 3000, heartbeatMs_ = 1000;
        std::uint32_t archiveWriteMs_ = 600000;
        std::uint64_t memoryBytes_ = 512ULL * 1024 * 1024;
    };
    struct HostSnapshot
    {
        std::uint32_t processId_ = 0, activeProcesses_ = 0, totalProcesses_ = 0;
        bool running_ = false;
        bool forcedTermination_ = false;
        std::optional<std::uint32_t> exitCode_;
        std::optional<storage::Error> failure_;
    };
    // Blocking worker-side owner; never construct/call/destroy on the GUI thread.
    // Calls serialize with the idle heartbeat. Destruction requires callers to
    // have returned; Close cancels monitoring and deterministically reaps the job.
    class PluginProcess final
    {
    public:
        static storage::Result<std::unique_ptr<PluginProcess>> Start(const QString& Executable, const QString& ManifestPath,
            const PluginApproval& Approval, HostLimits Limits = {}, storage::OperationContext Context = {},
            const QString& DefaultIniPath = {});
        ~PluginProcess();
        storage::Result<QJsonObject> Probe(storage::OperationContext Context = {});
        storage::Result<QJsonObject> Ping(storage::OperationContext Context = {});
        storage::Result<QJsonObject> ArchiveCall(WireKind Kind, QJsonObject Request, storage::OperationContext Context = {});
        storage::Result<QJsonObject> ViewerCall(WireKind Kind, QJsonObject Request, storage::OperationContext Context = {});
        storage::Result<QJsonObject> DataCall(WireKind Kind, QJsonObject Request, storage::OperationContext Context = {});
        HostSnapshot Snapshot() const;
        void Close();
    private:
        struct State;
        explicit PluginProcess(std::unique_ptr<State> Value);
        std::unique_ptr<State> state_;
    };
}
