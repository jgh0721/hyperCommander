#include "hostedWlxSession.hpp"
#include <QFileInfo>
#include <QUuid>
#include <condition_variable>
#include <deque>
#include <thread>

namespace hc::plugins
{
    using namespace storage;
    using namespace viewer;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"wlx"}}; }
    }
    struct HostedWlxSession::State
    {
        mutable std::mutex mutex_;
        std::condition_variable changed_;
        std::stop_source stop_;
        WlxSessionDetails details_;
        bool closed_ = false, cancelled_ = false, commands_ = false, focus_ = false;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> size_;
        struct Command { std::uint64_t sequence_; int command_, parameter_; };
        std::deque<Command> queue_;
        std::uint64_t nextCommand_ = 1;
        void stop(bool Close)
        {
            { std::lock_guard Lock(mutex_); closed_ |= Close; cancelled_ |= !Close; }
            stop_.request_stop(); changed_.notify_all();
        }
    };
    struct WlxViewerService::Task
    {
        std::shared_ptr<HostedWlxSession::State> state_;
        std::jthread worker_;
    };
    HostedWlxSession::HostedWlxSession(std::shared_ptr<State> State) : state_(std::move(State)) {}
    HostedWlxSession::~HostedWlxSession() { Close(); }
    ViewerSnapshot HostedWlxSession::Snapshot() const { std::lock_guard Lock(state_->mutex_); return state_->details_.viewer_; }
    WlxSessionDetails HostedWlxSession::Details() const { std::lock_guard Lock(state_->mutex_); return state_->details_; }
    std::vector<std::string> HostedWlxSession::Commands() const
    { std::lock_guard Lock(state_->mutex_); return state_->commands_ ? std::vector<std::string>{"copy", "wrap", "select-all", "set-percent"} : std::vector<std::string>{}; }
    void HostedWlxSession::Cancel() { state_->stop(false); }
    void HostedWlxSession::Close() { state_->stop(true); }
    Result<Unit> HostedWlxSession::Resize(std::uint32_t Width, std::uint32_t Height)
    {
        if (Width > 32768 || Height > 32768) return Invalid("host-viewer-size");
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.idle_) return Error{ErrorCategory::Cancelled, "viewer-closed", {"wlx"}};
        state_->size_ = {Width, Height}; state_->changed_.notify_one(); return Unit{};
    }
    Result<Unit> HostedWlxSession::Focus()
    {
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.idle_) return Error{ErrorCategory::Cancelled, "viewer-closed", {"wlx"}};
        state_->focus_ = true; state_->changed_.notify_one(); return Unit{};
    }
    Result<std::uint64_t> HostedWlxSession::SendCommand(int Command, int Parameter)
    {
        if (Command < 1 || Command > 4 || Parameter < 0 || Parameter > (Command == 4 ? 100 : 511)) return Invalid("host-viewer-command-fields");
        std::lock_guard Lock(state_->mutex_);
        if (state_->stop_.stop_requested() || state_->details_.viewer_.status_ != ViewerStatus::Ready) return Error{ErrorCategory::Unavailable, "viewer-not-ready", {"wlx"}};
        if (!state_->commands_) return Error{ErrorCategory::Unsupported, "host-viewer-command-unsupported", {"wlx"}};
        if (state_->queue_.size() >= 16) return Error{ErrorCategory::BudgetExceeded, "viewer-command-budget", {"wlx"}};
        const auto Sequence = state_->nextCommand_++; state_->queue_.push_back({Sequence, Command, Parameter});
        state_->changed_.notify_one(); return Sequence;
    }
    WlxViewerService::WlxViewerService(MaterializerFactory Factory, std::size_t MaxSessions)
        : factory_(std::move(Factory)), maxSessions_(MaxSessions) {}
    WlxViewerService::~WlxViewerService() { CancelAll(); tasks_.clear(); }
    Result<std::shared_ptr<HostedWlxSession>> WlxViewerService::Open(WlxViewerOptions Options,
        std::shared_ptr<IContentSource> Source, QString ParentWindow, std::uint32_t Width, std::uint32_t Height, OperationContext Context)
    {
        if (!factory_ || !Source || ParentWindow.isEmpty() || Width > 32768 || Height > 32768 || Options.flags_ < 0 || Options.flags_ > 511)
            return Invalid("invalid-wlx-session-request");
        if (!maxSessions_ || maxSessions_ > 16) return Invalid("invalid-wlx-session-budget");
        Collect(); if (tasks_.size() >= maxSessions_) return Error{ErrorCategory::BudgetExceeded, "viewer-session-budget", {"wlx"}};
        auto State = std::make_shared<HostedWlxSession::State>(); State->size_ = {Width, Height};
        State->details_.viewer_.id_ = QUuid::createUuid().toString(QUuid::Id128).toStdString();
        State->details_.viewer_.provider_ = "wlx." + Options.approval_.reviewed_.id_;
        auto Session = std::shared_ptr<HostedWlxSession>(new HostedWlxSession(State));
        auto Task = std::make_unique<WlxViewerService::Task>(); Task->state_ = State;
        // Retain the task before starting it: allocation failure while appending
        // must not join an already-running viewer that has never been cancelled.
        tasks_.push_back(std::move(Task));
        try
        {
            tasks_.back()->worker_ = std::jthread([State, Options = std::move(Options), Source = std::move(Source), ParentWindow = std::move(ParentWindow), Factory = factory_, Context]() mutable {
                run(State, std::move(Options), std::move(Source), std::move(ParentWindow), std::move(Factory), Context);
                std::lock_guard Lock(State->mutex_); State->details_.idle_ = true;
            });
        }
        catch (const std::system_error&) { tasks_.pop_back(); return Error{ErrorCategory::Unavailable, "viewer-worker-unavailable", {"wlx"}}; }
        return Session;
    }
    void WlxViewerService::CancelAll() { for (auto& Task : tasks_) Task->state_->stop(false); }
    bool WlxViewerService::IsIdle() const
    {
        for (const auto& Task : tasks_) { std::lock_guard Lock(Task->state_->mutex_); if (!Task->state_->details_.idle_) return false; }
        return true;
    }
    void WlxViewerService::Collect()
    {
        std::erase_if(tasks_, [](const auto& Task) { std::lock_guard Lock(Task->state_->mutex_); return Task->state_->details_.idle_; });
    }
    void WlxViewerService::run(const std::shared_ptr<HostedWlxSession::State>& State, WlxViewerOptions Options,
        std::shared_ptr<IContentSource> Source, QString ParentWindow, MaterializerFactory Factory, OperationContext Context)
    {
        std::stop_callback External(Context.stopToken_, [&] { State->stop(false); });
        Context.stopToken_ = State->stop_.get_token();
        std::shared_ptr<IMaterializer> Materializer;
        std::shared_ptr<IMaterializedFileLease> Lease;
        std::unique_ptr<PluginProcess> Host;
        const auto Work = [&]() -> Result<Unit> {
            if (const auto Stop = Context.Check({"wlx"})) return *Stop;
            if (!Options.approval_.approved_) return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"wlx"}};
            auto Inspected = InspectManifestFile(Options.manifestPath_, Context); if (!Inspected.HasValue()) return Inspected.Failure();
            if (Inspected.Value().manifest_.format_ != PluginFormat::Wlx || Options.approval_.reviewed_ != Inspected.Value().manifest_)
                return Error{ErrorCategory::AccessDenied, "plugin-review-required", {"wlx"}};
            if (Options.binaryPath_.isEmpty() || QFileInfo(Options.binaryPath_).canonicalFilePath().compare(Inspected.Value().binaryPath_, Qt::CaseInsensitive) != 0)
                return Error{ErrorCategory::AccessDenied, "plugin-configured-binary-mismatch", {"wlx"}};
            HostProfile Profile; Profile.formats_ = {PluginFormat::Wlx}; Profile.extensions_ = {Extension::Viewer}; Profile.supportsIsolation_ = true;
            auto Admission = Admit(Inspected.Value().manifest_, Options.approval_, Profile); if (!Admission.HasValue()) return Admission.Failure();
            const auto Entry = Source->Describe();
            { std::lock_guard Lock(State->mutex_); State->details_.viewer_.source_ = Entry; }
            if (Entry.directory_) return Error{ErrorCategory::Unsupported, "viewer-directory", {"wlx"}};
            auto Created = Factory(); if (!Created.HasValue()) return Created.Failure(); Materializer = std::move(Created.Value());
            if (!Materializer) return Invalid("viewer-materializer-required");
            auto Materialized = Materializer->Materialize({Source, {}, {}}, Context).get();
            if (!Materialized.HasValue()) return Materialized.Failure(); Lease = std::move(Materialized.Value());
            if (!Lease) return Invalid("viewer-content-lease-required");
            auto Started = PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_, Context);
            if (!Started.HasValue()) return Started.Failure(); Host = std::move(Started.Value());
            const auto StartedState = Host->Snapshot();
            { std::lock_guard Lock(State->mutex_); State->details_.host_ = StartedState; }
            auto Probe = Host->Probe(Context); if (!Probe.HasValue()) return Probe.Failure();
            std::pair<std::uint32_t, std::uint32_t> Size;
            { std::lock_guard Lock(State->mutex_); Size = *State->size_; State->size_.reset(); }
            auto Opened = Host->ViewerCall(WireKind::ViewerOpen, {{"parent", ParentWindow}, {"file", QString::fromStdString(Lease->LocalPath())},
                {"width", static_cast<int>(Size.first)}, {"height", static_cast<int>(Size.second)}, {"flags", Options.flags_}}, Context);
            if (!Opened.HasValue()) return Opened.Failure();
            auto HostState = Host->Snapshot();
            {
                std::lock_guard Lock(State->mutex_); State->details_.host_ = HostState;
                State->details_.window_ = Opened.Value()["window"].toString(); State->details_.child_ = Opened.Value()["viewer"].toString();
                State->details_.viewer_.bytesRead_ = Lease->Describe().size_; State->details_.viewer_.status_ = ViewerStatus::Ready;
                State->commands_ = Probe.Value()["commands"].toBool();
            }
            OperationContext Calls; Calls.stopToken_ = Context.stopToken_;
            while (!State->stop_.stop_requested())
            {
                std::optional<std::pair<std::uint32_t, std::uint32_t>> Resize;
                std::optional<HostedWlxSession::State::Command> Command; bool Focus = false;
                {
                    std::unique_lock Lock(State->mutex_);
                    State->changed_.wait_for(Lock, std::chrono::milliseconds(50), [&] { return State->stop_.stop_requested() || State->size_ || State->focus_ || !State->queue_.empty(); });
                    if (State->stop_.stop_requested()) break;
                    Resize = std::exchange(State->size_, {}); Focus = std::exchange(State->focus_, false);
                    if (!State->queue_.empty()) { Command = State->queue_.front(); State->queue_.pop_front(); }
                }
                if (Resize)
                {
                    auto Result = Host->ViewerCall(WireKind::ViewerResize, {{"width", static_cast<int>(Resize->first)}, {"height", static_cast<int>(Resize->second)}}, Calls);
                    if (!Result.HasValue()) return Result.Failure();
                }
                if (Focus) { auto Result = Host->ViewerCall(WireKind::ViewerFocus, {}, Calls); if (!Result.HasValue()) return Result.Failure(); }
                if (Command)
                {
                    auto Result = Host->ViewerCall(WireKind::ViewerCommand, {{"command", Command->command_}, {"parameter", Command->parameter_}}, Calls);
                    std::lock_guard Lock(State->mutex_); State->details_.commandSequence_ = Command->sequence_;
                    State->details_.commandError_ = Result.HasValue() ? std::optional<Error>{} : Result.Failure();
                    if (Result.HasValue() && Result.Value()["result"].toInt(-1) != 0) State->details_.commandError_ = Error{ErrorCategory::Unsupported, "host-viewer-command-rejected", {"wlx"}};
                }
                HostState = Host->Snapshot();
                { std::lock_guard Lock(State->mutex_); State->details_.host_ = HostState; }
                if (!HostState.running_) return HostState.failure_.value_or(Error{ErrorCategory::Unavailable, "host-exited", {"wlx"}});
            }
            return Unit{};
        };
        std::optional<Error> Failure;
        try { auto Result = Work(); if (!Result.HasValue()) Failure = Result.Failure(); }
        catch (const std::exception&) { Failure = Error{ErrorCategory::Internal, "viewer-worker-exception", {"wlx"}}; }
        catch (...) { Failure = Error{ErrorCategory::Internal, "viewer-worker-exception", {"wlx"}}; }
        // Never release a materialized file while a plugin can still read it.
        if (Host)
        {
            Host->Close(); auto FinalHost = Host->Snapshot();
            { std::lock_guard Lock(State->mutex_); State->details_.host_ = FinalHost; }
            Host.reset();
        }
        Lease.reset(); Source.reset();
        if (Materializer)
        {
            try
            {
                auto Cleanup = Materializer->DrainCleanup();
                if (!Failure && !Cleanup.errors_.empty()) Failure = Cleanup.errors_.front();
            }
            catch (...) { if (!Failure) Failure = Error{ErrorCategory::Internal, "viewer-cleanup-exception", {"wlx"}}; }
            Materializer.reset();
        }
        {
            std::lock_guard Lock(State->mutex_); auto& Snapshot = State->details_.viewer_;
            Snapshot.error_ = Failure; State->commands_ = false; State->queue_.clear();
            Snapshot.status_ = State->closed_ ? ViewerStatus::Closed : State->cancelled_ ? ViewerStatus::Cancelled : Failure ? ViewerStatus::Failed : ViewerStatus::Closed;
        }
    }
}
