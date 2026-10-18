#pragma once
#include "pluginProcess.hpp"
#include "commonLibs/viewer/viewer.hpp"
#include "commonLibs/storage/materializedContent.hpp"
#include <functional>
#include "commonLibs/storage/writableStorage.hpp"

namespace hc::plugins
{
    struct PluginViewerOptions
    {
        QString executable_, manifestPath_, binaryPath_, defaultIniPath_;
        PluginApproval approval_;
        HostLimits limits_;
        QJsonValue applicationRule_;
        std::shared_ptr<storage::IWritableStorage> writer_;
        int flags_ = 0;
        viewer::ViewerPurpose purpose_ = viewer::ViewerPurpose::FullViewer;
    };
    struct PluginViewerDetails
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
        bool documentChannel_ = false;
        QJsonObject documentState_, documentReply_;
    };
    class PluginViewerService;
    struct PluginViewerSelectionPolicy
    {
        bool force_ = false;
        // Only MULTIMEDIA candidates override supported builtin multimedia
        // unless the user explicitly requests forced plugin selection.
        bool builtinMultimedia_ = false;
        // Optional worker-only content probe supplied by the builtin adapter.
        std::function<bool(const storage::EntrySnapshot&, std::span<const std::byte>)> builtinProbe_;
    };
    class HostedViewerSession final : public viewer::IViewerSession
    {
    public:
        ~HostedViewerSession() override;
        viewer::ViewerSnapshot Snapshot() const override;
        // The native presentation is owned by the host, not a decoded document.
        std::shared_ptr<const viewer::ViewerDocument> Document() const override { return {}; }
        std::vector<std::string> Commands() const override;
        void Cancel() override;
        void Close() override;
        PluginViewerDetails Details() const;
        storage::Result<storage::Unit> Resize(std::uint32_t Width, std::uint32_t Height);
        storage::Result<storage::Unit> Focus();
        // Acceptance queues work; Details reports its completion sequence/error.
        storage::Result<std::uint64_t> SendCommand(int Command, int Parameter);
        storage::Result<std::uint64_t> Search(QString Text, int Flags);
        storage::Result<std::uint64_t> SendDocument(QJsonObject Request);
        struct DocumentInput { std::shared_ptr<storage::IContentSource> source_; std::shared_ptr<storage::IWritableStorage> writer_; };
        storage::Result<std::uint64_t> OpenDocuments(std::vector<DocumentInput> Inputs, bool ReplacePrepared = false);
    private:
        struct State;
        explicit HostedViewerSession(std::shared_ptr<State> State);
        std::shared_ptr<State> state_;
        friend class PluginViewerService;
    };
    class PluginViewerService final
    {
    public:
        using MaterializerFactory = std::function<storage::Result<std::shared_ptr<storage::IMaterializer>>()>;
        // Factory and every materializer/host operation run on a worker. The
        // owner drains IsIdle before destroying this service on the GUI thread.
        // Service methods use one owner thread; session methods are thread-safe.
        explicit PluginViewerService(MaterializerFactory Factory, std::size_t MaxSessions = 8);
        ~PluginViewerService();
        storage::Result<std::shared_ptr<HostedViewerSession>> Open(PluginViewerOptions Options,
            std::shared_ptr<storage::IContentSource> Source, QString ParentWindow,
            std::uint32_t Width, std::uint32_t Height, storage::OperationContext Context = {});
        storage::Result<std::shared_ptr<HostedViewerSession>> OpenAutomatic(std::vector<PluginViewerOptions> Candidates,
            std::shared_ptr<storage::IContentSource> Source, QString ParentWindow,
            std::uint32_t Width, std::uint32_t Height, PluginViewerSelectionPolicy Policy = {}, storage::OperationContext Context = {});
        void CancelAll();
        bool IsIdle() const;
        void Collect();
    private:
        struct Task;
        storage::Result<std::shared_ptr<HostedViewerSession>> open(std::vector<PluginViewerOptions> Candidates,
            std::optional<PluginViewerSelectionPolicy> Policy, std::shared_ptr<storage::IContentSource> Source,
            QString ParentWindow, std::uint32_t Width, std::uint32_t Height, storage::OperationContext Context);
        static void run(const std::shared_ptr<HostedViewerSession::State>& State, std::vector<PluginViewerOptions> Candidates,
            std::optional<PluginViewerSelectionPolicy> Policy,
            std::shared_ptr<storage::IContentSource> Source, QString ParentWindow,
            MaterializerFactory Factory, storage::OperationContext Context);
        MaterializerFactory factory_;
        std::size_t maxSessions_;
        std::vector<std::unique_ptr<Task>> tasks_;
    };
}
