#pragma once
#include "manifestFile.hpp"
#include "commonLibs/plugins/pluginWire.hpp"
#include "commonLibs/storage/providerSession.hpp"
#include <QJsonObject>

namespace hc::plugins
{
    enum class HostPurpose { General, Configuration };
    struct HostLimits
    {
        std::uint32_t startupMs_ = 5000, requestMs_ = 3000, heartbeatMs_ = 1000;
        std::uint32_t archiveWriteMs_ = 600000;
        std::uint64_t memoryBytes_ = 512ULL * 1024 * 1024;
        bool enableSessionV2_ = true;
        std::uint32_t noProgressMs_ = 600000, cancelGraceMs_ = 1000;
        HostPurpose purpose_ = HostPurpose::General;
        bool operator==(const HostLimits&) const = default;
    };
    struct HostSnapshot
    {
        std::uint32_t processId_ = 0, activeProcesses_ = 0, totalProcesses_ = 0;
        bool running_ = false;
        bool busy_ = false; // Cached observation while another IPC call owns the channel.
        bool forcedTermination_ = false;
        bool callerDetached_ = false;
        QJsonObject runtime_;
        std::optional<std::uint32_t> exitCode_;
        std::optional<storage::Error> failure_;
    };
    // Blocking worker-side owner; never construct/call/destroy on the GUI thread.
    // Viewer/configuration calls use independent command and control I/O owners;
    // archive/data v2 retains its session transport. Destruction requires callers
    // to have returned; Close deterministically reaps the runtime and its Job.
    class PluginProcess final : public storage::IProviderSession
    {
    public:
        static storage::Result<std::unique_ptr<PluginProcess>> Start(const QString& Executable, const QString& ManifestPath,
            const PluginApproval& Approval, HostLimits Limits = {}, storage::OperationContext Context = {},
            const QString& DefaultIniPath = {}, const QJsonObject& ImageArchiveEndpoint = {});
        ~PluginProcess();
        storage::Result<QJsonObject> Probe(storage::OperationContext Context = {});
        storage::Result<QJsonObject> Ping(storage::OperationContext Context = {});
        // Rejoin the last detached subscription; never retries an SDK call.
        storage::Result<QJsonObject> AwaitPending(storage::OperationContext Context = {});
        storage::Result<QJsonObject> ArchiveCall(WireKind Kind, QJsonObject Request, storage::OperationContext Context = {});
        storage::Result<QJsonObject> ViewerCall(WireKind Kind, QJsonObject Request, storage::OperationContext Context = {});
        storage::Result<QJsonObject> DataCall(WireKind Kind, QJsonObject Request, storage::OperationContext Context = {});
        storage::ProviderSessionSnapshot Session() const override;
        storage::Result<QJsonObject> SessionCall(WireKind Kind, QJsonObject Request, storage::OperationContext Context = {}, QByteArray Binary = {});
        storage::Result<storage::ReadChunk> ReadArchiveStream(std::uint64_t Stream, std::uint32_t Ordinal, std::size_t Count, storage::OperationContext Context = {});
        storage::Result<storage::ReadChunk> ReadStream(const QString& Token, std::size_t Count, storage::OperationContext Context = {});
        storage::Result<QJsonObject> ArchiveData(QJsonObject Request, const QByteArray& Bytes, storage::OperationContext Context = {});
        HostSnapshot Snapshot() const;
        // Value-only status source; safe after this owner has closed/died.
        std::function<HostSnapshot()> Observe() const;
        std::function<void()> ForceStopSource() const;
        unsigned ArchiveOptionsVersion() const;
        storage::Result<QJsonObject> Configure(std::uint64_t Owner, storage::OperationContext Context = {});
        void Close();
    private:
        struct State;
        explicit PluginProcess(std::unique_ptr<State> Value);
        std::unique_ptr<State> state_;
    };
}
