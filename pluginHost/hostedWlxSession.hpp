#pragma once
#include "pluginProcess.hpp"
#include "commonLibs/viewer/viewer.hpp"
#include "commonLibs/storage/materializedContent.hpp"
#include <functional>

namespace hc::plugins
{
    struct WlxViewerOptions
    {
        QString executable_, manifestPath_, binaryPath_, defaultIniPath_;
        PluginApproval approval_;
        HostLimits limits_;
        int flags_ = 0;
    };
    struct WlxSessionDetails
    {
        viewer::ViewerSnapshot viewer_;
        HostSnapshot host_;
        // Windows adapter tokens only; never interpreted as memory addresses.
        QString window_, child_;
        bool idle_ = false;
        bool noMatch_ = false;
        std::optional<std::size_t> selectedCandidate_;
        struct CandidateFailure { std::size_t index_; storage::Error error_; };
        std::vector<CandidateFailure> candidateFailures_;
        std::uint64_t commandSequence_ = 0;
        std::optional<storage::Error> commandError_;
    };
    class WlxViewerService;
    struct WlxAutomaticPolicy
    {
        bool force_ = false;
        // Only MULTIMEDIA candidates override supported builtin multimedia
        // unless the user explicitly requests forced plugin selection.
        bool builtinMultimedia_ = false;
        // Optional worker-only content probe supplied by the builtin adapter.
        std::function<bool(const storage::EntrySnapshot&, std::span<const std::byte>)> builtinProbe_;
    };
    class HostedWlxSession final : public viewer::IViewerSession
    {
    public:
        ~HostedWlxSession() override;
        viewer::ViewerSnapshot Snapshot() const override;
        // The native presentation is owned by the host, not a decoded document.
        std::shared_ptr<const viewer::ViewerDocument> Document() const override { return {}; }
        std::vector<std::string> Commands() const override;
        void Cancel() override;
        void Close() override;
        WlxSessionDetails Details() const;
        storage::Result<storage::Unit> Resize(std::uint32_t Width, std::uint32_t Height);
        storage::Result<storage::Unit> Focus();
        // Acceptance queues work; Details reports its completion sequence/error.
        storage::Result<std::uint64_t> SendCommand(int Command, int Parameter);
        storage::Result<std::uint64_t> Search(QString Text, int Flags);
    private:
        struct State;
        explicit HostedWlxSession(std::shared_ptr<State> State);
        std::shared_ptr<State> state_;
        friend class WlxViewerService;
    };
    class WlxViewerService final
    {
    public:
        using MaterializerFactory = std::function<storage::Result<std::shared_ptr<storage::IMaterializer>>()>;
        // Factory and every materializer/host operation run on a worker. The
        // owner drains IsIdle before destroying this service on the GUI thread.
        // Service methods use one owner thread; session methods are thread-safe.
        explicit WlxViewerService(MaterializerFactory Factory, std::size_t MaxSessions = 8);
        ~WlxViewerService();
        storage::Result<std::shared_ptr<HostedWlxSession>> Open(WlxViewerOptions Options,
            std::shared_ptr<storage::IContentSource> Source, QString ParentWindow,
            std::uint32_t Width, std::uint32_t Height, storage::OperationContext Context = {});
        storage::Result<std::shared_ptr<HostedWlxSession>> OpenAutomatic(std::vector<WlxViewerOptions> Candidates,
            std::shared_ptr<storage::IContentSource> Source, QString ParentWindow,
            std::uint32_t Width, std::uint32_t Height, WlxAutomaticPolicy Policy = {}, storage::OperationContext Context = {});
        void CancelAll();
        bool IsIdle() const;
        void Collect();
    private:
        struct Task;
        storage::Result<std::shared_ptr<HostedWlxSession>> open(std::vector<WlxViewerOptions> Candidates,
            std::optional<WlxAutomaticPolicy> Policy, std::shared_ptr<storage::IContentSource> Source,
            QString ParentWindow, std::uint32_t Width, std::uint32_t Height, storage::OperationContext Context);
        static void run(const std::shared_ptr<HostedWlxSession::State>& State, std::vector<WlxViewerOptions> Candidates,
            std::optional<WlxAutomaticPolicy> Policy,
            std::shared_ptr<storage::IContentSource> Source, QString ParentWindow,
            MaterializerFactory Factory, storage::OperationContext Context);
        MaterializerFactory factory_;
        std::size_t maxSessions_;
        std::vector<std::unique_ptr<Task>> tasks_;
    };
}
